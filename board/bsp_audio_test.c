/*
 * bsp_audio_test.c — 板载音频通路 bring-up（v5：DAC 放音 + ADC 录音 + 拍手响应）
 *
 * 版本演进（留档，别回退）
 * ========================
 * v1  DAC 初始化全 OK、寄存器回读全对，但"起播 1s 后 HT 0 / TC 0"。
 * v2  加 DMA 探针（CNDTR1 采样 + 原始 ISR 标志 + NVIC ISER/ISPR）：
 *         CNDTR1 采样: 首=1592 末=1592 -> DMA 没动
 *     请求 1600 个字，只搬了 8 个把 DAC FIFO 填满就冻住 —— 结论：
 *     **不是 DMA 的问题，是 codec 侧 DAC 数字时钟没跑**，FIFO 不消耗就不
 *     发 DMA 请求。
 * v3  补上 Zephyr pll_turn_on() 的那组使能位（走 XTAL 也一样要写）：
 *         PLL_CFG0 |= EN_IARY|EN_VCO|EN_ANA, ICP_SEL=8
 *         PLL_CFG2 |= EN_DIG          <-- 数字时钟总开关，漏的就是它
 *         PLL_CFG3 |= EN_SDM
 *         PLL_CFG4 |= EN_CLK_DIG
 *         PLL_CFG1 = 环路滤波
 *     只是**不做频率校准**（校准只服务 44.1k 系列），顺带完全绕开
 *     bf0_enable_pll() 里那个没有超时的 `while (r != 0)` 死循环。
 *     结果：CNDTR1 在变、HT 5 / TC 5、NVIC hwirq50 en=1、工作模式=DMA 中断。
 * v4  修时序 + 加开场提示音。本板 CONFIG_USEC_PER_TICK=10000，**tick 是 10ms**，
 *     usleep(5000) 会被向上取整成 10ms，把节拍拉慢一倍（8s 档口实测走了 30 多秒）。
 *     改成读 clock_systime_ticks() 累计。实测 HT=294 TC=293 TE=0，吞吐正好 16kHz。
 * v5  接 ADC（麦克风）采集通路，见下。
 *
 * v5 新增：ADC / 麦克风采集
 * =========================
 * 链路： 麦克风 M0100 (WMM7037) --MIC_BIAS/MIC_ADC_IN--> AUDCODEC 模拟 ADC1
 *        --> 数字 ADC 通道0 (ADC_CFG/ADC_CH0_CFG/ADC_CH0_ENTRY)
 *        --> DMA1 通道4（请求号 AUDCODEC_ADC0 = 39）--> 内存
 *
 * 和 DAC 的对应关系（容易绕晕，记一下）：
 *   · 模拟侧：DAC 用 DAC1/DAC2，ADC 用 ADC1/ADC2 —— 各自一对。
 *   · 数字侧：DAC 是 DAC_CH0，ADC 是 ADC_CH0，各有 _CFG / _ENTRY。
 *   · DMA 侧：DAC0 走 DMA1 通道1（req 41），ADC0 走 DMA1 通道4（req 39），
 *     和设备树里 `<&dmac 1 ... AUDAC_CH0>, <&dmac 4 ... AUDADC_CH0>` 一致。
 *
 * 16k 时钟取值（Zephyr codec_adc_clk_config 的晶振分支）：
 *   {16000, 0, 10, 1, 0, 0, 5, 2}
 *    = samplerate, clk_src_sel(0=xtal), clk_div, osr_sel,
 *      sel_clk_adc_source, sel_clk_adc, diva_clk_adc, fsp
 *   注意 ADC 的 clk_div=10、osr=1、fsp=2，和 DAC 的 clk_div=1/osr=4 完全不同，
 *   别照抄 DAC 那组。
 *
 * 有意**不调用**的两个 HAL 函数：
 *   · HAL_AUDCODEC_Config_ADCPath()  —— 它把值写到 Instance->CFG，
 *     那正是 CODEC_CFG（DAC_ENABLE/ADC_ENABLE 所在），会把总开关冲掉，
 *     属于 HAL 里的历史遗留写法。数字侧改用 Config_RChanel()（写 ADC_CFG）。
 *   · HAL_AUDCODEC_Init() 里 Config_ADCPath/Config_DACPath 那两行本来就是
 *     注释掉的，所以 Init 不会碰它们。
 *
 * 采集用**循环 DMA + 轮询读**，不给 ADC 挂中断：
 *   电平表不需要"每个半缓冲都回填"，让 DMA 自己在 2048 采样的环形缓冲里转，
 *   主循环每 100ms 读一遍整块算峰值即可。少一条中断路径 = 少一个出错面。
 *   （RX 中断仍然挂上了，但只用来数 HT/TC 做诊断，不参与数据搬运。）
 *
 * 演示：拍手响应
 *   "拍一下 -> 板子嘀一声"，一条命令同时证明**两个方向**都通：
 *     拍手要从麦克风进得来（ADC + DMA），嘀声要从喇叭出得去（DAC + 功放）。
 *   判定：峰值 > 自适应噪声底 * 6 且 > 1200 且距上次触发 > 500ms。
 *   底噪用慢速一阶跟踪（静音时快速贴下去，有声音时慢慢抬），所以房间里
 *   有持续谈笑声也不会误触发。
 *
 * 板子音频链路（原理图 + BOM）
 * ===========================
 *   麦克风 M0100 (WMM7037)  --MIC_BIAS/MIC_ADC_IN-->  SoC AUDCODEC ADC (A.36/A.37)
 *   SoC AUDCODEC DAC (A.33/A.34)  -->  NS4150B 功放 U0104  -->  喇叭 J0101
 *                                                   ^
 *                                                   |__ 使能脚 PA10（高有效）
 */

#include <nuttx/config.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <errno.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/clock.h>
#include <nuttx/sched.h>

#include "bsp_board.h"          /* -> drv_io.h, bf0_hal.h */
#include "bf0_hal_audcodec.h"
#include "dma_config.h"         /* AUDCODEC_DAC0_DMA_REQUEST / ADC0 等请求号 */
#include "drv_io.h"
#include "system_bf0_ap.h"      /* mpu_dcache_clean（本板 PSRAM_CACHE_WB 未开，展开为 0） */

/* ------------------------------------------------------------------------- */

#define AUD_RATE            (16000)
#define AUD_PA_PIN          (10)        /* PA10 -> NS4150B U0104 EN，高有效 */
#define AUD_HALF_SAMPLES    (1600)      /* 放音半缓冲 100ms @16k */
#define AUD_TX_SAMPLES      (AUD_HALF_SAMPLES * 2)

/* ---- 放音 DMA：DMAC1 通道1（请求号 AUDCODEC_DAC0 = 41）。
 *      ChannelIndex 编码是 (通道号-1)<<2，所以通道1 -> 0。 ---- */
#define AUD_DMA_CH_TX       (1)
#define AUD_DMA_IDX_TX      ((AUD_DMA_CH_TX - 1) << 2)
#define AUD_IRQ_TX          (DMAC1_CH1_IRQn + 16)   /* NuttX IRQ 号 = IRQn + 16 */

/* ---- 录音 DMA：DMAC1 通道4（请求号 AUDCODEC_ADC0 = 39），通道4 -> 12。 ---- */
/* 声学回环 / 电平表用的缓冲区大小：2048 采样（128ms @16k） */
#define AUD_RX_SAMPLES      (2048)
#define AUD_RX_HALF         (AUD_RX_SAMPLES / 2)

/* ---- 放音 PCM 环形缓冲 ----
 * 串口下行（PC 的 TTS）从一端写，DAC 的 HT/TC 回调从另一端取。
 * 取 16384 采样 = 1.024s @16k：够吸收串口和任务的抖动，又不至于让"打断"
 * 之后还要等一秒才静下来。 */
#define AUD_PCM_RING        (16384)
#define AUD_PCM_MASK        (AUD_PCM_RING - 1)   /* 必须是 2 的幂 */
#define AUD_DMA_CH_RX       (4)
#define AUD_DMA_IDX_RX      ((AUD_DMA_CH_RX - 1) << 2)
#define AUD_IRQ_RX          (DMAC1_CH4_IRQn + 16)

#define AUD_ROUNDS          (3)
#define AUD_STAGE_SEC       (6)

#define AUD_SILENCE         (0)
#define AUD_TONE            (1)
#define AUD_PCM             (2)     /* 放音内容来自 g_pcm_ring（串口下行） */

/* 采集侧数字增益（dB）。Config_RChanel 默认写的 rough_vol=0xa 正好是 0dB，
 * 也就是说采集原样进来。实测安静房间峰值只有 20~40（约 -60dBFS），
 * 直接拿来判"有没有声音"太贴地了，加一级数字增益把动态余量用起来。
 * 范围 -60~+30dB，HAL 用 rough=(v+60)/6 / fine=((v+60)%6)<<1 编码。
 * 取 +24dB（×15.8）：安静底噪 ~30 -> ~470，拍手这种瞬态能到几千，
 * 离 32767 的饱和还有 ~17dB 余量，不会一说话就削顶。
 * 注意：加太多增益也会把 ADC 自身的量化噪声一起抬起来，所以只加到够用。 */
#define AUD_MIC_GAIN_DB     (24)

/* 放音幅度（Q15，满量程 32767）。
 * 正常提示音取 8192（0.25FS，约 -12dBFS）：NS4150B 增益不小，别一上来拉满。
 * 回环自检时临时拉到 24000（0.73FS）——目的是让喇叭真的够响，好让麦克风
 * 拾到自己的声音。 */
#define AUD_AMP_NORMAL      (8192)
#define AUD_AMP_LOUD        (24000)

/* 通道1 的原始中断标志位（dmac.h）：
 *   GIF1=bit0  TCIF1=bit1  HTIF1=bit2  TEIF1=bit3
 * 注意这是"通道1 在 ISR 里的那 4 位"，不是通道号位偏移。 */
#define AUD_DMA_IFC_TC      (1u << 1)
#define AUD_DMA_IFC_HT      (1u << 2)
#define AUD_DMA_IFC_TE      (1u << 3)

/* 通道4 的对应位（GIF4=bit12 TCIF4=bit13 HTIF4=bit14 TEIF4=bit15） */
#define AUD_DMA_IFC_TC4     (1u << 13)
#define AUD_DMA_IFC_HT4     (1u << 14)
#define AUD_DMA_IFC_TE4     (1u << 15)

/* NVIC 直接读，避免依赖 arch 私有头：
 * ISER 基址 0xE000E100，ISPR 0xE000E200，一个字管 32 个 IRQ。
 * 注意这里要用**硬件** IRQ 号（DMAC1_CH1_IRQn=50 / CH4=53），不是 irq+16。 */
#define AUD_IRQ_HW_TX       ((uint32_t)DMAC1_CH1_IRQn)
#define AUD_IRQ_HW_RX       ((uint32_t)DMAC1_CH4_IRQn)
#define AUD_NVIC_ISER       (0xE000E100u)
#define AUD_NVIC_ISPR       (0xE000E200u)

/* 板级库里拿不到 arch 私有的 getreg32，直接指针读，语义一样 */
#define AUD_RD32(a)         (*(volatile uint32_t *)(uintptr_t)(a))

/* 自检开关：DAC 三段式自检（A 静音 / B 1kHz / C 八音阶）在 v4 已经验过了，
 * v5 默认不跑（跑一轮 18 秒，会盖住拍手响应）。设 1 可以回退验证。 */
#define AUD_RUN_DAC_SELFTEST   (0)

/* 诊断模式总开关。
 *   1 = 跑完整的三段式自检 + 频谱回环 + 拍手响应（bring-up 期用，看寄存器/频谱）
 *   0 = 只做硬件初始化，然后把通路交给串口语音链路（bsp_voice_link.c）
 * 产品形态下是 0；换板子/改时钟参数时切回 1 做回归。 */
#define AUD_SELFTEST_MODE      (0)

/* ------------------------------------------------------------------------- */
/* 时钟表                                                                     */
/* ------------------------------------------------------------------------- */

/* 走晶振不开 Audio PLL 的原因：HAL 的 bf0_enable_pll() 里
 *     do { r = updata_pll_freq(t); } while (r != 0);
 * 没有超时，而 updata_pll_freq 对 "16k 1000 系列"(type=2) 没有对应分支
 * （else 里的断言被注释掉了），锁不上就是死循环 + 串口无日志。
 * 晶振这条路完全绕开它。 */
#define AUD_SINC_GAIN       (0x14D)

static const AUDCODE_DAC_CLK_CONFIG_TYPE g_dac_clk_16k =
{
  .samplerate           = AUD_RATE,
  .clk_src_sel          = 0,
  .clk_div              = 1,
  .osr_sel              = 4,
  .sinc_gain            = AUD_SINC_GAIN,
  .sel_clk_dac_source   = 0,
  .diva_clk_dac         = 5,
  .diva_clk_chop_dac    = 4,
  .divb_clk_chop_dac    = 2,
  .diva_clk_chop_bg     = 20,
  .diva_clk_chop_refgen = 20,
  .sel_clk_dac          = 0,
};

/* 16k 采集：Zephyr codec_adc_clk_config 的晶振分支第 4 项 */
static const AUDCODE_ADC_CLK_CONFIG_TYPE g_adc_clk_16k =
{
  .samplerate          = AUD_RATE,
  .clk_src_sel         = 0,     /* 0 = 晶振 48M */
  .clk_div             = 10,
  .osr_sel             = 1,
  .sel_clk_adc_source  = 0,
  .sel_clk_adc         = 0,
  .diva_clk_adc        = 5,
  .fsp                 = 2,
};

/* 64 点 Q15 正弦表。查表 + 整数相位累加器，不拉 libm 的 double 版本。 */
static const int16_t g_sin64[64] =
{
       0,   3212,   6393,   9512,  12539,  15446,  18204,  20787,
   23170,  25329,  27245,  28898,  30273,  31356,  32137,  32609,
   32767,  32609,  32137,  31356,  30273,  28898,  27245,  25329,
   23170,  20787,  18204,  15446,  12539,   9512,   6393,   3212,
       0,  -3212,  -6393,  -9512, -12539, -15446, -18204, -20787,
  -23170, -25329, -27245, -28898, -30273, -31356, -32137, -32609,
  -32767, -32609, -32137, -31356, -30273, -28898, -27245, -25329,
  -23170, -20787, -18204, -15446, -12539,  -9512,  -6393,  -3212,
};

/* ------------------------------------------------------------------------- */

static AUDCODEC_HandleTypeDef g_codec;
static DMA_HandleTypeDef      g_hdma_tx;
static DMA_HandleTypeDef      g_hdma_rx;

/* DMA 按 32 位搬运，缓冲区要 4 字节对齐 */
static uint32_t g_txbuf[AUD_TX_SAMPLES * 2 / 4];
static uint32_t g_rxbuf[AUD_RX_SAMPLES * 2 / 4];

/* 下行 PCM 环形缓冲：bsp_audio_play_pcm() 写，DAC 半传输回调读。
 * 单写单读（写侧是语音链路任务，读侧是中断），所以 head/tail 各用
 * volatile uint32_t 就够了 —— 32 位读写本身是原子的。 */
static int16_t           g_pcm_ring[AUD_PCM_RING];
static volatile uint32_t g_pcm_head;      /* 写入位置（样本计，单调递增） */
static volatile uint32_t g_pcm_tail;      /* 读出位置（样本计，单调递增） */

static volatile uint8_t  g_play_type;
static volatile uint32_t g_play_freq;
static volatile int32_t  g_play_amp = AUD_AMP_NORMAL;
static uint32_t          g_phase;      /* Q16 相位，跨半缓冲连续 */

static volatile uint32_t g_ht_cnt;     /* 放音半传输中断次数 */
static volatile uint32_t g_tc_cnt;     /* 放音全传输中断次数 */
static volatile uint32_t g_te_cnt;     /* 放音传输错误次数 */
static volatile uint32_t g_rx_ht_cnt;  /* 录音半传输次数（仅诊断） */
static volatile uint32_t g_rx_tc_cnt;  /* 录音全传输次数（仅诊断） */
static volatile int      g_poll_mode;  /* 1 = 放音中断不来，改用轮询兜底 */
static uint32_t          g_mic_rd_pos; /* 采集环形缓冲的读游标（样本计） */
static volatile int      g_audio_ready;/* 通路初始化完成标志（给语音链路等） */

static void audlog(const char *fmt, ...)
{
  char msg[176];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  syslog(LOG_INFO, "[audio] %s\n", msg);
}

/* ------------------------------------------------------------------------- */
/* 缓冲区填充                                                                 */
/* ------------------------------------------------------------------------- */

/* 把第 half 个半缓冲填满。相位跨调用累加，两半接起来是连续正弦，
 * 不会在半缓冲边界出现相位跳变（听感上就是"漏拍"）。 */
static void aud_fill_half(int half)
{
  int16_t *p = (int16_t *)g_txbuf + (half * AUD_HALF_SAMPLES);
  int i;

  if (g_play_type == AUD_SILENCE)
    {
      memset(p, 0, AUD_HALF_SAMPLES * sizeof(int16_t));
      return;
    }

  if (g_play_type == AUD_PCM)
    {
      /* 从环形缓冲取。取得不够就补静音 —— 宁可短暂安静，也不要重复播旧数据
       * （重复播旧数据听感上是"卡带打嗝"，比静音难受得多）。 */
      uint32_t head = g_pcm_head;
      uint32_t tail = g_pcm_tail;
      uint32_t avail = head - tail;

      for (i = 0; i < AUD_HALF_SAMPLES; i++)
        {
          if (avail > 0)
            {
              p[i] = g_pcm_ring[tail & AUD_PCM_MASK];
              tail++;
              avail--;
            }
          else
            {
              p[i] = 0;
            }
        }

      g_pcm_tail = tail;
      return;
    }

  {
    uint32_t step = (uint32_t)(((uint64_t)g_play_freq << 16) / (uint64_t)AUD_RATE);
    uint32_t ph = g_phase;
    int32_t  amp = g_play_amp;

    for (i = 0; i < AUD_HALF_SAMPLES; i++)
      {
        p[i] = (int16_t)(((int32_t)g_sin64[(ph >> 10) & 63] * amp) >> 15);
        ph += step;
      }

    g_phase = ph;
  }
}

static void aud_flush_half(int half)
{
  mpu_dcache_clean((void *)((int16_t *)g_txbuf + half * AUD_HALF_SAMPLES),
                   AUD_HALF_SAMPLES * sizeof(int16_t));
}

/* ------------------------------------------------------------------------- */
/* 中断路径                                                                   */
/* ------------------------------------------------------------------------- */

static int aud_dma_tx_isr(int irq, void *context, void *arg)
{
  (void)irq;
  (void)context;
  (void)arg;

  /* 轮询兜底开着的时候不要两头都刷，会重复消费半缓冲 */
  if (g_poll_mode)
    {
      return OK;
    }

  HAL_DMA_IRQHandler(&g_hdma_tx);
  return OK;
}

/* 采集侧的中断只用来数数，不搬数据（环形缓冲由 DMA 自己绕）。
 * 这样万一中断有问题也不影响电平表本身。 */
static int aud_dma_rx_isr(int irq, void *context, void *arg)
{
  (void)irq;
  (void)context;
  (void)arg;

  HAL_DMA_IRQHandler(&g_hdma_rx);
  return OK;
}

/* HAL 内部的 DMA 回调最终会调这几个（HAL 里是 __weak，这里覆盖掉）。 */
void HAL_AUDCODEC_TxHalfCpltCallback(AUDCODEC_HandleTypeDef *hacodec, int cid)
{
  (void)hacodec;
  (void)cid;
  g_ht_cnt++;
  aud_fill_half(0);
  aud_flush_half(0);
}

void HAL_AUDCODEC_TxCpltCallback(AUDCODEC_HandleTypeDef *hacodec, int cid)
{
  (void)hacodec;
  (void)cid;
  g_tc_cnt++;
  aud_fill_half(1);
  aud_flush_half(1);
}

void HAL_AUDCODEC_RxHalfCpltCallback(AUDCODEC_HandleTypeDef *hacodec, int cid)
{
  (void)hacodec;
  (void)cid;
  g_rx_ht_cnt++;
}

void HAL_AUDCODEC_RxCpltCallback(AUDCODEC_HandleTypeDef *hacodec, int cid)
{
  (void)hacodec;
  (void)cid;
  g_rx_tc_cnt++;
}

/* ------------------------------------------------------------------------- */
/* DMA 探针                                                                   */
/* ------------------------------------------------------------------------- */

/* 一次性把"DMA 在不在搬 + 中断到没到"全打出来。
 *   CNDTR 是剩余传输数，DMA 在搬它就一直在变；
 *   ISR 是原始标志，哪怕中断被屏蔽，标志照样会被硬件置起来 —— 这一点很关键：
 *   标志置了但 pending 一直是 0、计数一直是 0，说明中断没派发到 NVIC/向量；
 *   标志压根不置、CNDTR 不动，说明 codec 根本没发请求。 */
static void aud_dma_dump(const char *tag)
{
  uint32_t idx = AUD_IRQ_HW_TX >> 5;
  uint32_t bit = 1u << (AUD_IRQ_HW_TX & 31u);

  audlog("[%s] TX: ISR=%08x CNDTR1=%u CCR1=%08x CSELR1=%08x",
         tag, (unsigned)DMA1->ISR, (unsigned)DMA1->CNDTR1,
         (unsigned)DMA1->CCR1, (unsigned)DMA1->CSELR1);
  /* 请求号寄存器只有 CSELR1/CSELR2 两个，每个管 4 条通道、每条 6 位：
   *   CSELR1 = C1S(b0) | C2S(b8) | C3S(b16) | C4S(b24)   -> 通道1~4
   *   CSELR2 = C5S(b0) | C6S(b8) | C7S(b16) | C8S(b24)   -> 通道5~8
   * 所以通道4 的请求号要从 CSELR1 的 bit24 取，不是"CSELR4"。 */
  audlog("[%s] RX: CNDTR4=%u CCR4=%08x CPAR4=%08x req4=%u(CSELR1.b24)",
         tag, (unsigned)DMA1->CNDTR4, (unsigned)DMA1->CCR4,
         (unsigned)DMA1->CPAR4,
         (unsigned)((DMA1->CSELR1 >> 24) & 0x3Fu));
  audlog("[%s] NVIC hwirq%u en=%d pend=%d (放音 NuttX irq%d)",
         tag, (unsigned)AUD_IRQ_HW_TX,
         (AUD_RD32(AUD_NVIC_ISER + idx * 4) & bit) ? 1 : 0,
         (AUD_RD32(AUD_NVIC_ISPR + idx * 4) & bit) ? 1 : 0,
         (int)AUD_IRQ_TX);
  audlog("[%s] 计数 TX HT=%u TC=%u TE=%u | RX HT=%u TC=%u",
         tag, (unsigned)g_ht_cnt, (unsigned)g_tc_cnt, (unsigned)g_te_cnt,
         (unsigned)g_rx_ht_cnt, (unsigned)g_rx_tc_cnt);
}

/* 采样 CNDTR1 看放音 DMA 有没有在搬。160ms 窗口 @16k/3200 计数，
 * 一个半缓冲 100ms，正常情况下计数必定变过。 */
static int aud_dma_watch(void)
{
  uint32_t first = DMA1->CNDTR1;
  uint32_t last;
  int i;

  for (i = 0; i < 8; i++)
    {
      usleep(20 * 1000);
    }

  last = DMA1->CNDTR1;

  audlog("CNDTR1 采样: 首=%u 末=%u -> %s",
         (unsigned)first, (unsigned)last,
         (first != last) ? "DMA 在搬（问题在中断派发）"
                         : "DMA 没动（codec 没发 DMA 请求）");
  return (first != last) ? 1 : 0;
}

/* 轮询兜底：直接读硬件标志、自己清、自己回填。不依赖 NVIC/向量表。 */
static void aud_poll_service(void)
{
  uint32_t isr = DMA1->ISR;

  if (isr & AUD_DMA_IFC_TE)
    {
      DMA1->IFCR = AUD_DMA_IFC_TE;
      g_te_cnt++;
    }

  if (isr & AUD_DMA_IFC_HT)
    {
      DMA1->IFCR = AUD_DMA_IFC_HT;
      g_ht_cnt++;
      aud_fill_half(0);
      aud_flush_half(0);
    }

  if (isr & AUD_DMA_IFC_TC)
    {
      DMA1->IFCR = AUD_DMA_IFC_TC;
      g_tc_cnt++;
      aud_fill_half(1);
      aud_flush_half(1);
    }
}

/* 播放期间的时间推进。
 *
 * 为什么不用 `usleep(ms)`：本板 CONFIG_USEC_PER_TICK=10000，也就是 **tick 是
 * 10ms**。usleep(5000) 会被向上取整成 1 个 tick = 10ms，直接把节拍拉慢一倍；
 * 再加上 CONFIG_RR_INTERVAL=10（100ms 时间片）和同时跑着的显示自检，
 * v3 实测 8 秒的档口实际走了 30 多秒。
 * 所以这里改成读系统 tick 累计来判断"真的过够时间了"，让日志里写 8s 就真是 8s。
 * 顺带每次循环（约 1 tick = 10ms）服务一次 DMA —— 对 100ms 的半缓冲余量很大。 */
#define AUD_MS_PER_TICK     (USEC_PER_TICK / 1000)

static void aud_delay_ms(uint32_t ms)
{
  uint32_t start = (uint32_t)clock_systime_ticks();
  uint32_t ticks = ms / AUD_MS_PER_TICK;

  if (ticks == 0)
    {
      ticks = 1;
    }

  while (((uint32_t)clock_systime_ticks() - start) < ticks)
    {
      if (g_poll_mode)
        {
          aud_poll_service();
        }

      usleep(1000);      /* 纯让路；粒度由 tick 决定 */
    }
}

/* ------------------------------------------------------------------------- */
/* 寄存器回读                                                                 */
/* ------------------------------------------------------------------------- */

static void aud_dump_regs(const char *tag)
{
  uint32_t pll0 = (unsigned)hwp_audcodec->PLL_CFG0;
  uint32_t pll2 = (unsigned)hwp_audcodec->PLL_CFG2;

  audlog("[%s] ID=%08x CFG=%08x PLL_STAT=%08x", tag,
         (unsigned)hwp_audcodec->ID,
         (unsigned)hwp_audcodec->CFG,
         (unsigned)hwp_audcodec->PLL_STAT);
  audlog("[%s] BG_CFG0=%08x REFGEN_CFG=%08x DAC_CFG=%08x",
         tag, (unsigned)hwp_audcodec->BG_CFG0,
         (unsigned)hwp_audcodec->REFGEN_CFG,
         (unsigned)hwp_audcodec->DAC_CFG);
  audlog("[%s] DAC_CH0_CFG=%08x DAC1_CFG=%08x", tag,
         (unsigned)hwp_audcodec->DAC_CH0_CFG,
         (unsigned)hwp_audcodec->DAC1_CFG);
  audlog("[%s] ADC_CFG=%08x ADC_CH0_CFG=%08x ADC_ANA_CFG=%08x",
         tag, (unsigned)hwp_audcodec->ADC_CFG,
         (unsigned)hwp_audcodec->ADC_CH0_CFG,
         (unsigned)hwp_audcodec->ADC_ANA_CFG);
  audlog("[%s] ADC1_CFG1=%08x ADC1_CFG2=%08x PLL_CFG6=%08x", tag,
         (unsigned)hwp_audcodec->ADC1_CFG1,
         (unsigned)hwp_audcodec->ADC1_CFG2,
         (unsigned)hwp_audcodec->PLL_CFG6);
  audlog("[%s] PLL_CFG0=%08x CFG1=%08x CFG2=%08x CFG3=%08x CFG4=%08x CFG5=%08x",
         tag, (unsigned)pll0,
         (unsigned)hwp_audcodec->PLL_CFG1, (unsigned)pll2,
         (unsigned)hwp_audcodec->PLL_CFG3,
         (unsigned)hwp_audcodec->PLL_CFG4,
         (unsigned)hwp_audcodec->PLL_CFG5);
  audlog("[%s] 关键位: EN_DIG(CFG2.b13)=%d EN_SDM(CFG3.b30)=%d "
         "EN_CLK_DIG(CFG4.b23)=%d EN_ANA(CFG0.b15)=%d | "
         "ADC_ENABLE(CFG.b0)=%d DAC_ENABLE(CFG.b1)=%d "
         "ADC_CH0_EN(b0)=%d ADC_CH0_DMA_EN(b4)=%d",
         tag,
         (pll2 & AUDCODEC_PLL_CFG2_EN_DIG) ? 1 : 0,
         ((unsigned)hwp_audcodec->PLL_CFG3 & AUDCODEC_PLL_CFG3_EN_SDM) ? 1 : 0,
         ((unsigned)hwp_audcodec->PLL_CFG4 & AUDCODEC_PLL_CFG4_EN_CLK_DIG) ? 1 : 0,
         (pll0 & AUDCODEC_PLL_CFG0_EN_ANA) ? 1 : 0,
         (((unsigned)hwp_audcodec->CFG & AUDCODEC_CFG_ADC_ENABLE) ? 1 : 0),
         (((unsigned)hwp_audcodec->CFG & AUDCODEC_CFG_DAC_ENABLE) ? 1 : 0),
         (((unsigned)hwp_audcodec->ADC_CH0_CFG & AUDCODEC_ADC_CH0_CFG_ENABLE) ? 1 : 0),
         (((unsigned)hwp_audcodec->ADC_CH0_CFG & AUDCODEC_ADC_CH0_CFG_DMA_EN) ? 1 : 0));
}

/* ------------------------------------------------------------------------- */
/* 频谱诊断（Goertzel）                                                       */
/* ------------------------------------------------------------------------- */

/* 光看"峰值"没法判断麦克风到底听没听到声音：底噪 690、放音时 1005，看着像
 * 抬了 1.46 倍，但这也是"宽带噪声涨了一点"的典型样子，和"真拾到一个 1kHz
 * 正弦"完全是两回事。
 *
 * 判据要用**频谱**：如果麦克风真听到喇叭的 1kHz，那么 1kHz 这一根谱线会单独
 * 窜起来；如果只是电噪声涨了，1kHz 和旁边 750/1250Hz 会一起涨、比例相当。
 * 所以这里只算三根谱线的能量，比较"放音时/静音时"的抬升倍数：
 *   · 1kHz 的抬升 >> 750/1250Hz 的抬升  ->  麦克风真的听到了
 *   · 三根一起抬、倍数差不多          ->  只是本底噪声变化，不算听到
 *
 * 用 Goertzel 而不是 FFT：只要三根谱线，Goertzel 每根 O(N)、无蝶形、无旋转
 * 因子表，代码量小得多，也就不用把 libm / 定点 FFT 拉进来。
 *
 * 窗长取 1024 采样（64ms @16k），此时 bin 间隔 15.625Hz，1kHz 正好落在
 * 第 64 根（16k*64/1024 = 1000.0Hz），整数对齐、没有泄漏修正的麻烦。 */
#define AUD_GK_N          (1024)
#define AUD_GK_K_750      (48)      /* 750Hz   */
#define AUD_GK_K_1000     (64)      /* 1000Hz  */
#define AUD_GK_K_1250     (80)      /* 1250Hz  */

/* 系数 2*cos(2*pi*k/N)，Q15 */
#define AUD_GK_COEF_750   (62716)
#define AUD_GK_COEF_1000  (60548)
#define AUD_GK_COEF_1250  (57801)

/* 返回该频点能量（任意单位，已 >>26 归一）。
 * 全程 int64：s1/s2 最大约 6e7，s1*s1 约 4e15，都还在 int64 里，
 * 但 coef*s1*s2 约 2e20 会溢出，所以先 (coef*s1)>>15 再乘 s2。 */
static uint32_t aud_goertzel(const int16_t *p, int n, int32_t coef)
{
  int64_t s1 = 0;
  int64_t s2 = 0;
  int     i;
  int64_t v;

  for (i = 0; i < n; i++)
    {
      int64_t s0 = (int64_t)p[i] + (((int64_t)coef * s1) >> 15) - s2;
      s2 = s1;
      s1 = s0;
    }

  v = s1 * s1 + s2 * s2 - ((((int64_t)coef * s1) >> 15) * s2);
  if (v < 0)
    {
      v = -v;
    }

  return (uint32_t)((uint64_t)v >> 20);
}

/* 取一个"无接缝"的 1024 采样窗口：
 * 环形缓冲是 2 个半缓冲（各 1024 采样），在 HT 中断发生的瞬间前半刚填满、
 * DMA 正在写后半 —— 这时读前半 [0,1024)，数据完整且不会被改写（有 64ms 余量）。
 * 不这么做直接读整块的话，会跨过 DMA 的写指针，波形中间出现一个跳变接缝，
 * 频谱上就是一坨宽带杂散，Goertzel 的判据会被糊掉。 */
static void aud_grab_window(int16_t *dst)
{
  uint32_t t0 = g_rx_ht_cnt;
  uint32_t guard = 0;

  while (g_rx_ht_cnt == t0 && guard < 200)
    {
      usleep(1000);              /* 最多等 200ms，HT 每 64ms 来一次 */
      guard++;
    }

  mpu_dcache_invalidate((void *)g_rxbuf, sizeof(g_rxbuf));
  memcpy(dst, g_rxbuf, AUD_GK_N * sizeof(int16_t));
}

/* 原始样本回显：峰值/最大/最小/均值 + 前 8 个样本。
 * 用来分辨"数据是活的噪声"还是"其实是常数/半字错位"：
 *   · 全 0            -> ADC 没在送数
 *   · 一堆相同的常数   -> FIFO 卡住 / 通道没使能
 *   · 0 和 ±1 交替     -> 只有最低位在动，模拟侧基本没信号
 *   · 小幅度随机       -> 有真实噪声，模拟通路是通的 */
static void aud_mic_raw(const char *tag, const int16_t *p, int n)
{
  int32_t  vmin = 32767, vmax = -32768;
  int64_t  sum = 0;
  int      i;

  for (i = 0; i < n; i++)
    {
      if (p[i] < vmin) vmin = p[i];
      if (p[i] > vmax) vmax = p[i];
      sum += p[i];
    }

  audlog("[%s] 原始样本: min=%d max=%d 均值=%d | 前8个 %d %d %d %d %d %d %d %d",
         tag, (int)vmin, (int)vmax, (int)(sum / n),
         p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
}

/* ------------------------------------------------------------------------- */
/* 硬件初始化                                                                 */
/* ------------------------------------------------------------------------- */

static int aud_hw_init(void)
{
  HAL_StatusTypeDef ret;
  uint32_t bg;

  memset(&g_codec, 0, sizeof(g_codec));
  memset(&g_hdma_tx, 0, sizeof(g_hdma_tx));
  memset(&g_hdma_rx, 0, sizeof(g_hdma_rx));

  /* ---- 1) 音频子系统上电 ----
   * 对应 bf0_enable_pll() 的 step 1（也是 Zephyr 里 pmu_enable_audio(1)
   * + clock_control_on 干的事）：开 48M 晶振到 Audio 的缓冲，再放行
   * AUDCODEC 的时钟门。 */
  hwp_pmuc->HXT_CR1 |= PMUC_HXT_CR1_BUF_AUD_EN;
  hwp_hpsys_rcc->ENR2 |= HPSYS_RCC_ENR2_AUDCODEC;
  audlog("音频时钟已使能（HXT BUF_AUD + AUDCODEC 门控）");

  /* ---- 2) bandgap / 参考源 ----
   * 值抄树内 HAL_TURN_ON_PLL() step 2：VREF_SEL=0xc 表示 AVDD=3.3V
   * （本板 NS4150B 用 3.3V），MIC_VREF_SEL=4，开 RC 滤波和 BG 放大器，
   * BG_CFG1/2 清 0 是官方注释里标了 "pop noise" 的做法。 */
  bg = (1   << AUDCODEC_BG_CFG0_EN_Pos)        |
       (0   << AUDCODEC_BG_CFG0_LP_MODE_Pos)   |
       (0xc << AUDCODEC_BG_CFG0_VREF_SEL_Pos)  |
       (0   << AUDCODEC_BG_CFG0_EN_SMPL_Pos)   |
       (1   << AUDCODEC_BG_CFG0_EN_RCFLT_Pos)  |
       (4   << AUDCODEC_BG_CFG0_MIC_VREF_SEL_Pos) |
       (1   << AUDCODEC_BG_CFG0_EN_AMP_Pos)    |
       (0   << AUDCODEC_BG_CFG0_SET_VC_Pos);
  hwp_audcodec->BG_CFG0 = bg;
  hwp_audcodec->BG_CFG1 = 0;
  hwp_audcodec->BG_CFG2 = 0;
  HAL_Delay_us(100);

  /* ---- 2b) PLL/数字时钟块的使能位（**v1 漏掉的关键一步**） ----
   *
   * 走 XTAL 不等于"不用管 PLL 寄存器"。Zephyr 的 pll_turn_on() 在
   * clk_src_sel==0（XTAL）时**照样**会写这一组寄存器，只是**不做频率校准**
   * （校准只给 44.1k 系列用）。漏掉它们的具体后果，v1 实测到的现象是：
   *   DMA 起了、DMA_EN 置了、CSELR 也对，但 CNDTR1 搬了 8 个字就冻住
   *   —— DAC 的 FIFO 被填满后再没人取走，说明**DAC 的数字时钟没在跑**。
   * 其中 EN_DIG(PLL_CFG2 bit13) 就是那个数字时钟总开关。
   * 这一组对 ADC 同样必需（数字 ADC 通道共用这套数字时钟）。
   *
   * 顺序完全照 pll_turn_on()：PLL_CFG0 模拟偏置 -> CFG2 数字使能 ->
   * CFG3 SDM -> CFG4 数字时钟 -> CFG1 环路滤波 -> 等 50us。
   * 注意这里刻意**不设** PLL_CFG0 的 OPEN 位、不跑 VCO 校准、
   * 不动 PLL_CFG3 的 SDM 分频 —— 因为 sel_clk_*_source=0 时时钟取自晶振，
   * PLL 只是给模拟电路提供偏置，不需要锁频。 */
  hwp_audcodec->PLL_CFG0 |= AUDCODEC_PLL_CFG0_EN_IARY;
  hwp_audcodec->PLL_CFG0 |= AUDCODEC_PLL_CFG0_EN_VCO;
  hwp_audcodec->PLL_CFG0 |= AUDCODEC_PLL_CFG0_EN_ANA;
  hwp_audcodec->PLL_CFG0 &= ~AUDCODEC_PLL_CFG0_ICP_SEL_Msk;
  hwp_audcodec->PLL_CFG0 |= (8 << AUDCODEC_PLL_CFG0_ICP_SEL_Pos);

  hwp_audcodec->PLL_CFG2 |= AUDCODEC_PLL_CFG2_EN_DIG;
  hwp_audcodec->PLL_CFG3 |= AUDCODEC_PLL_CFG3_EN_SDM;
  hwp_audcodec->PLL_CFG4 |= AUDCODEC_PLL_CFG4_EN_CLK_DIG;

  hwp_audcodec->PLL_CFG1 = (3 << AUDCODEC_PLL_CFG1_R3_SEL_Pos) |
                           (1 << AUDCODEC_PLL_CFG1_RZ_SEL_Pos) |
                           (3 << AUDCODEC_PLL_CFG1_C2_SEL_Pos) |
                           (6 << AUDCODEC_PLL_CFG1_CZ_SEL_Pos) |
                           (0 << AUDCODEC_PLL_CFG1_CSD_RST_Pos) |
                           (0 << AUDCODEC_PLL_CFG1_CSD_EN_Pos);
  HAL_Delay_us(50);
  audlog("PLL/数字块使能位已置（EN_DIG/SDM/CLK_DIG + 模拟偏置，未做锁频校准）");

  /* ---- 2c) refgen ----
   * HAL_TURN_ON_PLL() 末尾会调 HAL_AUCODEC_Refgen_Init() 打开 REFGEN_CFG，
   * 而 HAL_AUDCODEC_Config_Analog_DACPath() 里那段 refgen 是被注释掉的，
   * 所以得自己补。顺序照 HAL_AUCODEC_Refgen_Init。 */
  hwp_audcodec->BG_CFG0 &= ~AUDCODEC_BG_CFG0_EN_SMPL;
  hwp_audcodec->REFGEN_CFG &= ~AUDCODEC_REFGEN_CFG_EN_CHOP;
  hwp_audcodec->REFGEN_CFG |= AUDCODEC_REFGEN_CFG_EN;
  hwp_audcodec->REFGEN_CFG &= ~AUDCODEC_REFGEN_CFG_LV_MODE;
  hwp_audcodec->PLL_CFG5 |= (1 << AUDCODEC_PLL_CFG5_EN_CLK_CHOP_BG_Pos) |
                            (1 << AUDCODEC_PLL_CFG5_EN_CLK_CHOP_REFGEN_Pos);
  HAL_Delay_us(2000);
  audlog("REFGEN 使能（模拟参考源）");

  /* ---- 3) DMA 句柄 ----
   * Instance/Request/ChannelIndex/DmaBaseAddress/IrqPrio 由调用方填 —— HAL 只把
   * 这些写进 CSELR/CCR，方向/位宽/循环模式由 HAL_AUDCODEC_DMA_Init() 设好
   * （放音 MEMM2PERIPH，录音 PERIPH2MEM，都是 32 位 + 循环 + MINC）。 */
  g_hdma_tx.Instance       = DMA1_Channel1;
  g_hdma_tx.Init.Request   = AUDCODEC_DAC0_DMA_REQUEST;
  g_hdma_tx.ChannelIndex   = AUD_DMA_IDX_TX;
  g_hdma_tx.DmaBaseAddress = DMA1;
  g_hdma_tx.Init.IrqPrio   = 5;

  g_hdma_rx.Instance       = DMA1_Channel4;
  g_hdma_rx.Init.Request   = AUDCODEC_ADC0_DMA_REQUEST;
  g_hdma_rx.ChannelIndex   = AUD_DMA_IDX_RX;
  g_hdma_rx.DmaBaseAddress = DMA1;
  g_hdma_rx.Init.IrqPrio   = 5;

  /* ---- 4) codec 初始化 ----
   * 两个 DMA 句柄都登记上。Init 里只会给 DAC 那几个走 from_mem=1
   * （内存->外设），ADC 那几个走 from_mem=0（外设->内存），顺序自动分好。 */
  g_codec.Instance              = hwp_audcodec;
  g_codec.Init.en_dly_sel       = 0;
  g_codec.Init.samplerate_index = 3;      /* 3 = 16k */
  g_codec.Init.dac_cfg.opmode   = 1;      /* 1 = 内存直连 codec，不经 audprc */
  g_codec.Init.dac_cfg.dac_clk  = (AUDCODE_DAC_CLK_CONFIG_TYPE *)&g_dac_clk_16k;
  g_codec.Init.adc_cfg.opmode   = 1;
  g_codec.Init.adc_cfg.adc_clk  = (AUDCODE_ADC_CLK_CONFIG_TYPE *)&g_adc_clk_16k;
  g_codec.hdma[HAL_AUDCODEC_DAC_CH0] = &g_hdma_tx;
  g_codec.hdma[HAL_AUDCODEC_ADC_CH0] = &g_hdma_rx;

  ret = HAL_AUDCODEC_Init(&g_codec);
  audlog("HAL_AUDCODEC_Init -> %d", (int)ret);
  if (ret != HAL_OK)
    {
      return -1;
    }

  /* ---- 5) 通道配置 ----
   * TChanel 写 DAC_CFG / DAC_CH0_CFG / _EXT / _DEBUG；
   * RChanel 写 ADC_CFG / ADC_CH0_CFG。两个都会顺手把 CODEC_CFG 的
   * ADC_EN_DLY_SEL 设成 3（官方说 ADC->DAC 固定延时靠它）。 */
  ret = HAL_AUDCODEC_Config_TChanel(&g_codec, 0, &g_codec.Init.dac_cfg);
  audlog("Config_TChanel -> %d", (int)ret);
  if (ret != HAL_OK)
    {
      return -1;
    }

  ret = HAL_AUDCODEC_Config_RChanel(&g_codec, 0, &g_codec.Init.adc_cfg);
  audlog("Config_RChanel -> %d", (int)ret);
  if (ret != HAL_OK)
    {
      return -1;
    }

  /* RChanel 里 rough/fine 是写死的（rough=0xa -> 0dB），所以增益要在这之后
   * 单独设一次。放在 RChanel 之后是必须的：它是整写 ADC_CH0_CFG 的。 */
  ret = HAL_AUDCODEC_Config_ADCPath_Volume(&g_codec, 0, AUD_MIC_GAIN_DB);
  audlog("采集增益设为 %+d dB -> %d", AUD_MIC_GAIN_DB, (int)ret);

  /* ---- 6) 起播：先塞静音再起 DMA ----
   * 反过来开头几个字是随机内容，听感上就是一声"啪"。 */
  g_play_type = AUD_SILENCE;
  g_phase     = 0;
  aud_fill_half(0);
  aud_fill_half(1);
  mpu_dcache_clean((void *)g_txbuf, sizeof(g_txbuf));

  ret = HAL_AUDCODEC_Transmit_DMA(&g_codec,
                                  (uint8_t *)g_txbuf, sizeof(g_txbuf),
                                  HAL_AUDCODEC_DAC_CH0);
  audlog("Transmit_DMA（%d 字节 / 2x%d 采样，循环）-> %d",
         (int)sizeof(g_txbuf), AUD_HALF_SAMPLES, (int)ret);
  if (ret != HAL_OK)
    {
      return -1;
    }

  /* ---- 7) 放音 DMA 中断挂载 ----
   * 放在 Transmit_DMA 之后：这套 HAL 的 DMA 通道是动态分配的
   * （bf0_hal_dma.h 里 DMA_SUPPORT_DYN_CHANNEL_ALLOC 是定义着的）。
   * HAL_DMA_Start_IT -> DMA_AllocChannel 会把 hdma->ChannelIndex 当"想要哪个
   * 通道"，被占了就改 Instance/ChannelIndex 另找，中断号跟着变。
   * 所以不能先按"通道1"把向量挂死 —— 挂错不但收不到中断，还可能把别人挂好的
   * ISR 顶掉。起播后再读实际分配到的通道，按它挂。 */
  {
    int ch = (int)(g_hdma_tx.ChannelIndex >> 2);
    int irq = (int)DMAC1_CH1_IRQn + ch + 16;

    audlog("放音 DMA 实际分配到通道 %d（请求号 %u），IRQ %d",
           ch + 1, (unsigned)g_hdma_tx.Init.Request, irq);

    if (irq_attach(irq, aud_dma_tx_isr, NULL) == OK)
      {
        up_enable_irq(irq);
        audlog("放音 DMA TX irq %d 已挂载", irq);
      }
    else
      {
        audlog("放音 DMA TX irq 挂载失败，放音不会有回调（音会断）");
      }
  }

  /* ---- 8) 模拟放音通路加电 ----
   * 顺序照 Zephyr codec_start：先 mute -> 模拟通路加电 -> 解除 mute。
   * Config_Analog_DACPath 内部按 VCM -> AMP -> OS_DAC -> DAC 逐步加电，
   * 每步之间有微秒级等待，顺序错了会听到明显的"啪"。 */
  HAL_AUDCODEC_Config_DACPath(&g_codec, 1);

  ret = HAL_AUDCODEC_Config_Analog_DACPath(
          (AUDCODE_DAC_CLK_CONFIG_TYPE *)&g_dac_clk_16k);
  audlog("Config_Analog_DACPath -> %d", (int)ret);

  HAL_AUDCODEC_Config_DACPath(&g_codec, 0);

  /* ---- 9) 开 DAC 总开关，最后才开功放 ----
   * 功放最后开、最先关，避免上电瞬态被放大。 */
  __HAL_AUDCODEC_DAC_ENABLE(&g_codec);

  BSP_GPIO_Set(AUD_PA_PIN, 1, 1);
  usleep(10 * 1000);                     /* Zephyr 里等 10ms 功放稳定 */
  audlog("PA%d 使能（功放 NS4150B）", AUD_PA_PIN);

  /* ---- 10) 采集通路 ----
   * 顺序照 Zephyr codec_start 的 start_rx 分支：
   *   DMA 起 -> 置 ADC_CH0 的 DMA_EN -> 模拟 ADC 通路加电 -> 最后开 ADC 总开关。
   * "最后开"是有讲究的：ADC 和 DAC 之间要有固定延时（回声消除算法要），
   * 而且先开 ADC 会采到模拟通路自己上电的爆音。 */
  memset(g_rxbuf, 0, sizeof(g_rxbuf));
  mpu_dcache_clean((void *)g_rxbuf, sizeof(g_rxbuf));

  ret = HAL_AUDCODEC_Receive_DMA(&g_codec,
                                 (uint8_t *)g_rxbuf, sizeof(g_rxbuf),
                                 HAL_AUDCODEC_ADC_CH0);
  audlog("Receive_DMA（%d 字节 / %d 采样，循环）-> %d",
         (int)sizeof(g_rxbuf), AUD_RX_SAMPLES, (int)ret);
  if (ret != HAL_OK)
    {
      return -1;
    }

  /* 采集 DMA 也用动态分配，同样起播后再读实际通道 */
  {
    int ch = (int)(g_hdma_rx.ChannelIndex >> 2);
    int irq = (int)DMAC1_CH1_IRQn + ch + 16;

    audlog("录音 DMA 实际分配到通道 %d（请求号 %u），IRQ %d",
           ch + 1, (unsigned)g_hdma_rx.Init.Request, irq);

    if (irq_attach(irq, aud_dma_rx_isr, NULL) == OK)
      {
        up_enable_irq(irq);
        audlog("录音 DMA RX irq %d 已挂载（仅计数）", irq);
      }
    else
      {
        audlog("录音 DMA RX irq 挂载失败（不影响电平表，只是没计数）");
      }
  }

  /* 模拟 ADC 通路：这一步开 MICBIAS（麦克风偏置）并配 ADC1/ADC2 的时钟，
   * 内部含 2ms 偏置稳定 + 20ms 复位释放，合计 20 多毫秒。
   * 用 ROM 里这个完整版，不自己拼（自己拼容易漏 RSTB/VCMST 那几步的先后）。 */
  HAL_AUDCODEC_Config_Analog_ADCPath(
          (AUDCODE_ADC_CLK_CONFIG_TYPE *)&g_adc_clk_16k);
  audlog("Config_Analog_ADCPath 完成（MICBIAS + ADC1/ADC2 加电）");

  __HAL_AUDCODEC_ADC_ENABLE(&g_codec);
  HAL_Delay_us(200);
  audlog("ADC 总开关已开");

  return 0;
}

/* ------------------------------------------------------------------------- */
/* 麦克风电平表                                                               */
/* ------------------------------------------------------------------------- */

/* 读整块环形缓冲算峰值。缓冲才 2048 采样（128ms），DMA 在搬的时候读会读到
 * 半新半旧的数据 —— 对电平表无所谓，峰值的量级不受影响。
 *
 * 顺手统计一下"非零样本数"：如果 ADC 通路没真跑起来（比如时钟没给），
 * DMA 会一直搬同一份旧数据或全 0，这时非零比例会明显异常，一眼能看出来。 */
static uint32_t aud_mic_peak(uint32_t *nonzero_out)
{
  const int16_t *p = (const int16_t *)g_rxbuf;
  uint32_t peak = 0;
  uint32_t nz = 0;
  int i;

  /* DMA 往内存写，读之前把 cache 无效化（本板展开为 0，留着以防以后开缓存） */
  mpu_dcache_invalidate((void *)g_rxbuf, sizeof(g_rxbuf));

  for (i = 0; i < AUD_RX_SAMPLES; i++)
    {
      int32_t v = p[i];

      if (v != 0)
        {
          nz++;
        }

      if (v < 0)
        {
          v = -v;
        }

      if ((uint32_t)v > peak)
        {
          peak = (uint32_t)v;
        }
    }

  if (nonzero_out != NULL)
    {
      *nonzero_out = nz;
    }

  return peak;
}

/* ------------------------------------------------------------------------- */
/* DAC 三段式自检（v4 验证过，默认关，留作回归）                              */
/* ------------------------------------------------------------------------- */

static const uint32_t g_scale[] = {523, 587, 659, 784, 880, 784, 659, 587};

static void aud_play(uint8_t type, uint32_t freq)
{
  g_play_freq = freq;
  g_play_type = type;
}

#if AUD_RUN_DAC_SELFTEST
static void aud_stage_a(int round)
{
  audlog("round %d STAGE A: 通路开、静音 %d s（听底噪/上电声）",
         round, AUD_STAGE_SEC);
  aud_play(AUD_SILENCE, 0);
  aud_delay_ms(AUD_STAGE_SEC * 1000);
}

static void aud_stage_b(int round)
{
  audlog("round %d STAGE B: 1kHz 单音 %d s", round, AUD_STAGE_SEC);
  aud_play(AUD_TONE, 1000);
  aud_delay_ms(AUD_STAGE_SEC * 1000);
}

static void aud_stage_c(int round)
{
  int i;

  audlog("round %d STAGE C: 八音阶循环 %d s（听音高变化）",
         round, AUD_STAGE_SEC);
  for (i = 0; i < AUD_STAGE_SEC * 4; i++)
    {
      aud_play(AUD_TONE, g_scale[i % 8]);
      aud_delay_ms(250);
    }
}
#endif /* AUD_RUN_DAC_SELFTEST */

/* 短促"嘀"一声。用阻塞式补齐整个时长，保证音频不会半截被下一句打断。 */
static void aud_beep(uint32_t freq, uint32_t ms)
{
  aud_play(AUD_TONE, freq);
  aud_delay_ms(ms);
  aud_play(AUD_SILENCE, 0);
  /* 静音也要真放出去，否则尾音直接被下一个音替掉，听感上是"啪" */
  aud_delay_ms(30);
}

/* ------------------------------------------------------------------------- */
/* 对外 API（给 bsp_voice_link.c 用）                                         */
/* ------------------------------------------------------------------------- */

/* 只做硬件初始化，不起自检线程。返回 0 表示放音+采集两条通路都起来了。 */
int bsp_audio_hw_init(void)
{
  return aud_hw_init();
}

void bsp_audio_dump(const char *tag)
{
  aud_dump_regs(tag);
  aud_dma_dump(tag);
}

/* 放音：把下行 PCM 推进环形缓冲。
 * 非阻塞：缓冲不够就只写进去能写的部分，返回实际写进去的样本数。
 * 上层（串口链路）按这个返回值记丢样，不做等待 —— 语音宁可丢一点也不要卡住
 * 收数据的循环，一卡住上行也跟着堵，通话就僵了。 */
int bsp_audio_play_pcm(const int16_t *pcm, int count)
{
  uint32_t head = g_pcm_head;
  uint32_t tail = g_pcm_tail;
  uint32_t free_n = AUD_PCM_RING - (head - tail);
  int      n;

  if (count <= 0)
    {
      return 0;
    }

  if ((uint32_t)count > free_n)
    {
      count = (int)free_n;
    }

  for (n = 0; n < count; n++)
    {
      g_pcm_ring[head & AUD_PCM_MASK] = pcm[n];
      head++;
    }

  g_pcm_head = head;

  /* 有数据在流就切到 PCM 模式；播放端由 DAC 回调按需取 */
  if (count > 0)
    {
      g_play_type = AUD_PCM;
    }

  return count;
}

/* 清空下行缓冲并停到静音。用户打断 AI 说话（CMD_PLAY_STOP）时调。 */
void bsp_audio_play_clear(void)
{
  g_pcm_head = 0;
  g_pcm_tail = 0;
  g_play_type = AUD_SILENCE;
}

/* 环形缓冲里还有多少样本没被 DAC 取走（用于估计"还要说多久"） */
int bsp_audio_play_pending(void)
{
  return (int)(g_pcm_head - g_pcm_tail);
}

/* 功放使能（PA10）。关掉能省电，也能在待机时彻底断掉喇叭的底噪。 */
void bsp_audio_set_amp(int on)
{
  BSP_GPIO_Set(AUD_PA_PIN, on ? 1 : 0, 1);
}

/* 放音音量 0..100 -> DAC 通路 -36..+6 dB（留 6dB 余量，别一上来就顶满）。
 * 编码用 HAL 的 Config_DACPath_Volume，内部按 (dB+36)/6 拆 rough/fine。 */
void bsp_audio_set_volume_pct(int pct)
{
  int db;

  if (pct < 0)   pct = 0;
  if (pct > 100) pct = 100;

  db = -36 + (pct * 42) / 100;         /* 0 -> -36dB, 100 -> +6dB */
  HAL_AUDCODEC_Config_DACPath_Volume(&g_codec, 0, db);
}

/* 本地提示音：不进 PCM 环形缓冲，直接切合成模式。
 * 音高和时长固定由调用方给，PC 端只要发编号。 */
void bsp_audio_tone(uint32_t freq, uint32_t ms)
{
  g_play_amp = AUD_AMP_NORMAL;
  aud_beep(freq, ms);
  g_play_type = AUD_SILENCE;
}

/* 采集：从环形缓冲取"已经完整采到"的样本。
 *
 * 数据源是 DMA 自己在 2048 采样的环形缓冲里绕圈，我们靠 RX 的 HT/TC 计数
 * 算"一共产生了多少样本"，再减去已经取走的量。
 * 只取到"已产生的"，绝不碰 DMA 正在写的那一块 —— 那会读到半新半旧的拼接数据。
 * 上游取慢了就丢旧的（avail 超过一整圈时把读指针推到最新），
 * 语音上行丢一点旧数据比送出错位的数据好。 */
int bsp_audio_mic_read(int16_t *dst, int max)
{
  uint32_t produced = (g_rx_ht_cnt + g_rx_tc_cnt) * (uint32_t)AUD_RX_HALF;
  uint32_t rd       = g_mic_rd_pos;
  uint32_t avail    = produced - rd;
  int      n;
  int      i;

  if (max <= 0)
    {
      return 0;
    }

  if (avail > (uint32_t)AUD_RX_SAMPLES)
    {
      /* 取太慢，追不上：跳到"最新一整圈"的起点，丢掉的样本不再补 */
      rd    = produced - (uint32_t)AUD_RX_SAMPLES;
      avail = (uint32_t)AUD_RX_SAMPLES;
    }

  n = (avail > (uint32_t)max) ? max : (int)avail;
  if (n <= 0)
    {
      return 0;
    }

  mpu_dcache_invalidate((void *)g_rxbuf, sizeof(g_rxbuf));

  for (i = 0; i < n; i++)
    {
      dst[i] = ((const int16_t *)g_rxbuf)[(rd + (uint32_t)i) & (AUD_RX_SAMPLES - 1)];
    }

  g_mic_rd_pos = rd + (uint32_t)n;
  return n;
}

/* 采集侧计数（诊断用：确认采集在跑） */
void bsp_audio_mic_stats(uint32_t *ht, uint32_t *tc)
{
  if (ht) *ht = g_rx_ht_cnt;
  if (tc) *tc = g_rx_tc_cnt;
}

/* 通路是否已经初始化完成。语音链路的任务等这个再开跑，避免它一去调
 * play_pcm/mic_read 时 DMA 还没起来。 */
int bsp_audio_ready(void)
{
  return g_audio_ready;
}

/* ------------------------------------------------------------------------- */
/* 声学回环（可带"电路串扰"对照）                                              */
/* ------------------------------------------------------------------------- */

/* 放一段本地 1kHz，量麦克风频谱里 1kHz 与邻近频点(750/1250Hz)的能量。
 * 结果在多窗上累加（单窗方差太大：实测邻近频点会在 20~40 之间跳，
 * 只看一个窗根本判不出来，见下面 aud_grab_window 的注释）。
 *
 * 为什么要有 pa_on 这个参数
 * ------------------------
 * "麦克风里看到 1kHz" 有两个来源，只看一根谱线分不开：
 *   (a) 喇叭真的把声音放出来、空气传到麦克风（我们要的）
 *   (b) 码片/PCB 内部 DAC 输出耦合进 MIC 输入（声学压根没通）
 * 所以测两遍完全相同的音：功放开 / 功放关。声音只能从功放走 ——
 * (b) 与功放无关，两遍一样大；(a) 在功放关的那遍会塌下去。
 *
 * 为什么用本地音源而不是串口下行 PCM
 * --------------------------------
 * 下行 PCM 要先过"环形缓冲有没有欠载""串口时序来不来得及"这两道，
 * 量不准时根本分不清是'听不见'还是'没放出来'。本地音源直接灌 DAC，
 * 把这两个变量消掉，结论才干净。
 *
 * 输出都是 nwin 个窗的**累加值**（调用方自己除 nwin 取均值）：
 *   off_* = 静音窗（只统计，不放音）
 *   on_*  = 放音窗
 *   ref   = 750/1250Hz 的平均（当"底噪参考"，判 1kHz 有没有单独窜起来）
 * mic_peak / mic_rms 用来判断测试时的环境噪声大不大（窗都取在安静时段才有意义）。
 * 返回 0 成功，-1 通路没起来。 */
int bsp_audio_loopback(int pa_on, int nwin,
                       uint32_t *off_1k, uint32_t *on_1k,
                       uint32_t *off_ref, uint32_t *on_ref,
                       uint32_t *mic_peak, uint32_t *mic_rms)
{
  static int16_t win[AUD_GK_N];
  uint32_t so1k = 0, soff = 0, son1k = 0, sonref = 0;
  uint32_t mx = 0;
  uint64_t sq = 0;
  int      saved_amp;
  int      i, j;

  if (!g_audio_ready)
    {
      return -1;
    }
  if (nwin < 1)
    {
      nwin = 1;
    }

  saved_amp = g_play_amp;

  bsp_audio_set_volume_pct(100);         /* 放音通路拉满，别让音量掩盖结论 */
  bsp_audio_set_amp(pa_on ? 1 : 0);
  g_play_amp = AUD_AMP_LOUD;
  aud_delay_ms(50);                      /* 等功放的使能瞬态过去 */

  /* --- 静音窗：不放音，只摸当前底噪 --- */
  aud_play(AUD_SILENCE, 0);
  aud_delay_ms(100);
  for (i = 0; i < nwin; i++)
    {
      uint32_t a1k, a750, a1250;

      aud_grab_window(win);
      a1k   = aud_goertzel(win, AUD_GK_N, AUD_GK_COEF_1000);
      a750  = aud_goertzel(win, AUD_GK_N, AUD_GK_COEF_750);
      a1250 = aud_goertzel(win, AUD_GK_N, AUD_GK_COEF_1250);
      so1k += a1k;
      soff += (a750 + a1250) / 2u;
    }

  /* --- 放音窗 --- */
  aud_play(AUD_TONE, 1000);
  aud_delay_ms(150);                     /* 等正弦真灌进 DAC FIFO */
  for (i = 0; i < nwin; i++)
    {
      uint32_t b1k, b750, b1250;
      int32_t  v;

      aud_grab_window(win);
      b1k   = aud_goertzel(win, AUD_GK_N, AUD_GK_COEF_1000);
      b750  = aud_goertzel(win, AUD_GK_N, AUD_GK_COEF_750);
      b1250 = aud_goertzel(win, AUD_GK_N, AUD_GK_COEF_1250);
      son1k  += b1k;
      sonref += (b750 + b1250) / 2u;

      /* 顺手算麦克风的峰值/有效值：太吵的时段量什么谱线都不可信 */
      for (j = 0; j < AUD_GK_N; j++)
        {
          v = win[j];
          if (v < 0) v = -v;
          if ((uint32_t)v > mx) mx = (uint32_t)v;
          sq += (uint64_t)((int32_t)win[j] * (int32_t)win[j]);
        }
    }

  aud_play(AUD_SILENCE, 0);
  g_play_amp = saved_amp;
  bsp_audio_set_amp(1);                  /* 恢复成产品默认状态 */

  if (off_1k)   *off_1k   = so1k;
  if (on_1k)    *on_1k    = son1k;
  if (off_ref)  *off_ref  = soff;
  if (on_ref)   *on_ref   = sonref;
  if (mic_peak) *mic_peak = mx;
  if (mic_rms)  *mic_rms  =
      (uint32_t)sqrt((double)sq / (double)((uint32_t)nwin * AUD_GK_N));

  return 0;
}

/* ------------------------------------------------------------------------- */
/* 主线程                                                                     */
/* ------------------------------------------------------------------------- */

/* 诊断模式的完整流程（三段自检 + 频谱回环 + 拍手响应），见文件头注释 */
static void aud_diag_thread(void)
{
  uint32_t floor_level = 0;
  uint32_t thr;
  uint32_t peak, nz;
  uint32_t last_trigger = 0;
  uint32_t last_log = 0;
  uint32_t now;
  int      n;

  audlog("=== 板载音频通路 bring-up v5（放音 + 采集）===");

#if AUD_RUN_DAC_SELFTEST
  audlog("（DAC 三段式自检已开启）");
#endif

  if (aud_hw_init() != 0)
    {
      audlog("初始化失败，自检中止。请把上面的日志发出来。");
      return;
    }

  aud_dump_regs("init");
  aud_dma_dump("init");

  /* ---- 放音侧验证：没有中断/DMA 不动的话先修放音 ---- */
  sleep(1);
  audlog("起播 1s 后放音中断计数: HT %u / TC %u",
         (unsigned)g_ht_cnt, (unsigned)g_tc_cnt);

  if (g_ht_cnt == 0 && g_tc_cnt == 0)
    {
      int dma_running = aud_dma_watch();

      if (dma_running)
        {
          g_poll_mode = 1;
          audlog("放音中断没来但 DMA 在搬 -> 切轮询兜底（声音应当能出来）");
        }
      else
        {
          audlog("放音 DMA 没动：重压 DAC_CH0_CFG 的 DMA_EN，再看一次");
          hwp_audcodec->DAC_CH0_CFG &= ~AUDCODEC_DAC_CH0_CFG_DMA_EN_Msk;
          hwp_audcodec->DAC_CH0_CFG |= AUDCODEC_DAC_CH0_CFG_DMA_EN;
          aud_dump_regs("retry");

          if (aud_dma_watch())
            {
              g_poll_mode = 1;
              audlog("重压后放音 DMA 开始搬 -> 切轮询兜底");
            }
          else
            {
              audlog("放音仍然没动：codec 的 DAC 通路没真正跑起来。");
              aud_dump_regs("fail");
              aud_dma_dump("fail");
              return;
            }
        }
    }
  else
    {
      audlog("放音中断正常（HT/TC 都在涨），走中断路径");
    }

  audlog("放音工作模式: %s", g_poll_mode ? "轮询兜底（每 tick 一次）" : "DMA 中断");

  /* ---- 开场三声"嘀" ----
   * 听得到三声 = 放音全链路（DMA -> DAC -> NS4150B -> 喇叭）通了。
   * 音高递增，和后面的拍手响应（单声、更高）区分开。 */
  audlog("开场提示音：三声递增（880/1100/1320Hz）");
  aud_beep(880, 400);
  aud_beep(1100, 400);
  aud_beep(1320, 400);
  audlog("三声放完。若听到了说明放音通了。");

  /* ---- 采集侧自检 1：先摸本底噪声 ----
   * 静音 600ms 只统计，让 MICBIAS 和模拟 ADC 稳定下来，顺便拿到安静时的
   * 噪声底。触发门限必须以它为基准 —— 安静房间和嘈杂办公室差十几倍，
   * 门限写死必然一边不灵一边乱触发。 */
  audlog("采集侧自检 1/2：静音 600ms 测本底噪声...");
  {
    uint32_t nz_acc = 0;
    int      i;

    aud_play(AUD_SILENCE, 0);
    for (i = 0; i < 6; i++)
      {
        aud_delay_ms(100);
        peak = aud_mic_peak(&nz);
        nz_acc += nz;
        if (peak > floor_level)
          {
            floor_level = peak;      /* 取这段里的最大峰值当底噪 */
          }
      }

    if (floor_level < 60)
      {
        floor_level = 60;            /* 兜底：全 0 时不至于门限低到乱触发 */
      }

    audlog("底噪峰值=%u 非零样本≈%u/%d",
           (unsigned)floor_level, (unsigned)(nz_acc / 6), AUD_RX_SAMPLES);

    if (nz_acc / 6 == 0)
      {
        audlog("全部样本都是 0 -> ADC 通道没在送数据（ADC 总开关/时钟大概率没生效）");
      }
  }

  /* ---- 采集侧自检 2：声学回环（喇叭放，麦克风收），用频谱判定 ----
   * 这一步不需要人参与，也不看寄存器，直接看"空气里有没有 1kHz 传过去"：
   * 喇叭放 1kHz 大声，同时抓一段麦克风波形算 1kHz / 750Hz / 1250Hz 三根谱线。
   *   一路是 DAC -> NS4150B -> 喇叭，
   *   一路是麦克风 -> MICBIAS -> 模拟 ADC -> 数字 ADC -> DMA。
   * 一次把两个方向的模拟通路都验了。这也是回声消除要面对的耦合路径
   * （喇叭串到麦克风），提前量出这个耦合比，后面做全双工心里有数。 */
  audlog("采集侧自检 2/2：声学回环 —— 喇叭放 1kHz 大声，看麦克风频谱里有没有那根线");
  {
    static int16_t win[AUD_GK_N];
    uint32_t off_1k, off_750, off_1250;
    uint32_t on_1k,  on_750,  on_1250;
    uint32_t lift_1k;
    uint32_t lift_ref;
    int      i;

    /* --- 静音时的三根谱线 --- */
    aud_play(AUD_SILENCE, 0);
    aud_delay_ms(300);
    aud_grab_window(win);
    aud_mic_raw("静音窗", win, AUD_GK_N);
    off_1k   = aud_goertzel(win, AUD_GK_N, AUD_GK_COEF_1000);
    off_750  = aud_goertzel(win, AUD_GK_N, AUD_GK_COEF_750);
    off_1250 = aud_goertzel(win, AUD_GK_N, AUD_GK_COEF_1250);

    /* --- 放 1kHz 时的三根谱线 --- */
    g_play_amp = AUD_AMP_LOUD;
    aud_play(AUD_TONE, 1000);
    aud_delay_ms(300);            /* 让正弦真的开始灌进 DAC FIFO */
    aud_grab_window(win);
    aud_mic_raw("放音窗", win, AUD_GK_N);
    on_1k   = aud_goertzel(win, AUD_GK_N, AUD_GK_COEF_1000);
    on_750  = aud_goertzel(win, AUD_GK_N, AUD_GK_COEF_750);
    on_1250 = aud_goertzel(win, AUD_GK_N, AUD_GK_COEF_1250);

    aud_play(AUD_SILENCE, 0);
    g_play_amp = AUD_AMP_NORMAL;
    aud_delay_ms(100);

    audlog("谱线能量(任意单位) 静音: 750Hz=%u 1kHz=%u 1250Hz=%u",
           (unsigned)off_750, (unsigned)off_1k, (unsigned)off_1250);
    audlog("谱线能量(任意单位) 放音: 750Hz=%u 1kHz=%u 1250Hz=%u",
           (unsigned)on_750, (unsigned)on_1k, (unsigned)on_1250);

    /* 抬升倍数（用千分比避免浮点）：底噪那份兜到 1，防止除 0 */
    lift_1k  = (uint32_t)(((uint64_t)on_1k * 1000u) / (off_1k ? off_1k : 1u));
    {
      uint32_t a = (uint32_t)(((uint64_t)on_750  * 1000u) / (off_750  ? off_750  : 1u));
      uint32_t b = (uint32_t)(((uint64_t)on_1250 * 1000u) / (off_1250 ? off_1250 : 1u));
      lift_ref = (a > b) ? a : b;      /* 邻近频点里抬得更高的那个当参考 */
    }

    audlog("抬升倍数: 1kHz=%u.%03u 倍 | 邻近频点(参考)=%u.%03u 倍",
           (unsigned)(lift_1k / 1000u), (unsigned)(lift_1k % 1000u),
           (unsigned)(lift_ref / 1000u), (unsigned)(lift_ref % 1000u));

    /* 判定：看 1kHz **相对邻近频点**抬了多少。
     *
     * 这里踩过一次坑，留个记号：第一版判据是"1kHz 相对静音抬 3 倍以上
     * **且绝对值 > 2000**"。实测 1kHz 从 0 抬到 18、而 750/1250Hz 纹丝不动
     * （抬升 1.000 倍），**相对**上已经是非常干净的"听到了一根正弦"，
     * 但绝对量只有 18，被那个 2000 的门槛判成了"不通"。
     * 错在拿绝对量当主判据：麦克风的空气耦合本来就可能很弱（-70dB 量级），
     * 而"听到没听到"本质是**信噪比**问题，不是绝对电平问题。
     * 所以改成：1kHz 的能量要比邻近频点高出 4 倍以上，且 1kHz 自身要不为 0。 */
    {
      uint32_t ref_avg = ((on_750 + on_1250) / 2u) + 1u;   /* +1 防 0 */

      audlog("1kHz 相对邻近频点: %u 倍（邻近均值 %u）",
             (unsigned)(on_1k / ref_avg), (unsigned)ref_avg);

      if (on_1k >= 4u * ref_avg && on_1k > 0u)
        {
          audlog("声学回环**通**：麦克风频谱里 1kHz 单独窜起来了 -> 麦克风 + ADC 全链路 OK");
          audlog("  注意量级：绝对电平很低（约 %u，噪声底约 %u），说明空气耦合弱。",
                 (unsigned)on_1k, (unsigned)off_1k);
          audlog("  做语音上行时靠增益和近讲（嘴离板子 10~20cm）补，别指望远场。");
        }
      else
        {
          audlog("声学回环**不通**：1kHz 没有甩开邻近频点，麦克风没拾到喇叭的声音。");
          audlog("  可能原因（按概率）：");
          audlog("    1) 麦克风未贴装/被绝缘胶堵孔（DevKit 常见）");
          audlog("    2) MICBIAS 电压不对（ADC_ANA_CFG MICBIAS_EN 已置，见上面寄存器行）");
          audlog("    3) 喇叭本身没发声（功放/喇叭）—— 那就该先修放音");
          audlog("    4) 环境极端安静 + 喇叭离麦克风远，空气耦合本来就弱");
        }
    }
  }

  aud_play(AUD_SILENCE, 0);

  /* 门限：底噪的 4 倍 + 300 的绝对垫底。
   * 绝对垫底那 300 是为了防"底噪几乎为 0"的极端情况（门限低到一点电噪就触发）。 */
  thr = floor_level * 4 + 300;
  if (thr < 600)
    {
      thr = 600;
    }

  audlog("触发门限 = %u", (unsigned)thr);
  audlog("=== 进入监听循环：拍一下手，板子应当回一声短嘀 ===");

  last_log = (uint32_t)clock_systime_ticks();

  for (;;)
    {
      peak = aud_mic_peak(&nz);

      /* 自适应底噪：
       *   安静时（peak 比底噪还低）快速贴下去，避免环境变安静后门限虚高；
       *   有声音时慢慢抬，避免一次大声就把门限顶上去再也不响应。 */
      floor_level = (floor_level * 63 + peak) / 64;
      if (peak < floor_level)
        {
          floor_level = peak;
        }
      if (floor_level < 40)
        {
          floor_level = 40;
        }

      thr = floor_level * 4 + 300;
      if (thr < 600)
        {
          thr = 600;
        }

      now = (uint32_t)clock_systime_ticks();

      /* 触发判定：又超门限、又够响绝对值、又过了冷却期 */
      if (peak > thr && peak > 500 &&
          (now - last_trigger) > (uint32_t)(500 / AUD_MS_PER_TICK))
        {
          last_trigger = now;
          audlog(">>> 触发！峰值=%u 底噪=%u -> 嘀一声", (unsigned)peak,
                 (unsigned)floor_level);

          /* 响度分三档，用"嘀几声"表达（听感上"越响越急"）： */
          n = 1;
          if (peak > 8000)
            {
              n = 3;
            }
          else if (peak > 3000)
            {
              n = 2;
            }

          for (; n > 0; n--)
            {
              aud_beep(1400, 110);
            }
        }

      /* 每 500ms 打一行电平，方便不看板子也能从日志判断麦克风是否在收音 */
      if ((uint32_t)(now - last_log) >= (uint32_t)(500 / AUD_MS_PER_TICK))
        {
          last_log = now;
          audlog("电平: 峰值=%-6u 底噪=%-6u 门限=%-6u 非零=%d/%d | TX HT=%u TC=%u | RX HT=%u TC=%u",
                 (unsigned)peak, (unsigned)floor_level, (unsigned)thr,
                 (int)nz, AUD_RX_SAMPLES,
                 (unsigned)g_ht_cnt, (unsigned)g_tc_cnt,
                 (unsigned)g_rx_ht_cnt, (unsigned)g_rx_tc_cnt);
        }

      aud_delay_ms(100);
    }
}

/* 线程入口：诊断模式跑自检，产品模式只初始化通路然后让位给语音链路。 */
static void aud_selftest_thread(void)
{
#if AUD_SELFTEST_MODE
  aud_diag_thread();
#else
  audlog("=== 板载音频通路初始化（放音 + 采集）===");

  if (bsp_audio_hw_init() != 0)
    {
      audlog("音频通路初始化失败：串口语音链路拿不到喇叭和麦克风");
      return;
    }

  audlog("放音：DAC 通路已起（DMA 循环放静音，等下行 PCM）");
  audlog("采集：ADC 通路已起（%d 采样环形 = %d ms，%d Hz）",
         AUD_RX_SAMPLES, AUD_RX_SAMPLES * 1000 / AUD_RATE, AUD_RATE);
  bsp_audio_dump("init");

  g_audio_ready = 1;
  audlog("音频通路就绪，交给串口语音链路");
#endif
}

int bsp_audio_selftest_start(void)
{
  int pid = task_create("audio_test",
                        SCHED_PRIORITY_DEFAULT,
                        4096,
                        (main_t)aud_selftest_thread,
                        NULL);
  if (pid < 0)
    {
      syslog(LOG_ERR, "ERROR: audio_test task_create failed: %d\n", errno);
      return -1;
    }

  syslog(LOG_INFO, "[audio] audio_test 任务已启动 (pid=%d)\n", pid);
  return 0;
}
