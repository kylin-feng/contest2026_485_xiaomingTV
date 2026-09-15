#include "bsp_board.h"
#include "bf0_hal.h"
#include <syslog.h>


#ifdef BSP_USING_LCD
#define LCD_RESET_PIN           (0)         // GPIO_A00
#define TP_RESET                (9)         // GPIO_A09

#ifdef LCD_USING_CO5300
    #define LCD_VADD_EN             (37)
#endif


/***************************LCD ***********************************/
extern void BSP_PIN_LCD(void);
void BSP_LCD_Reset(uint8_t high1_low0)
{
    BSP_GPIO_Set(LCD_RESET_PIN, high1_low0, 1);
}

void BSP_LCD_PowerDown(void)
{
    // TODO: LCD power down
    BSP_GPIO_Set(LCD_RESET_PIN, 0, 1);
#ifdef LCD_USING_CO5300
    BSP_GPIO_Set(LCD_VADD_EN, 0, 1); //POwer down VADD EN
#endif
}

/***************************LCD Backlight *************************/
/* PA01 = GPTIM1_CH4 is wired to the panel backlight (see README_zh-cn.md).
 * Nothing else in the tree starts this PWM, so the panel stays dark even
 * when the frame buffer is being fed correctly.  Configure + start it here
 * right after LCD power-up.  GPTIM1 channel numbering: CHn -> (n-1)*4.
 */
#ifdef LCD_USING_CO5300

#define LCD_BL_PWM_FREQ_HZ   (20000u)   /* 20 kHz, above the audible band */
#define LCD_BL_CHANNEL       (12u)      /* CH4 -> PA01 */
#ifndef LCD_BL_DUTY_PCT
#  define LCD_BL_DUTY_PCT    (100u)     /* 0..100 */
#endif

static GPT_HandleTypeDef g_lcd_bl_tim;

void BSP_LCD_BacklightInit(uint8_t duty_pct)
{
    GPT_OC_InitTypeDef oc_cfg;
    uint32_t timclk;
    uint32_t ticks;
    uint32_t prescaler;
    uint32_t period;
    uint32_t pulse;

    if (duty_pct > 100u)
    {
        duty_pct = 100u;
    }

    timclk = HAL_RCC_GetPCLKFreq(CORE_ID_HCPU, 1);
    if (timclk == 0u)
    {
        return;
    }

    ticks = timclk / LCD_BL_PWM_FREQ_HZ;
    if (ticks == 0u)
    {
        ticks = 1u;
    }

    prescaler = (ticks + 65535u - 1u) / 65535u;
    if (prescaler == 0u)
    {
        prescaler = 1u;
    }

    period = ticks / prescaler;
    if (period == 0u)
    {
        period = 1u;
    }
    else if (period > 65535u)
    {
        period = 65535u;
    }

    pulse = (uint32_t)(((uint64_t)duty_pct * (uint64_t)period) / 100u);
    if (pulse > period)
    {
        pulse = period;
    }

    g_lcd_bl_tim.Instance              = GPTIM1;
    g_lcd_bl_tim.core                  = CORE_ID_HCPU;
    g_lcd_bl_tim.Init.Prescaler        = prescaler - 1u;
    g_lcd_bl_tim.Init.CounterMode      = GPT_COUNTERMODE_UP;
    g_lcd_bl_tim.Init.Period           = period - 1u;
    g_lcd_bl_tim.Init.RepetitionCounter = 0u;

    if (HAL_GPT_PWM_Init(&g_lcd_bl_tim) != HAL_OK)
    {
        return;
    }

    oc_cfg.OCMode       = GPT_OCMODE_PWM1;
    oc_cfg.Pulse        = pulse;
    oc_cfg.OCPolarity   = GPT_OCPOLARITY_HIGH;
    oc_cfg.OCNPolarity  = GPT_OCNPOLARITY_LOW;
    oc_cfg.OCFastMode   = GPT_OCFAST_DISABLE;
    oc_cfg.OCIdleState  = GPT_OCIDLESTATE_RESET;
    oc_cfg.OCNIdleState = GPT_OCNIDLESTATE_RESET;

    if (HAL_GPT_PWM_ConfigChannel(&g_lcd_bl_tim, &oc_cfg, LCD_BL_CHANNEL) != HAL_OK)
    {
        return;
    }

    HAL_GPT_PWM_Start(&g_lcd_bl_tim, LCD_BL_CHANNEL);
}
#endif /* LCD_USING_CO5300 */

void BSP_LCD_PowerUp(void)
{
    // TODO: LCD power up
    HAL_Delay_us(500);      // lcd power on finish ,need 500us
    BSP_PIN_LCD();
#ifdef LCD_USING_CO5300
    BSP_GPIO_Set(LCD_VADD_EN, 1, 1); //POwer up VADD EN
    syslog(LOG_ERR, "[co5300][diag] VADD_EN (PA37) driven HIGH\n");
    /* PA01 driven as GPIO HIGH: LEDA on 22p FPC pin 2 must be a solid DC level, a PWM
     * channel whose timer is never started sits low.
     *
     * Blink it three times before leaving it on.  This separates "backlight
     * path dead" from "backlight fine but panel shows nothing" with the naked
     * eye, without needing any panel response.
     */
    HAL_PIN_Set(PAD_PA01, GPIO_A1, PIN_NOPULL, 1);
    for (int bl_i = 0; bl_i < 3; bl_i++)
    {
        BSP_GPIO_Set(1, 1, 1);
        syslog(LOG_ERR, "[bl][diag] backlight ON  (PA01=1) #%d/3\n", bl_i + 1);
        HAL_Delay_us(300000);
        BSP_GPIO_Set(1, 0, 1);
        syslog(LOG_ERR, "[bl][diag] backlight OFF (PA01=0) #%d/3\n", bl_i + 1);
        HAL_Delay_us(300000);
    }
    BSP_GPIO_Set(1, 1, 1);
    syslog(LOG_ERR, "[bl][diag] backlight left ON (PA01=1)\n");
#endif
}


/***************************Touch ***********************************/
extern void BSP_PIN_Touch(void);
void BSP_TP_PowerUp(void)
{
    // TODO: Setup TP power up pin
    BSP_PIN_Touch();
    BSP_GPIO_Set(TP_RESET,  1, 1);

    /* v39 electrical check.
     *
     * The driver's bring-up scan walks every valid 7-bit address and finds
     * nothing, which has two very different explanations:
     *   - the panel's touch controller is missing / not fitted (bus is fine,
     *     both lines idle high), or
     *   - the I2C1 lines are dead (no supply, FPC not seated, or a short).
     * Re-mux SCL/SDA as plain inputs with the internal pull-up enabled and
     * read the level: with nothing driving the bus both must read 1.  A 0
     * means the line is being held low and no amount of driver work can help.
     */
    {
        GPIO_InitTypeDef gi;
        int scl;
        int sda;

        gi.Mode = GPIO_MODE_INPUT;
        gi.Pull = GPIO_PULLUP;
        gi.Pin  = 30;
        HAL_GPIO_Init(hwp_gpio1, &gi);
        gi.Pin  = 33;
        HAL_GPIO_Init(hwp_gpio1, &gi);
        HAL_Delay_us(2000);

        scl = (int)HAL_GPIO_ReadPin(hwp_gpio1, 30);
        sda = (int)HAL_GPIO_ReadPin(hwp_gpio1, 33);
        syslog(LOG_ERR,
               "[tp][diag] idle I2C1 level SCL(PA30)=%d SDA(PA33)=%d "
               "(expect 1/1; 0 => bus stuck or panel absent)\n", scl, sda);

        HAL_PIN_Set(PAD_PA30, I2C1_SCL, PIN_PULLUP, 1);
        HAL_PIN_Set(PAD_PA33, I2C1_SDA, PIN_PULLUP, 1);
    }

    syslog(LOG_ERR, "[tp][diag] BSP_TP_PowerUp done: CTP_RESET(PA09)=1\n");
}

void BSP_TP_PowerDown(void)
{
    // TODO: Setup TP power down pin
    BSP_GPIO_Set(TP_RESET,  0, 1);
}
void BSP_TP_Reset(uint8_t high1_low0)
{
    BSP_GPIO_Set(TP_RESET, high1_low0, 1);
}

#endif
