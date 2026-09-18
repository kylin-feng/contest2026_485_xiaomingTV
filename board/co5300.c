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
#include "sf32lb_lcd.h"
#include <syslog.h>   /* v46: bring-up diagnostics on the UART console */

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
            /* v46 - sync-free, unconditionally.
             *
             * The vendor guards this with LCD_CO5300_VSYNC_ENABLE and picks
             * HAL_LCDC_SYNC_VER (wait for the panel's TE edge before each
             * frame).  With TE sync the LCDC only advances on a TE edge, so
             * if the TE line is not reaching the SoC the whole panel-update
             * path silently stalls.  Every build we have measured on this
             * board ran sync-free and moved real pixels:
             *
             *   [lcd][hb] t=25s putarea=140 wrram=380 done=380 timeout=0
             *
             * i.e. the LCDC finished every transfer it was given.  Sync-free
             * is therefore the value with evidence behind it, so it stays on
             * unconditionally.  (The panel is still told TE is enabled via
             * 0x35 below when LCD_CO5300_VSYNC_ENABLE is set; that is a
             * panel-side output setting and is harmless either way.)
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


/* v46 - drive the panel backlight pin (PA01) as a plain GPIO and read the
 * level back, so the verdict below cannot be fooled by a driver that fails
 * silently.  PA01 is the module's own "BL PWM" pin (module pin 46) and the
 * board muxes it to GPTIM1_CH4; nothing else in this tree ever started that
 * timer, so before v46 it simply sat idle. */
static void co5300_bl_pin(int level)
{
    int rb;

    HAL_PIN_Set(PAD_PA01, GPIO_A1, PIN_NOPULL, 1);
    HAL_GPIO_WritePin(hwp_gpio1, 1,
                      level ? GPIO_PIN_SET : GPIO_PIN_RESET);
    rb = (int)HAL_GPIO_ReadPin(hwp_gpio1, 1);
    syslog(LOG_ERR, "[co5300][bl] PA01 as GPIO -> %d (readback=%d)\n",
           level, rb);
}


/* v50 - the panel's analogue enable, PA37 (LCD_VADD_EN).  BSP_LCD_PowerUp
 * calls BSP_GPIO_Set() for it and prints a line, but nothing ever reads the
 * pin back, so a silently ineffective write would have gone unnoticed for
 * the whole bring-up.  Drive it here as a plain GPIO and read the level. */
static void co5300_vadd_pin(int level)
{
    int rb;

    HAL_PIN_Set(PAD_PA37, GPIO_A37, PIN_NOPULL, 1);
    HAL_GPIO_WritePin(hwp_gpio1, 37,
                      level ? GPIO_PIN_SET : GPIO_PIN_RESET);
    rb = (int)HAL_GPIO_ReadPin(hwp_gpio1, 37);
    syslog(LOG_ERR, "[co5300][vadd] PA37 (LCD_VADD_EN) -> %d (readback=%d)\n",
           level, rb);
}


static void LCD_Drv_Init(LCDC_HandleTypeDef *hlcdc)
{
    uint8_t   parameter[14];

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
        /* v46: always print this on the console - a mismatch is the fastest
         * way to tell "panel/FPC problem" from "everything else". */
        syslog(LOG_ERR, "[co5300][diag] panel ReadID=0x%lx expected 0x%x %s\n",
               (unsigned long)pid, LCD_ID,
               (pid == LCD_ID) ? "(match)" : "(MISMATCH - check 22p FPC)");
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
    /* v48: WRCTRLD(53h)=0x28 - keep BCTRL(D5) and enable DD(D3, dimming).
     * The vendor value 0x20 clears DD; while the dimming block is disabled
     * the DBV written through WRDISBV(51h) is not applied by the panel, so
     * an otherwise healthy AMOLED emits nothing at all.  DBV is raised to
     * 0xFF for the same reason: any residual dimming must not be able to
     * masquerade as a dead panel. */
    parameter[0] = 0x28;
    LCD_WriteReg(hlcdc, REG_WRITE_CTRL_DISPLAY, parameter, 1);
    parameter[0] = 0xFF;
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

    /* v49 fix: brightness MUST be (re)written after SLPOUT + DISPON.
     * WRDISBV(51h) powers up at its default 00h = 0 % brightness, and the
     * vendor sequence writes 0x7F BEFORE 0x11 SLPOUT - so if sleep-out
     * restores the brightness defaults the panel ends up emitting nothing
     * at all while RDDPM still reports a perfectly healthy display.  That
     * matches every measurement taken so far (RDDPM=0x9C but black), so
     * the value is now written again on the far side of DISPON and left at
     * full scale. */
    parameter[0] = 0x28;
    LCD_WriteReg(hlcdc, REG_WRITE_CTRL_DISPLAY, parameter, 1);   /* 53h */
    parameter[0] = 0xFF;
    LCD_WriteReg(hlcdc, REG_WBRIGHT, parameter, 1);              /* 51h */
    LCD_DRIVER_DELAY_MS(30);

    /* ------------------------------------------------------------------
     * v50 - v49 proved the emission path is dead, so stop poking brightness.
     *
     * What v49 measured, all of it on real hardware:
     *   51h=0x00 -> RDDISBV(52h)=0x00 ; 51h=0xFF -> RDDISBV=0x00
     *        the brightness register does not follow what is written to it;
     *   after the datasheet CMD1 unlock (FE=0x80, F4=0x00, F5=0x00) it is
     *        still 0x00; the HBM path (66h=0x01 + 63h=0xFF) leaves
     *        RDCABC(64h) at 0x00 as well;
     *   and yet RDDIM(0Dh) reads 0x10 while 0x23 is asserted, i.e.
     *        ALLPON=1 - the panel really does accept and latch
     *        "all pixels on", it simply emits no light while doing so.
     *
     * A panel that lamps every pixel on demand but produces no light has no
     * emission current, and no amount of register writing changes that.  So
     * v50 does not repeat the brightness work.  It goes after the two things
     * still untested:
     *
     *   a) RDDSC (0Fh), the panel's own self-diagnostic result - if the
     *      embedded checksum comparison fails, the panel reports its own
     *      fault and the answer is the module, not the firmware;
     *   b) a cold re-run of the datasheet power-on order, because VADD_EN
     *      and the backlight rail may have to rise in a defined order with
     *      the panel held in reset - v50 pulls both low, lets the rails
     *      decay, then raises them and re-issues DISPOFF/SLPOUT/DISPON;
     *   c) INVON vs INVOFF, in case the panel's VCOM polarity is set the
     *      other way round and every grey level lands on black.
     *
     * Then six slow 8 s windows so a working panel is impossible to miss.
     * ------------------------------------------------------------------ */
    /* ==================================================================
     * v50 诊断块：**产品固件里默认必须关掉**。
     *
     * 这块代码是排查"屏不发光"时写的取证脚本，它的用途已经完成了
     * （结论见 docs/黑屏结论_v50.md：把整屏 ALLPON 拉亮、面板 RDDIM(0Dh)
     * 也确认 latch 住了 "all pixels on"，但就是不发光 —— 说明没有发光电流，
     * 是模组/供电侧的事，不是固件能改的）。
     *
     * 留在产品固件里的代价非常大，而且表现极具误导性：
     *   1) 它跑在 LCD_Drv_Init() 里，而 lcddev_register()/dev/lcd0 在 Init()
     *      **之后**才执行。六个 8 秒窗口 = 开机后 50 多秒内 /dev/lcd0 不存在
     *      —— 界面线程（mianyu_hal_vela.c 里死等 /dev/lcd0）只能干等，
     *      屏上这一分多钟只有测试色在闪。用户看到的就是"开机一直黑"。
     *   2) 它会把 VADD_EN(PA37) 和背光(PA01) 拉低再做"冷上电"，等于每次
     *      开机都拿屏的电轨做一次扰动。
     *   3) 它会把整屏 ALLPON（全白）闪六次 —— 屏要是好的，开机先白闪一分钟，
     *      看起来完全像故障。
     *
     * 需要复现取证时打开 CONFIG_MIANYU_LCD_V50_SELFTEST 再编。 */
#ifdef CONFIG_MIANYU_LCD_V50_SELFTEST
    {
        uint32_t v;
        int      i;

        /* ---- a) panel self-diagnostic --------------------------------- */
        v = LCD_ReadData(hlcdc, 0x0F, 1);
        syslog(LOG_ERR, "[v50][a] RDDSC(0Fh)=0x%02lx checksum_comp=%lu "
                        "(0 = panel reports itself OK)\n",
               (unsigned long)v, (unsigned long)(v & 1));
        syslog(LOG_ERR, "[v50][a] rail readbacks: PA10 (LCD Power En)=%d "
                        "PA37 (VADD_EN)=%d PA01 (BL)=%d\n",
               (int)HAL_GPIO_ReadPin(hwp_gpio1, 10),
               (int)HAL_GPIO_ReadPin(hwp_gpio1, 37),
               (int)HAL_GPIO_ReadPin(hwp_gpio1, 1));

        /* ---- b) cold power-on order ----------------------------------- */
        syslog(LOG_ERR, "[v50][b] cold power order: VADD_EN(PA37)=0 "
                        "BL(PA01)=0 -> 60ms -> both HIGH -> 120ms -> "
                        "DISPOFF/SLPOUT/DISPON\n");
        co5300_vadd_pin(0);
        co5300_bl_pin(0);
        LCD_DRIVER_DELAY_MS(60);
        co5300_vadd_pin(1);
        co5300_bl_pin(1);
        LCD_DRIVER_DELAY_MS(120);
        LCD_WriteReg(hlcdc, 0x28, (uint8_t *)NULL, 0);           /* DISPOFF */
        LCD_DRIVER_DELAY_MS(20);
        LCD_WriteReg(hlcdc, REG_SLEEP_OUT, (uint8_t *)NULL, 0);  /* SLPOUT */
        LCD_DRIVER_DELAY_MS(130);
        LCD_WriteReg(hlcdc, REG_DISPLAY_ON, (uint8_t *)NULL, 0); /* DISPON */
        LCD_DRIVER_DELAY_MS(60);

        /* ---- c) six slow windows, alternating inversion --------------- */
        for (i = 0; i < 6; i++)
        {
            if (i & 1)
            {
                LCD_WriteReg(hlcdc, 0x21, (uint8_t *)NULL, 0);   /* INVON */
            }
            else
            {
                LCD_WriteReg(hlcdc, 0x20, (uint8_t *)NULL, 0);   /* INVOFF */
            }

            syslog(LOG_ERR, "[v50][W%d] ALLPON 8000ms with INVON=%d - "
                            "screen MUST be WHITE now\n", i, i & 1);
            co5300_bl_pin(1);
            LCD_WriteReg(hlcdc, 0x23, (uint8_t *)NULL, 0);       /* ALLPON */
            LCD_DRIVER_DELAY_MS(8000);
            LCD_WriteReg(hlcdc, 0x22, (uint8_t *)NULL, 0);       /* ALLPOFF */
            LCD_DRIVER_DELAY_MS(1000);
        }

        /* ---- hand a sane state back to the application ---------------- */
        LCD_WriteReg(hlcdc, 0x20, (uint8_t *)NULL, 0);           /* INVOFF */
        co5300_vadd_pin(1);
        co5300_bl_pin(1);
        syslog(LOG_ERR, "[v50] sequence finished, panel left on normal "
                        "display with inversion off\n");
    }
#endif /* CONFIG_MIANYU_LCD_V50_SELFTEST */
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
        lcdwarn("[co5300] WriteReg failed reg=0x%02x len=%lu st=%d",
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
