/**
  ******************************************************************************
  * @file   co5300.c
  * @author Sifli software development team
  * @brief   This file includes the LCD driver for CO5300 LCD.
  * @attention
  ******************************************************************************
*/


#include <sfconfig.h>
#include "string.h"
#include <syslog.h>
#include "sf32lb_lcd.h"

/* RT-Thread graphic pixel format compatibility */
#define RTGRAPHIC_PIXEL_FORMAT_RGB565  LCDC_PIXEL_FORMAT_RGB565
#define RTGRAPHIC_PIXEL_FORMAT_RGB888  LCDC_PIXEL_FORMAT_RGB888

/* Map Kconfig symbols to driver-expected names */
#define LCD_HOR_RES_MAX  CONFIG_LCD_HOR_RES_MAX
#define LCD_VER_RES_MAX  CONFIG_LCD_VER_RES_MAX

#define DEBUG_PRINTF(...)   lcdinfo(__VA_ARGS__)

/* CO5300 panel address window offsets differ by module resolution. */
#if (LCD_HOR_RES_MAX == 390) && (LCD_VER_RES_MAX == 450)
    #define COL_OFFSET (0)
    #define ROW_OFFSET (0)
#elif (LCD_HOR_RES_MAX == 410) && (LCD_VER_RES_MAX == 502)
    #define COL_OFFSET (22)
    #define ROW_OFFSET (0)
#elif AM178Q368448LK_178_368x448
    #define COL_OFFSET (16)
    #define ROW_OFFSET (0)
#elif AM151Q466466LK_151_466x466_C
    #define COL_OFFSET (6)
    #define ROW_OFFSET (0)
#elif AM200Q460460LK_200_460x460
    #define COL_OFFSET (10)
    #define ROW_OFFSET (0)
#elif H0198S005AMT005_V0_195_410x502
    #define COL_OFFSET (44)
    #define ROW_OFFSET (0)
#else
    #define ROW_OFFSET  (0x00)
    #define COL_OFFSET  (0x00)
#endif

/**
  * @brief CO5300 chip IDs
  */

#if (LCD_HOR_RES_MAX == 390) && (LCD_VER_RES_MAX == 450)
    #define LCD_ID                  0x331100
#elif (LCD_HOR_RES_MAX == 410) && (LCD_VER_RES_MAX == 502)
    #define LCD_ID                  0x530001
#elif defined(AM151Q466466LK_151_466x466_C)
    #define LCD_ID                  0x530001
#elif defined(AM200Q460460LK_200_460x460)
    #define LCD_ID                  0x530001
#elif defined(H0198S005AMT005_V0_195_410x502)
    #define LCD_ID                  0x530001
#elif defined(AM178Q368448LK_178_368x448)
    #define LCD_ID                  0x530001
#else
    #define LCD_ID                  0x331100
#endif

/**
  * @brief  CO5300 Size
  */
#define  LCD_PIXEL_WIDTH    (LCD_HOR_RES_MAX)
#define  LCD_PIXEL_HEIGHT   (LCD_VER_RES_MAX)

/**
 *  @brief LCD_OrientationTypeDef
 *  Possible values of Display Orientation
 */
#define REG_ORIENTATION_PORTRAIT         (0x00) /* Portrait orientation choice of LCD screen  */
#define REG_ORIENTATION_LANDSCAPE        (0x01) /* Landscape orientation choice of LCD screen */
#define REG_ORIENTATION_LANDSCAPE_ROT180 (0x02) /* Landscape rotated 180 orientation choice of LCD screen */

/**
  * @brief  CO5300 Registers
  */
#define REG_SW_RESET           0x01
#define REG_LCD_ID             0x04
#define REG_DSI_ERR            0x05
#define REG_POWER_MODE         0x0A
#define REG_SLEEP_IN           0x10
#define REG_SLEEP_OUT          0x11
#define REG_NORMAL_DISPLAY_ON  0x13
#define REG_PARTIAL_DISPLAY    0x12
#define REG_DISPLAY_INVERSION  0x21
#define REG_ALL_PIXEL_OFF      0x22
#define REG_ALL_PIXEL_ON       0x23
#define REG_DISPLAY_OFF        0x28
#define REG_DISPLAY_ON         0x29
#define REG_WRITE_RAM          0x2C
#define REG_READ_RAM           0x2E
#define REG_CASET              0x2A
#define REG_RASET              0x2B
#define REG_PART_CASET         0x30
#define REG_PART_RASET         0x31
#define REG_TEARING_EFFECT_OFF 0x34
#define REG_TEARING_EFFECT_ON  0x35
#define REG_MADCTL             0x36
#define REG_IDLE_MODE_OFF      0x38
#define REG_IDLE_MODE_ON       0x39
#define REG_COLOR_MODE         0x3A
#define REG_CONTINUE_WRITE_RAM 0x3C
#define REG_WBRIGHT            0x51 /* Write brightness*/
#define REG_RBRIGHT            0x52 /* Read brightness*/
#define REG_WRITE_CTRL_DISPLAY 0x53
#define REG_WRHBMDISBV         0x63
#define REG_SET_DISPLAY_MODE   0xC2
#define REG_SET_SPI_MODE       0xC4
#define REG_PASSWD1            0xF4
#define REG_PASSWD2            0xF5
#define REG_CMD_PAGE_SWITCH    0xFE

#define REG_BRIGHTNESS_MAX 0xFF

#define QAD_SPI_ITF LCDC_INTF_SPI_DCX_4DATA

static LCDC_InitTypeDef lcdc_int_cfg_qadspi =
{
    .lcd_itf = QAD_SPI_ITF, // LCDC_INTF_SPI_NODCX_1DATA
#if defined(LCD_MAX_CLK_FREQ)
    .freq = LCD_MAX_CLK_FREQ,        //CO5300 RGB565 only support 50000000,  RGB888 support 60000000
#else
    .freq = 50000000, //CO5300 RGB565 only support 50000000,  RGB888 support 60000000
#endif

    .color_mode = LCDC_PIXEL_FORMAT_RGB565,//LCDC_PIXEL_FORMAT_RGB565,

    .cfg = {
        .spi = {
            .dummy_clock = 0,
            /* TE sync disabled unconditionally for bring-up.
             *
             * HAL_LCDC_SYNC_VER makes the LCDC wait for a TE/VSYNC edge
             * before it moves each frame.  If the panel's TE line is not
             * routed through the 22p FPC (or the adapter board leaves it
             * floating) the edge never arrives, no frame is ever pushed,
             * and the panel stays black while every register write still
             * reports success.  Sync-free mode is what bring-up needs; it
             * can be re-enabled once a picture is confirmed.
             */
            .syn_mode = HAL_LCDC_SYNC_DISABLE,
            .vsyn_polarity = 1,
            //default_vbp=2, frame rate=82, delay=115us,
            //TODO: use us to define delay instead of cycle, delay_cycle=115*48
            .vsyn_delay_us = 0,
            .hsyn_num = 0,
        },
    },

};


static LCDC_InitTypeDef lcdc_int_cfg;

/* Last handle handed to LCD_Drv_Init.  Kept so the post-boot visual
 * cycle (started from sf32lb_lcd.c once device registration is done) can
 * drive the panel without reaching into that module's private state. */
static LCDC_HandleTypeDef *s_diag_hlcdc;

static uint32_t LCD_ReadID(LCDC_HandleTypeDef *hlcdc);
static void LCD_SetRegion(LCDC_HandleTypeDef *hlcdc, uint16_t Xpos0, uint16_t Ypos0, uint16_t Xpos1, uint16_t Ypos1);
static void     LCD_WriteReg(LCDC_HandleTypeDef *hlcdc, uint16_t LCD_Reg, uint8_t *Parameters, uint32_t NbParameters);
static uint32_t LCD_ReadData(LCDC_HandleTypeDef *hlcdc, uint16_t RegValue, uint8_t ReadSize);
static void LCD_ReadMode(LCDC_HandleTypeDef *hlcdc, bool enable);


/**
  * @brief  spi read/write mode
  * @param  enable: false - write spi mode |  true - read spi mode
  * @retval None
  */
static void LCD_ReadMode(LCDC_HandleTypeDef *hlcdc, bool enable)
{
    if (HAL_LCDC_IS_SPI_IF(lcdc_int_cfg.lcd_itf))
    {
        if (enable)
        {
            HAL_LCDC_SetFreq(hlcdc, 2000000); //read mode min cycle 300ns
        }
        else
        {
            HAL_LCDC_SetFreq(hlcdc, lcdc_int_cfg.freq); //Restore normal frequency
        }
    }

}

static void LCD_Clear(LCDC_HandleTypeDef *hlcdc)
{
    /*Clear gram*/
    HAL_LCDC_Next_Frame_TE(hlcdc, 0);
    LCD_SetRegion(hlcdc, 0, 0, LCD_PIXEL_WIDTH, LCD_PIXEL_HEIGHT);
    HAL_LCDC_LayerSetFormat(hlcdc, HAL_LCDC_LAYER_DEFAULT, LCDC_PIXEL_FORMAT_RGB565);
    HAL_LCDC_LayerDisable(hlcdc, HAL_LCDC_LAYER_DEFAULT);
    HAL_LCDC_SetBgColor(hlcdc, 0, 0, 0);
    HAL_LCDC_SendLayerData2Reg(hlcdc, ((0x32 << 24) | (REG_WRITE_RAM << 8)), 4);
    HAL_LCDC_LayerEnable(hlcdc, HAL_LCDC_LAYER_DEFAULT);

}


static void LCD_Drv_Init(LCDC_HandleTypeDef *hlcdc)
{
    uint8_t   parameter[14];

    s_diag_hlcdc = hlcdc;

    /* Initialize CO5300 low level bus layer ----------------------------------*/
    memcpy(&hlcdc->Init, &lcdc_int_cfg, sizeof(LCDC_InitTypeDef));
    if (HAL_LCDC_Init(hlcdc) != HAL_OK)
    {
        lcdwarn("[co5300] HAL_LCDC_Init failed");
        return;
    }

    BSP_LCD_Reset(1);
    LCD_DRIVER_DELAY_MS(10);
    BSP_LCD_Reset(0);//Reset LCD
    LCD_DRIVER_DELAY_MS(10);
    BSP_LCD_Reset(1);
    LCD_DRIVER_DELAY_MS(120);

    LCD_WriteReg(hlcdc, 0x01, (uint8_t *)NULL, 0);
    LCD_DRIVER_DELAY_MS(120);

    /* ReadID is informational only - some panels do not respond reliably
     * to ID queries on USB-only power but still init/draw correctly.
     * Log mismatch for diagnostics but proceed with panel init regardless.
     */
    {
        uint32_t pid = LCD_ReadID(hlcdc);
        /* [co5300][diag] always visible on the UART1 console */
        syslog(LOG_ERR, "[co5300][diag] panel ReadID=0x%lx expected 0x%x %s\n",
               (unsigned long)pid, LCD_ID,
               (pid == LCD_ID) ? "(match)" : "(MISMATCH -> panel not\n"
               "                          responding on QSPI: check 22p FPC,\n"
               "                          VADD_EN/PA37 and 3V3 on FPC pin 17)");
    }

    /* This board uses fixed panel config via Kconfig; avoid blocking ID read
     * in early bringup on platforms where QSPI read timing is not ready yet.
     */

#if (LCD_HOR_RES_MAX == 390) && (LCD_VER_RES_MAX == 450)
    parameter[0] = 0x20;
#elif (LCD_HOR_RES_MAX == 410) && (LCD_VER_RES_MAX == 502)
    parameter[0] = 0x00;
#elif defined(AM151Q466466LK_151_466x466_C)
    parameter[0] = 0x00;
#elif defined(AM178Q368448LK_178_368x448)
    parameter[0] = 0x00;
#elif defined(AM200Q460460LK_200_460x460)
    parameter[0] = 0x00;
#elif defined(H0198S005AMT005_V0_195_410x502)
    parameter[0] = 0x00;
#else
    parameter[0] = 0x20;
#endif
    LCD_WriteReg(hlcdc, REG_CMD_PAGE_SWITCH, parameter, 1); //Pass word unlock
    parameter[0] = 0x5A;
    LCD_WriteReg(hlcdc, REG_PASSWD1, parameter, 1);
    parameter[0] = 0x59;
    LCD_WriteReg(hlcdc, REG_PASSWD2, parameter, 1);

    parameter[0] = 0x20;
    LCD_WriteReg(hlcdc, REG_CMD_PAGE_SWITCH, parameter, 1); //Pass word lock
    parameter[0] = 0xA5;
    LCD_WriteReg(hlcdc, REG_PASSWD1, parameter, 1);
    parameter[0] = 0xA5;
    LCD_WriteReg(hlcdc, REG_PASSWD2, parameter, 1);

    parameter[0] = 0x00;
    LCD_WriteReg(hlcdc, REG_CMD_PAGE_SWITCH, parameter, 1);
    parameter[0] = 0x80;
    LCD_WriteReg(hlcdc, REG_SET_SPI_MODE, parameter, 1);
    parameter[0] = 0x55;
    LCD_WriteReg(hlcdc, REG_COLOR_MODE, parameter, 1);
#ifdef LCD_CO5300_VSYNC_ENABLE
    parameter[0] = 0x00;
    LCD_WriteReg(hlcdc, REG_TEARING_EFFECT_ON, parameter, 1);
#else
    LCD_WriteReg(hlcdc, REG_TEARING_EFFECT_OFF, (uint8_t *)NULL, 0);
#endif
    parameter[0] = 0x20;
    LCD_WriteReg(hlcdc, REG_WRITE_CTRL_DISPLAY, parameter, 1);
    parameter[0] = 0x7F;
    LCD_WriteReg(hlcdc, REG_WBRIGHT, parameter, 1);

    parameter[0] = 0xff;
    LCD_WriteReg(hlcdc, REG_WRHBMDISBV, parameter, 1);

    parameter[0] = (COL_OFFSET >> 8) & 0xFF;
    parameter[1] = COL_OFFSET & 0xFF;
    parameter[2] = ((LCD_PIXEL_WIDTH + COL_OFFSET - 1) >> 8) & 0xFF;
    parameter[3] = (LCD_PIXEL_WIDTH + COL_OFFSET - 1) & 0xFF;
    LCD_WriteReg(hlcdc, REG_CASET, parameter, 4);
    parameter[0] = (ROW_OFFSET >> 8) & 0xFF;
    parameter[1] = ROW_OFFSET & 0xFF;
    parameter[2] = ((LCD_PIXEL_HEIGHT + ROW_OFFSET - 1) >> 8) & 0xFF;
    parameter[3] = (LCD_PIXEL_HEIGHT + ROW_OFFSET - 1) & 0xFF;
    LCD_WriteReg(hlcdc, REG_RASET, parameter, 4);

    LCD_WriteReg(hlcdc, REG_SLEEP_OUT, (uint8_t *)NULL, 0);

    //sleep out+display on
    LCD_DRIVER_DELAY_MS(120);
    LCD_WriteReg(hlcdc, REG_DISPLAY_ON, (uint8_t *)NULL, 0);
    LCD_DRIVER_DELAY_MS(20);
    syslog(LOG_ERR, "[co5300][diag] init done: DISPLAY_ON issued\n");

    /* Bring-up colour ramp.
     *
     * Up to this point the driver has only ever written commands: not a
     * single pixel has been pushed, so the panel is showing whatever its
     * GRAM happened to power up with.  "Black screen" is therefore the
     * expected result even on perfect hardware, which makes it useless as
     * a fault signal.
     *
     * Painting a few full-screen colours here exercises the whole
     * LCDC -> QSPI -> panel data path with no involvement from fb, LVGL,
     * TE sync or application code.  What the panel does now is a clean
     * verdict: colours appear => the data path works and any remaining
     * problem is in the UI layer; nothing appears => the path itself is
     * broken (sync, timing, supply or wiring).
     */
    {
        /* Panel bring-up test.
         *
         * This module is an AMOLED panel: it emits its own light, so the
         * PA01 PWM the pinmux calls "backlight" is not what makes it
         * visible, and brightness is programmed through register 0x51.
         * More importantly, an AMOLED that has been sent commands but no
         * pixel data shows black - which is exactly what the driver did up
         * to this point, so a black screen carries no fault information.
         *
         * The two tests below split the link in half:
         *
         *   A) 0x23 / 0x22  ALL_PIXEL_ON / ALL_PIXEL_OFF
         *      Panel-side hardware commands.  They need no GRAM writes and
         *      no address window, so they exercise the command channel
         *      alone.  Colour appears => commands reach the panel.
         *
         *   B) full-screen GRAM fill
         *      Adds the bulk data channel on top of A.
         *
         * A fails            -> the QSPI write never lands (supply, reset,
         *                       pin mux or the FPC itself)
         * A works, B fails   -> commands land but pixel data does not
         *                       (sync / timing / bandwidth)
         * both work          -> panel and display link are fine, the fault
         *                       is above the driver (fb, LVGL, app)
         */
        static const uint8_t ramp[4][3] =
        {
            { 255, 255, 255 },  /* white */
            { 255,   0,   0 },  /* red   */
            {   0, 255,   0 },  /* green */
            {   0,   0, 255 },  /* blue  */
        };
        uint8_t maxb = (uint8_t)REG_BRIGHTNESS_MAX;
        int ci;

        /* Make sure nothing can be blamed on a dim setting. */
        LCD_WriteReg(hlcdc, REG_WBRIGHT, &maxb, 1);
        syslog(LOG_ERR, "[co5300][selftest] brightness set to 0x%02x\n",
               (unsigned)maxb);

        /* ---- A) command-channel test: ALL_PIXEL_ON / ALL_PIXEL_OFF ---- */
        for (ci = 0; ci < 4; ci++)
        {
            uint16_t c565 = (uint16_t)
                (((ramp[ci][0] >> 3) << 11) |
                 ((ramp[ci][1] >> 2) <<  5) |
                  (ramp[ci][2] >> 3));
            uint8_t  ap[2];

            ap[0] = (uint8_t)(c565 >> 8);
            ap[1] = (uint8_t)(c565 & 0xFF);

            syslog(LOG_ERR,
                   "[co5300][cmdtest] ALL_PIXEL_ON RGB(%u,%u,%u) 565=0x%04x\n",
                   (unsigned)ramp[ci][0], (unsigned)ramp[ci][1],
                   (unsigned)ramp[ci][2], (unsigned)c565);

            LCD_WriteReg(hlcdc, REG_ALL_PIXEL_ON, ap, 2);
            LCD_DRIVER_DELAY_MS(700);

            LCD_WriteReg(hlcdc, REG_ALL_PIXEL_OFF, (uint8_t *)NULL, 0);
            LCD_DRIVER_DELAY_MS(300);
        }

        syslog(LOG_ERR, "[co5300][cmdtest] done\n");

        /* ---- B) data-channel test: full-screen GRAM fill ---- */
        for (ci = 0; ci < 4; ci++)
        {
            syslog(LOG_ERR,
                   "[co5300][selftest] GRAM fill RGB(%u,%u,%u)\n",
                   (unsigned)ramp[ci][0], (unsigned)ramp[ci][1],
                   (unsigned)ramp[ci][2]);

            HAL_LCDC_LayerDisable(hlcdc, HAL_LCDC_LAYER_DEFAULT);
            HAL_LCDC_SetBgColor(hlcdc, ramp[ci][0], ramp[ci][1], ramp[ci][2]);
            LCD_SetRegion(hlcdc, 0, 0,
                          LCD_PIXEL_WIDTH - 1, LCD_PIXEL_HEIGHT - 1);
            HAL_LCDC_SendLayerData2Reg(
                hlcdc, ((0x32 << 24) | (REG_WRITE_RAM << 8)), 4);
            HAL_LCDC_LayerEnable(hlcdc, HAL_LCDC_LAYER_DEFAULT);

            LCD_DRIVER_DELAY_MS(700);
        }

        /* ---- C) read the panel back ------------------------------------
         *
         * Everything above is write-only, and a write the panel never
         * latched looks exactly like one that worked.  The driver's QSPI
         * read path is already proven by the ReadID at the top of init, so
         * whatever comes back here comes back from the panel itself.
         */
        {
            /* The byte layout HAL_LCDC_ReadDatas() uses for short reads is
             * not documented in this tree - the body lives in ROM
             * (__HAL_ROM_USED).  It decides how every read has to be
             * interpreted:
             *
             *   MSB-first      first byte read sits at bits 31:24
             *   right-aligned  first byte read sits at bits  7:0
             *
             * The 3-byte ReadID at the top of init returns 0x00331100, which
             * fits both readings, so probe one register at four lengths and
             * let the numbers decide instead of guessing.
             */
            uint32_t id1 = LCD_ReadData(hlcdc, REG_LCD_ID, 1);
            uint32_t id2 = LCD_ReadData(hlcdc, REG_LCD_ID, 2);
            uint32_t id3 = LCD_ReadData(hlcdc, REG_LCD_ID, 3);
            uint32_t id4 = LCD_ReadData(hlcdc, REG_LCD_ID, 4);

            uint32_t r0a = LCD_ReadData(hlcdc, REG_POWER_MODE,      1);
            uint32_t r0b = LCD_ReadData(hlcdc, 0x0B,                1);
            uint32_t r0c = LCD_ReadData(hlcdc, 0x0C,                1);
            uint32_t r0d = LCD_ReadData(hlcdc, 0x0D,                1);
            uint32_t r0e = LCD_ReadData(hlcdc, 0x0E,                1);
            uint32_t r0f = LCD_ReadData(hlcdc, 0x0F,                1);
            uint32_t r3a = LCD_ReadData(hlcdc, REG_COLOR_MODE,      1);
            uint32_t r52 = LCD_ReadData(hlcdc, REG_RBRIGHT,         1);

            syslog(LOG_ERR,
                   "[co5300][probe] ID len1=0x%08x len2=0x%08x "
                   "len3=0x%08x len4=0x%08x\n",
                   (unsigned)id1, (unsigned)id2,
                   (unsigned)id3, (unsigned)id4);
            syslog(LOG_ERR,
                   "[co5300][probe] 0x0A=0x%08x 0x0B=0x%08x "
                   "0x0C=0x%08x 0x0D=0x%08x\n",
                   (unsigned)r0a, (unsigned)r0b,
                   (unsigned)r0c, (unsigned)r0d);
            syslog(LOG_ERR,
                   "[co5300][probe] 0x0E=0x%08x 0x0F=0x%08x "
                   "0x3A=0x%08x 0x52=0x%08x\n",
                   (unsigned)r0e, (unsigned)r0f,
                   (unsigned)r3a, (unsigned)r52);

            /* Magenta 0xF81F: neither byte is 0x00 or 0xFF, so reading blank
             * GRAM (0x0000 / 0xFFFF) can never fake a hit. */
            HAL_LCDC_LayerDisable(hlcdc, HAL_LCDC_LAYER_DEFAULT);
            HAL_LCDC_SetBgColor(hlcdc, 255, 0, 255);
            LCD_SetRegion(hlcdc, 0, 0,
                          LCD_PIXEL_WIDTH - 1, LCD_PIXEL_HEIGHT - 1);
            HAL_LCDC_SendLayerData2Reg(
                hlcdc, ((0x32 << 24) | (REG_WRITE_RAM << 8)), 4);
            HAL_LCDC_LayerEnable(hlcdc, HAL_LCDC_LAYER_DEFAULT);
            LCD_DRIVER_DELAY_MS(200);

            {
                uint32_t ram4 = LCD_ReadData(hlcdc, REG_READ_RAM, 4);
                uint32_t ram2 = LCD_ReadData(hlcdc, REG_READ_RAM, 2);
                uint32_t idag = LCD_ReadData(hlcdc, REG_LCD_ID, 3);
                uint8_t  b0 = (uint8_t)((ram4 >> 24) & 0xFFu);
                uint8_t  b1 = (uint8_t)((ram4 >> 16) & 0xFFu);
                uint8_t  b2 = (uint8_t)((ram4 >>  8) & 0xFFu);
                uint8_t  b3 = (uint8_t)( ram4         & 0xFFu);
                int      hit;

                /* Byte-order independent: a 2-byte read of solid magenta is
                 * exactly the colour, in one order or the other. */
                hit = (ram2 == 0x0000F81Fu || ram2 == 0xF81F0000u);

                /* ... and in the 4-byte read, F8/1F must appear adjacent. */
                if (!hit)
                {
                    hit = ((b0 == 0xF8 && b1 == 0x1F) ||
                           (b1 == 0xF8 && b2 == 0x1F) ||
                           (b2 == 0xF8 && b3 == 0x1F) ||
                           (b0 == 0x1F && b1 == 0xF8) ||
                           (b1 == 0x1F && b2 == 0xF8) ||
                           (b2 == 0x1F && b3 == 0xF8));
                }

                syslog(LOG_ERR,
                       "[co5300][probe] after magenta fill: "
                       "GRAM len4=0x%08x len2=0x%08x | ID(again) len3=0x%08x\n",
                       (unsigned)ram4, (unsigned)ram2, (unsigned)idag);

                syslog(LOG_ERR,
                       "[co5300][readback] GRAM(0x2E)=%02x %02x %02x %02x "
                       "-> %s\n",
                       (unsigned)b0, (unsigned)b1, (unsigned)b2, (unsigned)b3,
                       hit
                       ? "MATCH -> magenta reached panel GRAM, data path OK"
                       : "NO MATCH -> pixel data is not landing (QSPI write/mode)");
            }
        }

        syslog(LOG_ERR, "[co5300][selftest] both tests done\n");
    }

}




/**
  * @brief  Power on the LCD.
  * @param  None
  * @retval None
 */
static void LCD_Init(LCDC_HandleTypeDef *hlcdc)
{
#ifdef BSP_LCDC_USING_QADSPI
    memcpy(&lcdc_int_cfg, &lcdc_int_cfg_qadspi, sizeof(lcdc_int_cfg));
#endif /* BSP_LCDC_USING_QADSPI */

    LCD_Drv_Init(hlcdc);
}


/**
  * @brief  Disables the Display.
  * @param  None
  * @retval LCD Register Value.
  */
static uint32_t LCD_ReadID(LCDC_HandleTypeDef *hlcdc)
{
    uint32_t data;
    data = LCD_ReadData(hlcdc, REG_LCD_ID, 3);

    if (data == LCD_ID)
        DEBUG_PRINTF("LCD module use CO5300 IC \n");
#if defined(AM178Q368448LK_178_368x448)
    data = LCD_ID;
#elif defined(AM151Q466466LK_151_466x466_C)
    data = LCD_ID;
#elif defined(AM200Q460460LK_200_460x460)
    data = LCD_ID;
#elif defined(H0198S005AMT005_V0_195_410x502)
    data = LCD_ID;
#endif
    return data;
}

/**
  * @brief  Enables the Display.
  * @param  None
  * @retval None
  */
static void LCD_DisplayOn(LCDC_HandleTypeDef *hlcdc)
{
    /* Display On */
    LCD_WriteReg(hlcdc, REG_DISPLAY_ON, (uint8_t *)NULL, 0);
}

/**
  * @brief  Disables the Display.
  * @param  None
  * @retval None
  */
static void LCD_DisplayOff(LCDC_HandleTypeDef *hlcdc)
{
    /* Display Off */
    LCD_WriteReg(hlcdc, REG_DISPLAY_OFF, (uint8_t *)NULL, 0);
}

static void LCD_SetRegion(LCDC_HandleTypeDef *hlcdc, uint16_t Xpos0, uint16_t Ypos0, uint16_t Xpos1, uint16_t Ypos1)
{
    uint8_t   parameter[4];

    HAL_LCDC_SetROIArea(hlcdc, Xpos0, Ypos0, Xpos1, Ypos1);

    Xpos0 += COL_OFFSET;
    Xpos1 += COL_OFFSET;

    Ypos0 += ROW_OFFSET;
    Ypos1 += ROW_OFFSET;

    parameter[0] = (Xpos0) >> 8;
    parameter[1] = (Xpos0) & 0xFF;
    parameter[2] = (Xpos1) >> 8;
    parameter[3] = (Xpos1) & 0xFF;
    LCD_WriteReg(hlcdc, REG_CASET, parameter, 4);

    parameter[0] = (Ypos0) >> 8;
    parameter[1] = (Ypos0) & 0xFF;
    parameter[2] = (Ypos1) >> 8;
    parameter[3] = (Ypos1) & 0xFF;
    LCD_WriteReg(hlcdc, REG_RASET, parameter, 4);
}

/**
  * @brief  Writes pixel.
  * @param  Xpos: specifies the X position.
  * @param  Ypos: specifies the Y position.
  * @param  RGBCode: the RGB pixel color
  * @retval None
  */
static void LCD_WritePixel(LCDC_HandleTypeDef *hlcdc, uint16_t Xpos, uint16_t Ypos, const uint8_t *RGBCode)
{
    uint8_t data = 0;

    /* Set Cursor */
    LCD_SetRegion(hlcdc, Xpos, Ypos, Xpos, Ypos);
    LCD_WriteReg(hlcdc, REG_WRITE_RAM, (uint8_t *)RGBCode, 2);
}

static void LCD_WriteMultiplePixels(LCDC_HandleTypeDef *hlcdc, const uint8_t *RGBCode, uint16_t Xpos0, uint16_t Ypos0, uint16_t Xpos1, uint16_t Ypos1)
{
    HAL_LCDC_LayerSetData(hlcdc, HAL_LCDC_LAYER_DEFAULT, (uint8_t *)RGBCode, Xpos0, Ypos0, Xpos1, Ypos1);

    /* Keep transfer interrupt-driven so upper layer timeout can recover
     * from unexpected LCDC/TE conditions.
     */

    HAL_LCDC_SendLayerData2Reg_IT(hlcdc, ((0x32 << 24) | (REG_WRITE_RAM << 8)), 4);

}


/**
  * @brief  Writes  to the selected LCD register.
  * @param  LCD_Reg: address of the selected register.
  * @retval None
  */
static void LCD_WriteReg(LCDC_HandleTypeDef *hlcdc, uint16_t LCD_Reg, uint8_t *Parameters, uint32_t NbParameters)
{
    uint32_t cmd;
    HAL_StatusTypeDef status;

    if ((REG_WRITE_RAM == LCD_Reg) || (REG_CONTINUE_WRITE_RAM == LCD_Reg))
    {
        cmd = (0x32 << 24) | (LCD_Reg << 8);
    }
    else
    {
        cmd = (0x02 << 24) | (LCD_Reg << 8);
    }

    status = HAL_LCDC_WriteU32Reg(hlcdc, cmd, Parameters, NbParameters);

    if (status != HAL_OK)
    {
        syslog(LOG_ERR, "[co5300][diag] WriteReg failed reg=0x%02x len=%lu st=%d\n",
               LCD_Reg, (unsigned long)NbParameters, status);
    }

}



/**
  * @brief  Reads the selected LCD Register.
  * @param  RegValue: Address of the register to read
  * @param  ReadSize: Number of bytes to read
  * @retval LCD Register Value.
  */
static uint32_t LCD_ReadData(LCDC_HandleTypeDef *hlcdc, uint16_t RegValue, uint8_t ReadSize)
{
    uint32_t rd_data = 0;

    LCD_ReadMode(hlcdc, true);

    HAL_LCDC_ReadU32Reg(hlcdc, ((0x03 << 24) | (RegValue << 8)), (uint8_t *)&rd_data, ReadSize);


    LCD_ReadMode(hlcdc, false);

    return rd_data;
}



static uint32_t LCD_ReadPixel(LCDC_HandleTypeDef *hlcdc, uint16_t Xpos, uint16_t Ypos)
{
    uint8_t  r, g, b;
    uint32_t ret_v, read_value;
    DEBUG_PRINTF("CO5300_ReadPixel[%d,%d]\n", Xpos, Ypos);

    LCD_SetRegion(hlcdc, Xpos, Ypos, Xpos, Ypos);

    read_value = LCD_ReadData(hlcdc, REG_READ_RAM, 4);
    DEBUG_PRINTF("result: [%x]\n", read_value);

    b = (read_value >> 0) & 0xFF;
    g = (read_value >> 8) & 0xFF;
    r = (read_value >> 16) & 0xFF;

    DEBUG_PRINTF("r=%d, g=%d, b=%d \n", r, g, b);

    switch (lcdc_int_cfg.color_mode)
    {
    case LCDC_PIXEL_FORMAT_RGB565:
        ret_v = (uint32_t)(((r << 11) & 0xF800) | ((g << 5) & 0x7E0) | ((b >> 3) & 0X1F));
        break;

    /*
       (8bit R + 3bit dummy + 8bit G + 3bit dummy + 8bit B)

    */
    case LCDC_PIXEL_FORMAT_RGB888:
        ret_v = (uint32_t)(((r << 16) & 0xFF0000) | ((g << 8) & 0xFF00) | ((b) & 0XFF));
        break;

    default:
        DEBUGASSERT(0);
        break;
    }

    return ret_v;
}


static void LCD_SetColorMode(LCDC_HandleTypeDef *hlcdc, uint16_t color_mode)
{
    uint8_t   parameter[2];

    switch (color_mode)
    {
    case RTGRAPHIC_PIXEL_FORMAT_RGB565:
        /* Color mode 16bits/pixel */
        parameter[0] = 0xD5;
        lcdc_int_cfg.color_mode = LCDC_PIXEL_FORMAT_RGB565;
        break;

    case RTGRAPHIC_PIXEL_FORMAT_RGB888:
        parameter[0] = 0xF7;
        lcdc_int_cfg.color_mode = LCDC_PIXEL_FORMAT_RGB888;
        break;

    default:
        return; //unsupport
        break;
    }

    LCD_WriteReg(hlcdc, REG_COLOR_MODE, parameter, 1);
    HAL_LCDC_SetOutFormat(hlcdc, lcdc_int_cfg.color_mode);
}

static void  LCD_SetBrightness(LCDC_HandleTypeDef *hlcdc, uint8_t br)
{
    uint8_t bright = (uint8_t)((int)REG_BRIGHTNESS_MAX * br / 100);
    LCD_WriteReg(hlcdc, REG_WBRIGHT, &bright, 1);
}

/**
  * @brief  Enable the Display idle mode.
  * @param  None
  * @retval None
  */
static void LCD_IdleModeOn(LCDC_HandleTypeDef *hlcdc)
{
    uint8_t   parameter[14];

    parameter[0] = 0x00;
    LCD_WriteReg(hlcdc, 0xFE, parameter, 1);

    /* Idle mode On */
    LCD_WriteReg(hlcdc, REG_IDLE_MODE_ON, NULL, 0);
}

/**
  * @brief  Disables the Display idle mode.
  * @param  None
  * @retval None
  */
static void LCD_IdleModeOff(LCDC_HandleTypeDef *hlcdc)
{
    uint8_t   parameter[14];

    parameter[0] = 0x00;
    LCD_WriteReg(hlcdc, 0xFE, parameter, 1);

    /* Idle mode Off */
    LCD_WriteReg(hlcdc, REG_IDLE_MODE_OFF, NULL, 0);

}


static const LCD_DrvOpsDef CO5300_drv =
{
    LCD_Init,
    LCD_ReadID,
    LCD_DisplayOn,
    LCD_DisplayOff,

    LCD_SetRegion,
    LCD_WritePixel,
    LCD_WriteMultiplePixels,

    LCD_ReadPixel,

    LCD_SetColorMode,
    LCD_SetBrightness,
    LCD_IdleModeOn,
    LCD_IdleModeOff,

};

LCD_DRIVER_EXPORT(co5300, LCD_ID, &lcdc_int_cfg,
                  &CO5300_drv,
                  LCD_PIXEL_WIDTH,
                  LCD_PIXEL_HEIGHT,
                  2);


/* ---------------------------------------------------------------------------
 * Bring-up aid: visual colour cycle.
 *
 * The init-time test above lasts about seven seconds, which is easy to miss.
 * This runs as its own thread and repaints the whole panel once a second for
 * CO5300_VISUAL_CYCLES iterations, so the question "does this panel emit any
 * light at all?" can be answered at leisure - by eye or with a camera.
 *
 * It stops by itself and settles on solid white, so it will not fight a real
 * UI once one exists (delete the task_create in sf32lb_lcd.c at that point).
 *
 * The layer is disabled around each fill on purpose: the LCDC is never
 * started for continuous scan in this build, so GRAM keeps whatever was
 * written last and the colour stays on screen until the next write.
 * ------------------------------------------------------------------------- */
#define CO5300_VISUAL_CYCLES 240
#define CO5300_ROUNDS        10   /* 10 轮 x 24 s = 240 s */

/* One full-screen fill, using the vendor's own clear-screen recipe
 * (LCD_Clear in this file) so the bring-up test exercises the same path the
 * panel driver itself trusts. */
static void co5300_fill(LCDC_HandleTypeDef *hlcdc,
                        uint8_t r, uint8_t g, uint8_t b)
{
    HAL_LCDC_Next_Frame_TE(hlcdc, 0);
    LCD_SetRegion(hlcdc, 0, 0, LCD_PIXEL_WIDTH - 1, LCD_PIXEL_HEIGHT - 1);
    HAL_LCDC_LayerSetFormat(hlcdc, HAL_LCDC_LAYER_DEFAULT,
                            LCDC_PIXEL_FORMAT_RGB565);
    HAL_LCDC_LayerDisable(hlcdc, HAL_LCDC_LAYER_DEFAULT);
    HAL_LCDC_SetBgColor(hlcdc, r, g, b);
    HAL_LCDC_SendLayerData2Reg(
        hlcdc, ((0x32 << 24) | (REG_WRITE_RAM << 8)), 4);
    HAL_LCDC_LayerEnable(hlcdc, HAL_LCDC_LAYER_DEFAULT);
}

/* ---------------------------------------------------------------------------
 * Bring-up aid: three-stage visual test.
 *
 * Each stage isolates one layer of the display chain, so a single look at the
 * panel says where it breaks:
 *
 *   A  display off/on blink          0x28 / 0x29   panel lights and blanks
 *   B  ALL_PIXEL_ON / OFF            0x23 / 0x22   panel-side uniform white,
 *                                                 no pixel data involved
 *   C  full-screen colour fill       GRAM write    adds the bulk data path
 *
 * 8 s per stage, 24 s per round, 10 rounds, then it parks on solid white.
 * Runs as its own thread and stops by itself - delete the task_create in
 * sf32lb_lcd.c once a real UI paints the panel.
 * ------------------------------------------------------------------------- */
void co5300_bringup_visual_cycle(void)
{
    static const uint8_t cyc[4][3] =
    {
        { 255, 255, 255 },  /* white   */
        { 255,   0,   0 },  /* red     */
        {   0, 255,   0 },  /* green   */
        {   0,   0, 255 },  /* blue    */
    };
    LCDC_HandleTypeDef *hlcdc = s_diag_hlcdc;
    int round, i;

    if (hlcdc == NULL)
    {
        syslog(LOG_ERR, "[co5300][visual] no handle, cycle skipped\n");
        return;
    }

    /* Let lcddev/fb registration and the second Init finish first. */
    LCD_DRIVER_DELAY_MS(6000);

    syslog(LOG_ERR,
           "[co5300][visual] three-stage test start: A blink / B all-pixel / "
           "C colour fill, %d rounds x 24 s\n", CO5300_ROUNDS);

    for (round = 0; round < CO5300_ROUNDS; round++)
    {
        /* ---- A: display off / on (8 s at 2 Hz) ---- */
        syslog(LOG_ERR,
               "[co5300][visual] round %d STAGE A: 0x28/0x29 display blink 8 s\n",
               round);
        for (i = 0; i < 16; i++)
        {
            LCD_WriteReg(hlcdc, REG_DISPLAY_OFF, (uint8_t *)NULL, 0);
            LCD_DRIVER_DELAY_MS(250);
            LCD_WriteReg(hlcdc, REG_DISPLAY_ON, (uint8_t *)NULL, 0);
            LCD_DRIVER_DELAY_MS(250);
        }

        /* ---- B: panel-side all pixels on / off (8 s) ---- */
        syslog(LOG_ERR,
               "[co5300][visual] round %d STAGE B: 0x23 ALL_PIXEL_ON white 8 s\n",
               round);
        for (i = 0; i < 8; i++)
        {
            LCD_WriteReg(hlcdc, REG_ALL_PIXEL_ON, (uint8_t *)NULL, 0);
            LCD_DRIVER_DELAY_MS(700);
            LCD_WriteReg(hlcdc, REG_ALL_PIXEL_OFF, (uint8_t *)NULL, 0);
            LCD_DRIVER_DELAY_MS(300);
        }

        /* ---- C: full-screen colour fill (8 s) ---- */
        syslog(LOG_ERR,
               "[co5300][visual] round %d STAGE C: GRAM fill 8 s\n", round);
        for (i = 0; i < 4; i++)
        {
            co5300_fill(hlcdc, cyc[i][0], cyc[i][1], cyc[i][2]);
            LCD_DRIVER_DELAY_MS(2000);
        }
    }

    /* Park on solid white: unambiguous, and obviously not "black". */
    co5300_fill(hlcdc, 255, 255, 255);
    syslog(LOG_ERR,
           "[co5300][visual] test finished, parked on solid white\n");
}
