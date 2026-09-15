/****************************************************************************
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <sfconfig.h>

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <semaphore.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/board.h>
#include <nuttx/kmalloc.h>
#include <nuttx/spi/spi.h>
#include <nuttx/lcd/lcd.h>
#include <nuttx/lcd/lcd_dev.h>
#include <nuttx/lcd/memlcd.h>
#include <nuttx/nuttx.h>
#include <nuttx/cache.h>
#include <nuttx/video/fb.h>

#include "chip.h"
#include "arm_internal.h"


#include "drv_io.h"
#include "sf32lb_lcd.h"

/* Force linker to pull in LCD driver objects from static library.
 * LCD_DRIVER_EXPORT places descriptors in the LcdDriverDescTab section,
 * but the linker will not extract unreferenced .o files from .a archives.
 * These extern references ensure the driver objects are linked in.
 */

#ifdef CONFIG_LCD_USING_CO5300
extern const lcd_drv_desc_t __lcddriver_co5300;
const void *_lcd_drv_ref_co5300
  __attribute__((used, section(".rodata"))) = &__lcddriver_co5300;
#endif

#ifdef CONFIG_LCD_USING_ILI8688E
extern const lcd_drv_desc_t __lcddriver_ili8688e;
const void *_lcd_drv_ref_ili8688e
  __attribute__((used, section(".rodata"))) = &__lcddriver_ili8688e;
#endif
/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/
#define lcd_debug_print lcdinfo

struct sf32lb_lcd_dev_s
{
    struct lcd_dev_s dev;
    LCDC_HandleTypeDef hlcdc;
    lcd_drv_desc_t *p_drv_ops;
    uint16_t buf_format;
    HAL_LCDC_LayerDef select_layer;

    FAR sem_t init_sem;
    FAR sem_t draw_sem;

    int power;
    uint8_t bpp;
};

/* Configuration ************************************************************/

/****************************************************************************
 * Private Data
 ****************************************************************************/
static struct sf32lb_lcd_dev_s s_drv_lcd;
static volatile bool s_fb_registering;
static volatile bool s_lcd_hw_ready;

/* ---------------------------------------------------------------------------
 * v39 bring-up instrumentation
 *
 * Panel command/register traffic is already proven good (ReadID and the
 * 0x3A/0x51/0x52 reads all come back correct), so the open question is the
 * *pixel* path the UI actually uses:
 *
 *   LVGL -> /dev/lcd0 -> sf32lb_lcd_putarea -> sf32lb_lcd_wrram
 *        -> HAL_LCDC_SendLayerData2Reg_IT -> LCDC completion IRQ -> draw_sem
 *
 * Two things are unknown, and both are answered here without a debugger:
 *   1. can that path put a picture on the glass at all (lcd_v39_itvis below)?
 *   2. does the UI really reach it in normal operation, and do the
 *      interrupt-driven transfers ever complete (counters + heartbeat)?
 * ------------------------------------------------------------------------- */
static volatile uint32_t g_pa_calls;     /* sf32lb_lcd_putarea entries      */
static volatile uint32_t g_dropped;      /* putarea calls silently dropped  */
static volatile uint32_t g_wr_calls;     /* sf32lb_lcd_wrram entries        */
static volatile uint32_t g_wr_done;      /* transfers that completed        */
static volatile uint32_t g_wr_timeouts;  /* draw_sem waits that expired     */
static volatile uint32_t g_wr_kb;        /* KiB pushed to the panel         */
static volatile uint16_t g_wr_w;         /* last region width               */
static volatile uint16_t g_wr_h;         /* last region height              */
static volatile bool     s_panel_inited; /* panel program done once         */
static sem_t             s_wr_lock;      /* serialises sf32lb_lcd_wrram      */

#define LCD_V39_W 390
#define LCD_V39_BAND 8

static void sf32lb_lcd_wrram(FAR struct sf32lb_lcd_dev_s *dev,
                             FAR const uint8_t *buffer,
                             uint16_t x0, uint16_t y0,
                             uint16_t x1, uint16_t y1);

/* Full-screen solid colours followed by a sweeping bar, every pixel pushed
 * through sf32lb_lcd_wrram() - byte for byte the same call LVGL makes, via
 * the interrupt-driven HAL entry point.  Runs before the UI starts, so the
 * glass is exclusively ours: whatever appears (or does not) is a clean
 * verdict on the write path, with no readback guesswork involved. */
static void lcd_v39_itvis(void)
{
    static uint16_t band[LCD_V39_W * LCD_V39_BAND];
    static const uint16_t col[4] = { 0xFFFFu, 0xF800u, 0x07E0u, 0x001Fu };
    static const char *nm[4] = { "white", "red", "green", "blue" };
    uint16_t w = (uint16_t)s_drv_lcd.p_drv_ops->lcd_horizonal_res;
    uint16_t h = (uint16_t)s_drv_lcd.p_drv_ops->lcd_vertical_res;
    int ci;
    int strip;
    int f;
    int k;
    int y;

    if (w == 0 || w > LCD_V39_W)
    {
        w = LCD_V39_W;
    }

    syslog(LOG_ERR, "[lcd][itvis] start: LVGL write path, %ux%u, band=%d\n",
           (unsigned)w, (unsigned)h, LCD_V39_BAND);

    for (ci = 0; ci < 4; ci++)
    {
        for (k = 0; k < LCD_V39_W * LCD_V39_BAND; k++)
        {
            band[k] = col[ci];
        }

        for (y = 0; y < (int)h; y += LCD_V39_BAND)
        {
            uint16_t bh = (uint16_t)(((h - (uint16_t)y) < LCD_V39_BAND) ?
                                     (h - (uint16_t)y) : LCD_V39_BAND);
            sf32lb_lcd_wrram(&s_drv_lcd, (uint8_t *)band, 0, (uint16_t)y,
                             (uint16_t)(w - 1), (uint16_t)(y + bh - 1));
        }

        syslog(LOG_ERR,
               "[lcd][itvis] %s pushed (wrram=%lu done=%lu timeout=%lu)\n",
               nm[ci], (unsigned long)g_wr_calls, (unsigned long)g_wr_done,
               (unsigned long)g_wr_timeouts);
        usleep(200 * 1000);
    }

    /* A moving bar on a black field: repeated transfers plus motion, which a
     * single stuck frame cannot fake. */
    for (f = 0; f < 6; f++)
    {
        int target = (f * 55) / 6;
        int strips = ((int)h + LCD_V39_BAND - 1) / LCD_V39_BAND;

        for (strip = 0; strip < strips; strip++)
        {
            uint16_t c = (strip >= target && strip <= target + 1) ?
                         0xFFFFu : 0x0000u;
            int yy = strip * LCD_V39_BAND;
            uint16_t bh = (uint16_t)(((h - (uint16_t)yy) < LCD_V39_BAND) ?
                                     (h - (uint16_t)yy) : LCD_V39_BAND);

            for (k = 0; k < LCD_V39_W * LCD_V39_BAND; k++)
            {
                band[k] = c;
            }

            sf32lb_lcd_wrram(&s_drv_lcd, (uint8_t *)band, 0, (uint16_t)yy,
                             (uint16_t)(w - 1), (uint16_t)(yy + bh - 1));
        }

        usleep(120 * 1000);
    }

    syslog(LOG_ERR,
           "[lcd][itvis] done: wrram=%lu done=%lu timeout=%lu kb=%lu\n",
           (unsigned long)g_wr_calls, (unsigned long)g_wr_done,
           (unsigned long)g_wr_timeouts, (unsigned long)g_wr_kb);
}

/* Runs the visual test off the init thread so lcddev_register is never queued
 * behind it.  See the call site for why that ordering matters. */
static int lcd_v39_itvis_thread_entry(int argc, FAR char *argv[])
{
    (void)argc;
    (void)argv;

    lcd_v39_itvis();
    return 0;
}

/* Keeps the UART log self-describing: from this alone it is visible whether
 * the UI is still pushing frames, and whether those transfers complete. */
static int lcd_v39_hb_thread_entry(int argc, FAR char *argv[])
{
    uint32_t pc = 0;
    uint32_t wc = 0;
    uint32_t dc = 0;
    uint32_t tc = 0;
    int i;

    for (i = 0; i < 240; i++)   /* ~20 min at 5 s intervals */
    {
        usleep(5 * 1000 * 1000);

        syslog(LOG_ERR,
               "[lcd][hb] t=%ds putarea=%lu(+%lu) drop=%lu wrram=%lu(+%lu) "
               "done=%lu(+%lu) timeout=%lu(+%lu) kb=%lu last=%ux%u\n",
               (i + 1) * 5,
               (unsigned long)g_pa_calls, (unsigned long)(g_pa_calls - pc),
               (unsigned long)g_dropped,
               (unsigned long)g_wr_calls, (unsigned long)(g_wr_calls - wc),
               (unsigned long)g_wr_done, (unsigned long)(g_wr_done - dc),
               (unsigned long)g_wr_timeouts, (unsigned long)(g_wr_timeouts - tc),
               (unsigned long)g_wr_kb,
               (unsigned)g_wr_w, (unsigned)g_wr_h);

        pc = g_pa_calls;
        wc = g_wr_calls;
        dc = g_wr_done;
        tc = g_wr_timeouts;
    }

    return 0;
}

static void sf32lb_lcd_ensure_display_on(FAR struct sf32lb_lcd_dev_s *dev)
{
  if (dev == NULL || dev->p_drv_ops == NULL || dev->p_drv_ops->p_ops == NULL)
    {
      return;
    }

  if (dev->power > 0)
    {
      return;
    }

  if (dev->p_drv_ops->p_ops->DisplayOn != NULL)
    {
      dev->p_drv_ops->p_ops->DisplayOn(&dev->hlcdc);
      dev->power = CONFIG_LCD_MAXPOWER;
    }
}

/****************************************************************************
 * Private Functions
 ****************************************************************************/
static lcd_drv_desc_t *find_right_driver(void)
{

#ifdef CONFIG_LCD_USING_CO5300
  lcdinfo("Use configured lcd driver: co5300");
  return (lcd_drv_desc_t *)&__lcddriver_co5300;
#endif

#ifdef CONFIG_LCD_USING_ILI8688E
  lcdinfo("Use configured lcd driver: ili8688e");
  return (lcd_drv_desc_t *)&__lcddriver_ili8688e;
#endif

    lcd_drv_desc_t *table_begin = NULL;
    lcd_drv_desc_t *table_end = NULL;
    lcd_drv_desc_t *p_drv_desc = NULL;

#if defined(__CC_ARM) || (defined (__ARMCC_VERSION) && (__ARMCC_VERSION >= 6010050))                                 /* ARM C Compiler */
    extern const int LcdDriverDescTab$$Base;
    extern const int LcdDriverDescTab$$Limit;
    table_begin = (lcd_drv_desc_t *) &LcdDriverDescTab$$Base;
    table_end = (lcd_drv_desc_t *)   &LcdDriverDescTab$$Limit;
#elif defined (__ICCARM__) || defined(__ICCRX__)      /* for IAR Compiler */
#error "tobe contribute"
#elif defined (__GNUC__)                              /* for GCC Compiler */
    extern const int LcdDriverDescTab_start;
    extern const int LcdDriverDescTab_end;
    table_begin = (lcd_drv_desc_t *)&LcdDriverDescTab_start;
    table_end = (lcd_drv_desc_t *) &LcdDriverDescTab_end;
#endif /* defined(__CC_ARM) */

    if ((NULL == table_begin) || (NULL == table_end) || (table_begin == table_end))
    {
        lcdwarn("No LCD driver registered!");
        return NULL;

    }

#ifndef LCD_MISSING
    for (p_drv_desc = table_begin; p_drv_desc < table_end; p_drv_desc++)
    {
        if ((p_drv_desc->p_ops != NULL) && (p_drv_desc->p_init_cfg != NULL))
        {
            if (p_drv_desc->p_ops->ReadID != NULL)
            {
                uint32_t id;

                if (p_drv_desc->p_ops->Init != NULL)
                    p_drv_desc->p_ops->Init(&s_drv_lcd.hlcdc);

                id = p_drv_desc->p_ops->ReadID(&s_drv_lcd.hlcdc);

                if (p_drv_desc->id == id)
                {
                    lcdinfo("Found lcd %s id:%lxh", p_drv_desc->name, id);
                    return p_drv_desc;
                }
                else
                {
                    lcdinfo("Try lcd %s, read id:%lxh, expect:%lxh", p_drv_desc->name, id, p_drv_desc->id);
                }
            }
        }
    }
#endif
    lcdwarn("unknow lcd!");
    return NULL;
}

static void sf32lb_lcd_setarea(FAR struct sf32lb_lcd_dev_s *dev,
                           uint16_t x0, uint16_t y0,
                           uint16_t x1, uint16_t y1)
{
	if (dev && dev->p_drv_ops && dev->p_drv_ops->p_ops 
		   && dev->p_drv_ops->p_ops->SetRegion)
    {
        int new_x0, new_x1, new_y0, new_y1;


        //disable_low_power(&drv_lcd);

        lcd_debug_print("set_window [%d,%d,%d,%d]", x0, y0, x1, y1);
        new_x0 = x0;
        new_x1 = x1;
        new_y0 = y0;
        new_y1 = y1;


        DEBUGASSERT((new_x0 <= new_x1) && (new_y0 <= new_y1));
        DEBUGASSERT((new_x1 - new_x0 + 1) <= dev->p_drv_ops->lcd_horizonal_res);
        DEBUGASSERT((new_y1 - new_y0 + 1) <= dev->p_drv_ops->lcd_vertical_res);

        dev->p_drv_ops->p_ops->SetRegion(&dev->hlcdc, new_x0, new_y0, new_x1, new_y1);
        //enable_low_power(&drv_lcd);

    }
}
static void SendLayerDataCpltCbk(LCDC_HandleTypeDef *lcdc)
{

   //lcdinfo("SendLayerDataCpltCbk \r\n");

   if (lcdc->XferCpltCallback != NULL)
   {
       lcdc->XferCpltCallback = NULL;


       struct sf32lb_lcd_dev_s *p_drvlcd = container_of(lcdc, struct sf32lb_lcd_dev_s, hlcdc);
       sem_post(&p_drvlcd->draw_sem);
   }

}

static void SendLayerDataErrCbk(LCDC_HandleTypeDef *lcdc)
{
    lcdinfo("SendLayerDataErrCbk \r\n");
}


static void sf32lb_lcd_wrram(FAR struct sf32lb_lcd_dev_s *dev, FAR const uint8_t *buffer,
                          uint16_t x0, uint16_t y0,
                          uint16_t x1, uint16_t y1)
{
   if (dev && dev->p_drv_ops && dev->p_drv_ops->p_ops 
   		  && dev->p_drv_ops->p_ops->WriteMultiplePixels)
   {
       uint16_t new_x0, new_x1, new_y0, new_y1;
  size_t pixels;
  size_t xfer_bytes;

       DEBUGASSERT((x0 <= x1) && (y0 <= y1));

       //disable_low_power(&drv_lcd);

       lcd_debug_print("sf32lb_lcd_wrram [%d,%d,%d,%d]", x0, y0, x1, y1);
       new_x0 = x0;
       new_x1 = x1;
       new_y0 = y0;
       new_y1 = y1;


       DEBUGASSERT((new_x0 <= new_x1) && (new_y0 <= new_y1));
       DEBUGASSERT((new_x1 - new_x0 + 1) <= dev->p_drv_ops->lcd_horizonal_res);
       DEBUGASSERT((new_y1 - new_y0 + 1) <= dev->p_drv_ops->lcd_vertical_res);

        /* Ensure DMA reads the latest pixel data from memory. */

        pixels = (size_t)(new_x1 - new_x0 + 1) * (size_t)(new_y1 - new_y0 + 1);
        xfer_bytes = pixels * ((size_t)dev->bpp >> 3);
        if (buffer != NULL && xfer_bytes > 0)
        {
          up_clean_dcache((uintptr_t)buffer, (uintptr_t)buffer + xfer_bytes);
        }

        g_wr_calls++;
        g_wr_kb += (uint32_t)(xfer_bytes >> 10);
        g_wr_w = (uint16_t)(new_x1 - new_x0 + 1);
        g_wr_h = (uint16_t)(new_y1 - new_y0 + 1);
        if (g_wr_calls == 1)
          {
            syslog(LOG_ERR,
                   "[lcd][wr] first write region=[%d,%d,%d,%d] bytes=%u bpp=%u\n",
                   x0, y0, x1, y1, (unsigned)xfer_bytes, (unsigned)dev->bpp);
          }
       
        /* 写路径串行化。draw_sem 是「整机唯一一个完成信号量」，两个写者同时在跑
         * 时后到的那个会把别人等的令牌 drain 掉 —— v40 实测 itvis 与 LVGL 重叠
         * 时出现 7 次 200ms 超时（errno=110）。生产上只有 LVGL 一条线程在写，
         * 但把它锁上才不依赖这个假设。 */
        sem_wait(&s_wr_lock);

        /* Drain stale completion tokens before starting a new transfer. */
        while (sem_trywait(&(dev->draw_sem)) == 0)
        {
        }

        dev->hlcdc.XferCpltCallback = SendLayerDataCpltCbk;
        dev->hlcdc.XferErrorCallback = SendLayerDataErrCbk;
        dev->hlcdc.debug_cnt0++;


        dev->p_drv_ops->p_ops->WriteMultiplePixels(&dev->hlcdc, buffer, new_x0, new_y0, new_x1, new_y1);
        //enable_low_power(&drv_lcd);
        /* --------- Wait send complete (bounded wait) -----------------*/
        {
          struct timespec ts;
          clock_gettime(CLOCK_REALTIME, &ts);
          ts.tv_nsec += 200 * 1000 * 1000; /* 200ms */
          if (ts.tv_nsec >= 1000000000L)
          {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
          }

          if (sem_timedwait(&(dev->draw_sem), &ts) < 0)
          {
            g_wr_timeouts++;
            if (g_wr_timeouts <= 10 || (g_wr_timeouts % 100) == 0)
              {
                syslog(LOG_ERR,
                       "[lcd][wr] xfer TIMEOUT #%lu errno=%d region=[%d,%d,%d,%d]"
                       " bytes=%u\n",
                       (unsigned long)g_wr_timeouts, errno, x0, y0, x1, y1,
                       (unsigned)xfer_bytes);
              }
            else
              {
                lcdwarn("lcd xfer wait timeout: %d", errno);
              }
          }
          else
          {
            g_wr_done++;
          }
        }

        /* xfer finished (or timed out) - hand the write lock to the next writer.
         * Released on both paths on purpose: a timeout must not wedge the LCD
         * for the rest of the boot. */
        sem_post(&s_wr_lock);
   }
}


                           

/****************************************************************************
 * Name:  sf32lb_lcd_putrun
 *
 * Description:
 *   This method can be used to write a partial raster line to the LCD:
 *
 *   dev     - The lcd device
 *   row     - Starting row to write to (range: 0 <= row < yres)
 *   col     - Starting column to write to (range: 0 <= col <= xres-npixels)
 *   buffer  - The buffer containing the run to be written to the LCD
 *   npixels - The number of pixels to write to the LCD
 *             (range: 0 < npixels <= xres-col)
 *
 ****************************************************************************/

static int sf32lb_lcd_putrun(FAR struct lcd_dev_s *dev,
                         fb_coord_t row, fb_coord_t col,
                         FAR const uint8_t *buffer, size_t npixels)
{
  FAR struct sf32lb_lcd_dev_s *priv = (FAR struct sf32lb_lcd_dev_s *)dev;

  if (s_fb_registering || !s_lcd_hw_ready)
    {
      return OK;
    }

  lcd_debug_print("row: %d col: %d npixels: %d\n", row, col, npixels);
  DEBUGASSERT(buffer && ((uintptr_t)buffer & 1) == 0);

  if (priv->bpp == 8)
    {
      FAR uint16_t *conv;
      size_t i;

      conv = kmm_malloc(npixels * sizeof(uint16_t));
      if (conv == NULL)
        {
          return -ENOMEM;
        }

      for (i = 0; i < npixels; i++)
        {
          uint8_t v = buffer[i];
          uint8_t r = (v >> 5) & 0x07;
          uint8_t g = (v >> 2) & 0x07;
          uint8_t b = v & 0x03;

          conv[i] = (uint16_t)((((uint16_t)r * 31 / 7) << 11) |
                               (((uint16_t)g * 63 / 7) << 5) |
                               (((uint16_t)b * 31 / 3) << 0));
        }

      sf32lb_lcd_setarea(priv, col, row, col + npixels - 1, row);
      sf32lb_lcd_wrram(priv, (FAR const uint8_t *)conv,
                       col, row, col + npixels - 1, row);
      sf32lb_lcd_ensure_display_on(priv);
      kmm_free(conv);
      return OK;
    }

  sf32lb_lcd_setarea(priv, col, row, col + npixels - 1, row);
  sf32lb_lcd_wrram(priv, buffer, col, row, col + npixels - 1, row);
  sf32lb_lcd_ensure_display_on(priv);

  return OK;
}

/****************************************************************************
 * Name:  sf32lb_lcd_putarea
 *
 * Description:
 *   This method can be used to write a partial area to the LCD:
 *
 *   dev       - The lcd device
 *   row_start - Starting row to write to (range: 0 <= row < yres)
 *   row_end   - Ending row to write to (range: row_start <= row < yres)
 *   col_start - Starting column to write to (range: 0 <= col <= xres)
 *   col_end   - Ending column to write to
 *               (range: col_start <= col_end < xres)
 *   buffer    - The buffer containing the area to be written to the LCD
 *   stride    - Length of a line in bytes. This parameter may be necessary
 *               to allow the LCD driver to calculate the offset for partial
 *               writes when the buffer needs to be splited for row-by-row
 *               writing.
 *
 ****************************************************************************/

static int sf32lb_lcd_putarea(FAR struct lcd_dev_s *dev,
                          fb_coord_t row_start, fb_coord_t row_end,
                          fb_coord_t col_start, fb_coord_t col_end,
                          FAR const uint8_t *buffer, fb_coord_t stride)
{
  FAR struct sf32lb_lcd_dev_s *priv = (FAR struct sf32lb_lcd_dev_s *)dev;
  size_t bytes_per_pixel;
  size_t row_bytes;

  if (s_fb_registering || !s_lcd_hw_ready)
    {
      /* Silence here is what makes "black screen" so hard to read: the caller
       * is told the write succeeded, so LVGL marks the area clean and never
       * repaints it.  Count them and shout once. */
      g_dropped++;
      if (g_dropped == 1)
        {
          syslog(LOG_ERR,
                 "[lcd][pa] DROP #1 rows=%d..%d cols=%d..%d hw_ready=%d "
                 "fb_reg=%d - frame discarded, panel never saw it\n",
                 row_start, row_end, col_start, col_end,
                 (int)s_lcd_hw_ready, (int)s_fb_registering);
        }
      return OK;
    }

  g_pa_calls++;
  if (g_pa_calls == 1)
    {
      syslog(LOG_ERR,
             "[lcd][pa] first putarea rows=%d..%d cols=%d..%d bpp=%u stride=%d\n",
             row_start, row_end, col_start, col_end,
             (unsigned)priv->bpp, stride);
    }

  bytes_per_pixel = priv->bpp >> 3;
  row_bytes = (size_t)(col_end - col_start + 1) * bytes_per_pixel;

  lcd_debug_print("row_start: %d row_end: %d col_start: %d col_end: %d\n",
         row_start, row_end, col_start, col_end);

  DEBUGASSERT(buffer && ((uintptr_t)buffer & 1) == 0);

  if (priv->bpp == 8)
    {
      fb_coord_t y;
      fb_coord_t width = col_end - col_start + 1;
      FAR uint16_t *conv = kmm_malloc(width * sizeof(uint16_t));

      if (conv == NULL)
        {
          return -ENOMEM;
        }

      for (y = row_start; y <= row_end; y++)
        {
          FAR const uint8_t *src = buffer + (y - row_start) * stride;
          fb_coord_t x;

          for (x = 0; x < width; x++)
            {
              uint8_t v = src[x];
              uint8_t r = (v >> 5) & 0x07;
              uint8_t g = (v >> 2) & 0x07;
              uint8_t b = v & 0x03;

              conv[x] = (uint16_t)((((uint16_t)r * 31 / 7) << 11) |
                                   (((uint16_t)g * 63 / 7) << 5) |
                                   (((uint16_t)b * 31 / 3) << 0));
            }

          sf32lb_lcd_setarea(priv, col_start, y, col_end, y);
          sf32lb_lcd_wrram(priv, (FAR const uint8_t *)conv,
                           col_start, y, col_end, y);
        }

      sf32lb_lcd_ensure_display_on(priv);
      kmm_free(conv);
      return OK;
    }

  if ((size_t)stride == row_bytes)
    {
      fb_coord_t y = row_start;
      const fb_coord_t chunk_rows = 24;

      while (y <= row_end)
        {
          fb_coord_t y1 = y + chunk_rows - 1;
          FAR const uint8_t *src;

          if (y1 > row_end)
            {
              y1 = row_end;
            }

          src = buffer + (y - row_start) * stride;
          sf32lb_lcd_setarea(priv, col_start, y, col_end, y1);
          sf32lb_lcd_wrram(priv, src, col_start, y, col_end, y1);
          y = y1 + 1;
        }
    }
  else
    {
      fb_coord_t y;

      /* The source rows are not tightly packed for this area, so send
       * one row at a time using stride to step through the source buffer.
       */

      for (y = row_start; y <= row_end; y++)
        {
          FAR const uint8_t *src = buffer + (y - row_start) * stride;

          sf32lb_lcd_setarea(priv, col_start, y, col_end, y);
          sf32lb_lcd_wrram(priv, src, col_start, y, col_end, y);
        }
    }

  sf32lb_lcd_ensure_display_on(priv);

  return OK;
}

/****************************************************************************
 * Name:  sf32lb_lcd_getrun
 *
 * Description:
 *   This method can be used to read a partial raster line from the LCD:
 *
 *  dev     - The lcd device
 *  row     - Starting row to read from (range: 0 <= row < yres)
 *  col     - Starting column to read read (range: 0 <= col <= xres-npixels)
 *  buffer  - The buffer in which to return the run read from the LCD
 *  npixels - The number of pixels to read from the LCD
 *            (range: 0 < npixels <= xres-col)
 *
 ****************************************************************************/

#ifndef CONFIG_LCD_NOGETRUN
static int sf32lb_lcd_getrun(FAR struct lcd_dev_s *dev,
                         fb_coord_t row, fb_coord_t col,
                         FAR uint8_t *buffer, size_t npixels)
{
  FAR struct sf32lb_lcd_dev_s *priv = (FAR struct sf32lb_lcd_dev_s *)dev;
  //FAR uint16_t *dest = (FAR uint16_t *)buffer;

  lcdinfo("row: %d col: %d npixels: %d\n", row, col, npixels);
  DEBUGASSERT(buffer && ((uintptr_t)buffer & 1) == 0);

  sf32lb_lcd_setarea(priv, col, row, col + npixels - 1, row);
  //sf32lb_lcd_rdram(priv, dest, npixels);
  DEBUGASSERT(0);
  return OK;
}
#endif

/****************************************************************************
 * Name:  sf32lb_lcd_getvideoinfo
 *
 * Description:
 *   Get information about the LCD video controller configuration.
 *
 ****************************************************************************/

static int sf32lb_lcd_getvideoinfo(FAR struct lcd_dev_s *dev,
                               FAR struct fb_videoinfo_s *vinfo)
{
  DEBUGASSERT(dev && vinfo);

  /* Wait for async lcd_init task to finish before accessing driver state */

  if(!s_drv_lcd.p_drv_ops)
  {
  	sem_wait(&(s_drv_lcd.init_sem));
	sem_post(&s_drv_lcd.init_sem);
  }

 switch(s_drv_lcd.bpp)
 {
    case 8:
      vinfo->fmt     = FB_FMT_RGB8_332;
      break;

    case 16:
      vinfo->fmt     = FB_FMT_RGB16_565;    /* Color format: RGB16-565: RRRR RGGG GGGB BBBB */
      break;

    case 24:
      vinfo->fmt     = FB_FMT_RGB24;    /* Color format: RGB24 */
      break;

    default:
        DEBUGASSERT(0);
        break;
  }

  vinfo->xres    = s_drv_lcd.p_drv_ops->lcd_horizonal_res;        /* Horizontal resolution in pixel columns */
  vinfo->yres    = s_drv_lcd.p_drv_ops->lcd_vertical_res;        /* Vertical resolution in pixel rows */
  vinfo->nplanes = 1;                  /* Number of color planes supported */

  
  lcdinfo("fmt: %d xres: %d yres: %d nplanes: 1\n",
          vinfo->fmt, vinfo->xres, vinfo->yres);
  return OK;
}

/****************************************************************************
 * Name:  sf32lb_lcd_getplaneinfo
 *
 * Description:
 *   Get information about the configuration of each LCD color plane.
 *
 ****************************************************************************/

static int sf32lb_lcd_getplaneinfo(FAR struct lcd_dev_s *dev,
                               unsigned int planeno,
                               FAR struct lcd_planeinfo_s *pinfo)
{
  FAR struct sf32lb_lcd_dev_s *priv = (FAR struct sf32lb_lcd_dev_s *)dev;

  if(!s_drv_lcd.p_drv_ops)
  {
  	sem_wait(&(s_drv_lcd.init_sem));
	sem_post(&s_drv_lcd.init_sem);
  }

  DEBUGASSERT(dev && pinfo && planeno == 0);
  lcdinfo("planeno: %d bpp: %d\n", planeno, priv->bpp);

  pinfo->putrun = sf32lb_lcd_putrun;                  /* Put a run into LCD memory */
  pinfo->putarea = sf32lb_lcd_putarea;                /* Put an area into LCD */
#ifndef CONFIG_LCD_NOGETRUN
  pinfo->getrun = sf32lb_lcd_getrun;                  /* Get a run from LCD memory */
#endif
  pinfo->buffer = NULL; //(FAR uint8_t *)priv->runbuffer; /* Run scratch buffer */
  pinfo->bpp    = priv->bpp;                      /* Bits-per-pixel */
  pinfo->dev    = dev;                            /* The lcd device */
  return OK;
}

/****************************************************************************
 * Name:  sf32lb_lcd_getpower
 ****************************************************************************/

static int sf32lb_lcd_getpower(FAR struct lcd_dev_s *dev)
{
  FAR struct sf32lb_lcd_dev_s *priv = (FAR struct sf32lb_lcd_dev_s *)dev;

  lcdinfo("power: %d\n", priv->power);
  return priv->power;
}

/****************************************************************************
 * Name:  sf32lb_lcd_setpower
 ****************************************************************************/

static int sf32lb_lcd_setpower(FAR struct lcd_dev_s *dev, int power)
{
  FAR struct sf32lb_lcd_dev_s *priv = (FAR struct sf32lb_lcd_dev_s *)dev;

  lcdinfo("power: %d\n", power);
  DEBUGASSERT((unsigned)power <= CONFIG_LCD_MAXPOWER);

  /* Set new power level */

  if (power > 0)
    {
      /* Turn on the display */

      if (priv->p_drv_ops && priv->p_drv_ops->p_ops &&
          priv->p_drv_ops->p_ops->DisplayOn)
        {
          priv->p_drv_ops->p_ops->DisplayOn(&priv->hlcdc);
        }

      /* Save the power */

      priv->power = power;
    }
  else
    {
      /* Turn off the display */

      if (priv->p_drv_ops && priv->p_drv_ops->p_ops &&
          priv->p_drv_ops->p_ops->DisplayOff)
        {
          priv->p_drv_ops->p_ops->DisplayOff(&priv->hlcdc);
        }

      /* Save the power */

      priv->power = 0;
    }

  return OK;
}

/****************************************************************************
 * Name:  sf32lb_lcd_getcontrast
 *
 * Description:
 *   Get the current contrast setting (0-CONFIG_LCD_MAXCONTRAST).
 *
 ****************************************************************************/

static int sf32lb_lcd_getcontrast(FAR struct lcd_dev_s *dev)
{
  lcdinfo("Not implemented\n");
  return -ENOSYS;
}

/****************************************************************************
 * Name:  sf32lb_lcd_setcontrast
 *
 * Description:
 *   Set LCD panel contrast (0-CONFIG_LCD_MAXCONTRAST).
 *
 ****************************************************************************/

static int sf32lb_lcd_setcontrast(FAR struct lcd_dev_s *dev,
                              unsigned int contrast)
{
  lcdinfo("contrast: %d\n", contrast);
  return -ENOSYS;
}

                              
static int sf32lb_lcd_getalignment(FAR struct lcd_dev_s *dev,
                    FAR struct lcddev_area_align_s *align)
{
    if(align)
    {
        align->row_start_align = 2;
        align->height_align    = 2;
        align->col_start_align = 2;
        align->width_align     = 2;
        align->buf_align       = sizeof(uintptr_t);
    }

    return OK;
}

static int lcdc1_isr(int irq, void *context, void *arg)
{
    HAL_LCDC_IRQHandler((LCDC_HandleTypeDef *)arg);

    
    return OK;
}

static int lcd_hw_setup_thread_entry(int argc, FAR char *argv[])
{
    lcd_drv_desc_t *p_drv_ops = s_drv_lcd.p_drv_ops;
  int ret;
  int retry;

#if defined(CONFIG_VIDEO_FB) && defined(CONFIG_LCD_FRAMEBUFFER)
    /* Register /dev/fb0 first so node creation is not blocked by panel init. */
    for (retry = 0; retry < 30; retry++)
      {
        s_fb_registering = true;
        ret = fb_register(0, 0);
        s_fb_registering = false;

        if (ret == OK || ret == -EEXIST)
          {
            lcdinfo("fb_register done.\n");
            break;
          }

        if (ret != -ENOENT && ret != -ENODEV && ret != -EBUSY)
          {
            syslog(LOG_ERR, "ERROR: fb_register() failed: %d\n", ret);
            break;
          }

        usleep(100 * 1000);
      }
#endif

    if (p_drv_ops && p_drv_ops->p_ops && p_drv_ops->p_ops->Init)
    {
        if (s_panel_inited)
        {
            /* 面板已经在 lcd_init_thread_entry 里程序化过一次（那是
             * lcddev_register 的前提），这里再跑一遍等于把整段 co5300 初始化
             * 和自检重来：多等好几秒，而且会在 LVGL 已经 open 了 /dev/lcd0
             * 之后把整屏刷成测试色。 */
            syslog(LOG_ERR, "[lcd][diag] hw thread: panel already inited, "
                            "skip duplicate Init\n");
        }
        else
        {
            p_drv_ops->p_ops->Init(&s_drv_lcd.hlcdc);
            s_panel_inited = true;
        }
    }

    /* IRQ / 图层 / s_lcd_hw_ready / bring-up 可视测试都已经在
     * lcd_init_thread_entry 里、lcddev_register 之前做完了 —— 见那边的长注释。
     * 这条线程现在只剩 fb_register。 */

    return OK;
}

static int lcd_init_thread_entry(int argc, FAR char *argv[])
{
	lcd_drv_desc_t *p_drv_ops;
  int ret;
  int hw_pid;
  bool lcd_registered = false;
  int retry;

	BSP_LCD_PowerUp();
	
	p_drv_ops = find_right_driver();

#ifdef CONFIG_LCD_USING_CO5300
  if (!p_drv_ops)
  {
    p_drv_ops = (lcd_drv_desc_t *)&__lcddriver_co5300;
  }
#endif

#ifdef CONFIG_LCD_USING_ILI8688E
  if (!p_drv_ops)
  {
    p_drv_ops = (lcd_drv_desc_t *)&__lcddriver_ili8688e;
  }
#endif

	if (p_drv_ops)
	{
    lcdinfo("Init LCD %s", p_drv_ops->name);

		switch(p_drv_ops->p_init_cfg->color_mode)
		{
		   case LCDC_PIXEL_FORMAT_RGB565:
			 s_drv_lcd.bpp = 16;
			 break;
		
		   case LCDC_PIXEL_FORMAT_RGB888:
			 s_drv_lcd.bpp = 24;
			 break;
			 
         default:
       s_drv_lcd.bpp = 16;
       lcdwarn("Unknown color mode %d, fallback to RGB565",
               p_drv_ops->p_init_cfg->color_mode);
       break;
		 }

  /* Keep framebuffer format aligned with panel color mode (typically RGB565)
   * so fb writes are sent without intermediate color conversion.
   */

	}

	/* If find_right_driver fell back without running Init() (panel ReadID
	 * mismatch is normal for some QSPI panels), make sure the panel is
	 * actually programmed BEFORE lcddev_register / fb_register triggers
	 * setpower() -> DisplayOn (REG 0x29). Otherwise the very first WriteReg
	 * runs against an uninitialised LCDC controller and returns HAL_BUSY.
	 */
	if (p_drv_ops && p_drv_ops->p_ops && p_drv_ops->p_ops->Init)
	{
		p_drv_ops->p_ops->Init(&s_drv_lcd.hlcdc);
		s_panel_inited = true;
	}

	s_drv_lcd.p_drv_ops = p_drv_ops; 
	sem_post(&s_drv_lcd.init_sem);

	if (!p_drv_ops)
	{
		syslog(LOG_ERR, "ERROR: No LCD driver found, skip device register\n");
		return -ENODEV;
	}

  /* ---- 面板就绪：必须在 /dev/lcd0 注册之前完成 ----
   *
   * 顺序是这条链路最要紧的一件事。sf32lb_lcd_putarea() 在 !s_lcd_hw_ready 时
   * 会静默 return OK —— 调用方以为刷成功了，其实一个像素都没发。而 /dev/lcd0
   * 一旦注册出去，LVGL 立刻 open 并开始发首帧。
   *
   * v39 实测：首帧是【整屏】rows=0..449 cols=0..389，正好落在就绪之前，被整帧
   * 丢掉。[lcd][pa] DROP #1 就是这么来的。丢掉之后 LVGL 认为屏幕已经画干净，
   * 只重画真正变化的脏区 —— 于是 90 秒里只推了 1 次 100×52 的时间标签，
   * 整屏永远是残影。这就是"界面显示跑不通"的根因。
   *
   * 所以：IRQ、图层、就绪标志前置到这里，紧接着就注册，绝不在中间插任何
   * 耗时动作（bring-up 可视测试已挪到注册之后，见下面）。
   */
#ifdef SOC_BF0_HCPU     /* gpio1 only work on hcpu */
    irq_attach(NX_IRQ(LCDC1_IRQn), lcdc1_isr, (void *)&s_drv_lcd.hlcdc);
    up_enable_irq(NX_IRQ(LCDC1_IRQn));
#endif /* SOC_BF0_HCPU */

    HAL_LCDC_SetBgColor(&s_drv_lcd.hlcdc, 0, 0, 0);
    HAL_LCDC_LayerReset(&s_drv_lcd.hlcdc, HAL_LCDC_LAYER_DEFAULT);
    HAL_LCDC_LayerSetFormat(&s_drv_lcd.hlcdc, HAL_LCDC_LAYER_DEFAULT,
                            LCDC_PIXEL_FORMAT_RGB565);

    s_lcd_hw_ready = true;

    /* 这里【不再】跑 bring-up 可视测试。v41 实测：co5300 那一长串初始化 + 面板
     * 自检已经吃掉十几秒，itvis 再占好几秒，正好把应用侧等 /dev/lcd0 的 20 秒
     * 窗口撑爆 —— 日志里直接就是「[mianyu] 等 /dev/lcd0 超时，界面不启动」。
     * itvis 本身没问题（v41: wrram=912 done=912 timeout=0），错的是它挡在
     * lcddev_register 前面。现在注册第一优先，可视测试挪到注册之后跑。 */

    {
      int hb_pid = task_create("lcd_hb",
                               SCHED_PRIORITY_DEFAULT,
                               2048,
                               lcd_v39_hb_thread_entry,
                               NULL);
      if (hb_pid < 0)
        {
          syslog(LOG_ERR, "[lcd][hb] task_create failed: %d\n", errno);
        }
    }

    syslog(LOG_ERR, "[lcd][diag] panel ready before lcddev_register\n");


#ifdef CONFIG_LCD_DEV
    lcd_registered = false;
#endif
    /* Retry registration to tolerate early-boot timing races. */
    for (retry = 0; retry < 30; retry++)
    {
#ifdef CONFIG_LCD_DEV
      if (!lcd_registered)
      {
        ret = lcddev_register(0);
        if (ret == OK || ret == -EEXIST)
        {
          lcd_registered = true;
          lcdinfo("lcddev_register done.");
        }
        else if (ret != -ENOENT && ret != -ENODEV)
        {
          syslog(LOG_ERR, "ERROR: lcddev_register() failed: %d\n", ret);
          lcd_registered = true; /* stop retrying on hard errors */
        }
      }
#endif

#ifdef CONFIG_LCD_DEV
      if (lcd_registered)
      {
        break;
      }
#endif

      usleep(100 * 1000);
    }

    if (lcd_registered)
      {
        syslog(LOG_ERR, "[lcd][diag] /dev/lcd0 registered (took %d retries)\n",
               retry);
      }
    else
      {
        syslog(LOG_ERR, "[lcd][diag] WARN /dev/lcd0 NOT registered after 30 "
                        "retries\n");
      }

    /* ---- bring-up 可视测试（默认关，用 CONFIG_MIANYU_LCD_BRINGUP_VISUAL 召回）----
     *
     * 走的是 LVGL 那条一模一样的写路径，屏幕上出不出东西就是写通路的干净
     * 判决。它的使命已经完成：v42 真机日志里 itvis 744 次 wrram 全 done、
     * timeout=0，同一轮 LVGL 又成功推了 618 次 putarea —— 写通路、注册顺序、
     * 首帧落地三件事全部定性。现在每次开机再跑一遍只会用 8 秒纯色盖住刚起来
     * 的界面，所以默认不跑，代码留着以便以后换屏/换驱动时再定性一次。
     */
#ifdef CONFIG_MIANYU_LCD_BRINGUP_VISUAL
    {
      int vis_pid = task_create("lcd_itvis",
                                SCHED_PRIORITY_DEFAULT,
                                4096,
                                lcd_v39_itvis_thread_entry,
                                NULL);
      if (vis_pid < 0)
        {
          syslog(LOG_ERR, "[lcd][itvis] task_create failed: %d\n", errno);
        }
    }
#endif

    hw_pid = task_create("lcd_hw",
                         SCHED_PRIORITY_DEFAULT,
                         8192,
                         lcd_hw_setup_thread_entry,
                         NULL);

    if (hw_pid < 0)
    {
      syslog(LOG_ERR, "ERROR: lcd_hw task_create failed: %d\n", errno);
    }

#if defined(CONFIG_LCD_USING_CO5300)
    /* Bring-up 全屏纯色循环：真 UI 已经能上屏（v42：putarea 618 次、wrram 2248
     * 次全 done、timeout=0），这段测试色只会盖住界面，永久关闭。
     * 之前那段 if(0) 会把 visual_pid 留成 -1，每次都误报一行
     * "ERROR: lcd_visual task_create failed: 0"，一并清掉。 */
#endif

	return 0;
}
/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name:  board_lcd_initialize
 *
 * Description:
 *   Initialize the LCD video hardware.  The initial state of the LCD is
 *   fully initialized, display memory cleared, and the LCD ready to use, but
 *   with the power setting at 0 (full off).
 *
 ****************************************************************************/

int board_lcd_initialize(void)
{
    static bool initialized = false;
  int pid;

    if (initialized)
      return OK;
    initialized = true;

    lcdinfo("board_lcd_initialize\n");
    
    memset(&s_drv_lcd, 0, sizeof(s_drv_lcd));

    s_drv_lcd.hlcdc.Instance = LCDC1;

    s_drv_lcd.select_layer = HAL_LCDC_LAYER_DEFAULT;
    s_lcd_hw_ready = false;

    sem_init(&(s_drv_lcd.init_sem), 0, 0);
    sem_init(&(s_drv_lcd.draw_sem), 0, 0);

    /* Binary semaphore guarding the panel write path.  Initialised before the
     * init worker is spawned so every writer (itvis, LVGL) finds it valid. */
    sem_init(&s_wr_lock, 0, 1);

    /* Keep bringup non-blocking; init/register devices in a worker task. */

    pid = task_create("lcd_init",
                      SCHED_PRIORITY_DEFAULT,
                      4096,
                      lcd_init_thread_entry,
                      NULL);

    if (pid < 0)
      {
        lcdwarn("lcd_init task_create failed: %d", errno);
        return -errno;
      }

    return OK;
}

/****************************************************************************
 * Name:  board_lcd_getdev
 *
 * Description:
 *   Return a a reference to the LCD object for the specified LCD.  This
 *   allows support for multiple LCD devices.
 *
 ****************************************************************************/

struct lcd_dev_s *board_lcd_getdev(int devno)
{
    lcdinfo("board_lcd_getdev\n");

    
    struct lcd_dev_s *g_lcd = NULL;
    g_lcd = &s_drv_lcd.dev;

    g_lcd->getvideoinfo = sf32lb_lcd_getvideoinfo;
    g_lcd->getplaneinfo = sf32lb_lcd_getplaneinfo;
    g_lcd->getpower     = sf32lb_lcd_getpower;
    g_lcd->setpower     = sf32lb_lcd_setpower;
    g_lcd->getcontrast  = sf32lb_lcd_getcontrast;
    g_lcd->setcontrast  = sf32lb_lcd_setcontrast;
    g_lcd->getareaalign = sf32lb_lcd_getalignment;
  #if 0
  g_lcd = st7789_lcdinitialize(g_spidev);
  if (!g_lcd)
    {
      lcderr("ERROR: Failed to bind SPI port %d to LCD %d\n", LCD_SPI_PORTNO,
             devno);
    }
  else
    {
      lcdinfo("SPI port %d bound to LCD %d\n", LCD_SPI_PORTNO, devno);
      return g_lcd;
    }
  #endif /* 0 */

  return g_lcd;
}

/****************************************************************************
 * Name:  board_lcd_uninitialize
 *
 * Description:
 *   Uninitialize the LCD support
 *
 ****************************************************************************/

void board_lcd_uninitialize(void)
{
    lcdinfo("board_lcd_uninitialize\n");
    BSP_LCD_PowerDown();
    sem_destroy(&(s_drv_lcd.draw_sem));

}

