/* SPDX-License-Identifier: Apache-2.0
 *
 * 眠语 · 噪声合成实现
 * 定点算法说明见 mianyu_noise_gen.h。
 */
#include "mianyu_noise_gen.h"

/* ---------------- PRNG ----------------
 * xorshift32：周期 2^32-1，3 次移位 3 次异或，无乘法。
 * 对白噪而言频谱平坦性足够，且完全可复现（同 seed 同输出），
 * 这对"睡眠记忆记住用户偏好声音"和单元测试都重要。
 */
static inline uint32_t xs32(uint32_t *s)
{
    uint32_t x = *s;
    if (x == 0) x = 0x9E3779B9u;   /* 黄金比例常数兜底，避免全 0 死锁 */
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

/* 有符号白噪样本，Q15 满幅 ±32767 */
static inline int32_t white_q15(uint32_t *s)
{
    uint32_t r = xs32(s);
    /* 取高 16 位映射到 [-32767, 32767]。
     * 不用 (int16_t)r：那样分布对 0 不对称且 -32768 会溢出后续乘法。 */
    int32_t v = (int32_t)(r >> 16) - 32768;      /* -32768..32767 */
    return MY_CLAMP(v, -32767, 32767);
}

/* ---------------- 粉噪：Paul Kellet 精简法 ----------------
 * 原版（浮点）：
 *   b0 = 0.99886*b0 + white*0.0555179;
 *   b1 = 0.99332*b1 + white*0.0750759;
 *   b2 = 0.96900*b2 + white*0.1538520;
 *   b3 = 0.86650*b3 + white*0.3104856;
 *   b4 = 0.55000*b4 + white*0.5329522;
 *   b5 = -0.7616*b5 - white*0.0168980;
 *   pink = b0+b1+b2+b3+b4+b5+b6*0.5362;  b6 = white*0.115926;
 *   输出 ≈ pink * 0.11 （归一到 ±1）
 *
 * 定点化：系数放大 2^20（Q20），状态累积也用 Q20，最后 >>20 回 Q15 量级。
 * 0.99886*2^20 = 1047658, 0.0555179*2^20 = 58222 ... 见下表。
 */
#define PK_A0  1047658   /* 0.99886  */
#define PK_A1  1041827   /* 0.99332  */
#define PK_A2  1016095   /* 0.96900  */
#define PK_A3   908735   /* 0.86650  */
#define PK_A4   576716   /* 0.55000  */
#define PK_A5  -798570   /* -0.7616  */

#define PK_B0    58222   /* 0.0555179 */
#define PK_B1    78733   /* 0.0750759 */
#define PK_B2   161332   /* 0.1538520 */
#define PK_B3   325583   /* 0.3104856 */
#define PK_B4   558893   /* 0.5329522 */
#define PK_B5   -17720   /* -0.0168980 */
#define PK_B6   121569   /* 0.115926  */
#define PK_OUT6   56234  /* 0.05362*2^20 (b6*0.5362 合并系数) */

/* 累积后归一系数。粉噪 6 路求和后能量偏大，crest factor 接近高斯分布
 * （32000 样本峰均比 ≈ sqrt(2·ln N) ≈ 4.5），满音量下极易削顶。
 *   定点标度修正后（b[] 稳态回到 Q15≈22000，sum std≈35000），
 *   out = (sum·PK_NORM)>>20。目标 100% 音量 RMS≈7000 → 峰值≈31500 无削顶。
 *   PK_NORM ≈ 7000·2^20/35000 ≈ 209715，取 200000 再经探针标定微调。
 * 此时粉噪 RMS 略低于白噪，符合粉噪能量集中低频、主观音量更响而客观 RMS 更低的特性。 */
#define PK_NORM   200000

/* 单样本粉噪，返回约 Q15 量级。
 *
 * 定点标度（关键，曾因标度错误导致 b[] 溢出 int32）：
 *   系数 A/B 用 Q20，状态 b[] 与输入 w 用 Q15。
 *   衰减项 (A·b)>>20：Q20·Q15=Q35 → >>20 = Q15 ✓
 *   输入项 (B·w)>>20：Q20·Q15=Q35 → >>20 = Q15 ✓
 *   b[] 稳态 std ≈ 0.67·32768 ≈ 22000（Q15），int32 安全（曾误用 >>4 放大 2^16 倍致溢出）
 *   6 路求和 sum std ≈ 0.995·32768 ≈ 32600（Q15）
 *   归一 out = (sum·PK_NORM)>>20，PK_NORM 定标见下。
 */
static inline int32_t pink_sample(my_noise_gen_t *g, int32_t w)
{
    int32_t *b = g->pink_acc;
    b[0] = (int32_t)(((int64_t)PK_A0 * b[0]) >> 20) + (int32_t)(((int64_t)PK_B0 * w) >> 20);
    b[1] = (int32_t)(((int64_t)PK_A1 * b[1]) >> 20) + (int32_t)(((int64_t)PK_B1 * w) >> 20);
    b[2] = (int32_t)(((int64_t)PK_A2 * b[2]) >> 20) + (int32_t)(((int64_t)PK_B2 * w) >> 20);
    b[3] = (int32_t)(((int64_t)PK_A3 * b[3]) >> 20) + (int32_t)(((int64_t)PK_B3 * w) >> 20);
    b[4] = (int32_t)(((int64_t)PK_A4 * b[4]) >> 20) + (int32_t)(((int64_t)PK_B4 * w) >> 20);
    b[5] = (int32_t)(((int64_t)PK_A5 * b[5]) >> 20) + (int32_t)(((int64_t)PK_B5 * w) >> 20);

    int64_t sum = (int64_t)b[0] + b[1] + b[2] + b[3] + b[4] + b[5];
    sum += ((int64_t)PK_B6 * w) >> 20;   /* b6 = white*0.115926，Q15，直接并入 */

    int32_t out = (int32_t)((sum * PK_NORM) >> 20);
    return MY_CLAMP(out, -32767, 32767);
}

/* ---------------- 棕噪：一阶低通（积分的有界近似） ----------------
 * 理想棕噪 = 白噪的积分（布朗运动），但纯积分无界会漂移溢出。
 * 工程上用一阶低通 y[n] = a*y[n-1] + (1-a)*x[n] 近似积分：
 * a 越接近 1，截止频率越低，越像积分（-6dB/octave 滚降），同时天然有界。
 * 取 a = 0.998（Q20: 1046446）→ 截止约 5Hz，听感是深沉的低频隆隆声。
 *
 * 增益：一阶低通把幅度压得极低（稳态 std 仅约白噪的 3%），必须补偿才可听。
 * 但不能像早期版本用 <<5 硬补（会把 RMS 顶到接近白噪、满音量削顶）。
 * 改用可调乘法增益 BR_GAIN，标定为满音量 RMS≈7000（与粉噪一致，crest≈4.5
 * 时峰值≈31500 不削顶）。棕噪 crest factor 实测低于高斯，余量更足。
 */
#define BR_A_Q20   1046446   /* 0.998，一阶低通极点 */
#define BR_GAIN     360000   /* 输出增益，Q15；标定于 test 探针，满音量 RMS≈7000 */

static inline int32_t brown_sample(my_noise_gen_t *g, int32_t w)
{
    /* y = a*y + (1-a)*w，Q15 状态，Q20 系数，>>20 回 Q15 */
    int64_t y = (int64_t)g->brown_state * BR_A_Q20;          /* Q15*Q20 = Q35 */
    y += (int64_t)w * ((1 << 20) - BR_A_Q20);                /* (1-a)≈0.002 */
    y >>= 20;                                                /* 回 Q15 */
    y = MY_CLAMP(y, -32767, 32767);
    g->brown_state = (int32_t)y;
    int32_t out = (int32_t)((y * BR_GAIN) >> 15);            /* 可调增益补偿 */
    return MY_CLAMP(out, -32767, 32767);
}

/* ---------------- DC blocker：一阶高通 ----------------
 * y[n] = R*(y[n-1] + x[n] - x[n-1])，R=0.995 → 截止 f = fs*(1-R)/(2π) ≈ 12.7Hz。
 * 挡掉粉/棕噪定点截断累积的直流，对 20Hz 以上几乎无衰减。
 * 用 Q15 定点：R*2^15 = 32604。放大域用 <<8 保精度，输出再 >>8。
 */
#define DCB_R_Q15  32604    /* 0.995 * 32768 */

static inline int32_t dc_block(my_noise_gen_t *g, int32_t x)
{
    /* 内部用 Q23（<<8）避免精度损失 */
    int32_t xq = x << 8;
    int64_t y = (int64_t)g->dc_y1 + xq - g->dc_x1;
    y = (y * DCB_R_Q15) >> 15;
    g->dc_x1 = xq;
    g->dc_y1 = (int32_t)MY_CLAMP(y, -(1LL << 30), (1LL << 30));
    int32_t out = (int32_t)(y >> 8);
    return MY_CLAMP(out, -32767, 32767);
}


/* ---------------- 公共 API ---------------- */

my_err_t my_noise_init(my_noise_gen_t *g, my_noise_kind_t kind, uint32_t seed, int level_pct)
{
    if (!g || kind >= MY_NOISE_KIND_MAX) return MY_ERR_PARAM;
    g->kind = kind;
    g->rng_state = seed ? seed : 0x9E3779B9u;
    for (int i = 0; i < MY_PINK_STAGES; i++) g->pink_acc[i] = 0;
    g->brown_state = 0;
    g->dc_x1 = 0;
    g->dc_y1 = 0;
    return my_noise_set_level(g, level_pct);
}

my_err_t my_noise_set_kind(my_noise_gen_t *g, my_noise_kind_t kind)
{
    if (!g || kind >= MY_NOISE_KIND_MAX) return MY_ERR_PARAM;
    g->kind = kind;          /* 故意不清状态：切换瞬间听感连续 */
    return MY_OK;
}

my_err_t my_noise_set_level(my_noise_gen_t *g, int level_pct)
{
    if (!g) return MY_ERR_PARAM;
    level_pct = MY_CLAMP(level_pct, 0, 100);
    /* 100% → Q15 32768（即 1.0），用 int32 存避免 32768 超 short 范围 */
    g->level_q15 = (int32_t)((level_pct * 32768L) / 100);
    return MY_OK;
}

int my_noise_render(my_noise_gen_t *g, my_pcm_t *out, int count)
{
    if (!g || !out || count <= 0) return MY_ERR_PARAM;

    const int32_t lvl = g->level_q15;
    for (int i = 0; i < count; i++) {
        int32_t w = white_q15(&g->rng_state);
        int32_t s;
        switch (g->kind) {
        case MY_NOISE_PINK:  s = dc_block(g, pink_sample(g, w));  break;
        case MY_NOISE_BROWN: s = dc_block(g, brown_sample(g, w)); break;
        case MY_NOISE_WHITE:
        default:             s = w;                  break;  /* 白噪无 DC 累积，不过高通 */
        }
        /* 幅度缩放：s(Q15) * lvl(Q15) >> 15 */
        s = (int32_t)(((int64_t)s * lvl) >> 15);
        out[i] = (my_pcm_t)MY_CLAMP(s, MY_PCM_MIN, MY_PCM_MAX);
    }
    return count;
}

/* ---------------- 测试观测接口 ---------------- */

int32_t my_noise_measure_dc(const my_pcm_t *buf, int count)
{
    if (!buf || count <= 0) return 0;
    int64_t sum = 0;
    for (int i = 0; i < count; i++) sum += buf[i];
    return (int32_t)(sum / count);
}

int32_t my_noise_measure_rms_q8(const my_pcm_t *buf, int count)
{
    if (!buf || count <= 0) return 0;
    uint64_t acc = 0;
    for (int i = 0; i < count; i++) {
        int32_t v = buf[i];
        acc += (uint64_t)(v * v);
    }
    /* sqrt(acc/count) * 256。用整数牛顿迭代开方，避免链接 -lm（嵌入式省 Flash） */
    uint64_t mean = acc / (uint64_t)count;
    if (mean == 0) return 0;
    uint64_t x = mean, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + mean / x) / 2; }
    return (int32_t)(x * 256);   /* x 是 rms，放大 Q8 返回 */
}

int my_noise_count_clipped(const my_pcm_t *buf, int count)
{
    if (!buf || count <= 0) return 0;
    int n = 0;
    for (int i = 0; i < count; i++)
        if (buf[i] >= MY_PCM_MAX || buf[i] <= MY_PCM_MIN) n++;
    return n;
}

int32_t my_noise_measure_hf_ratio_q8(const my_pcm_t *buf, int count)
{
    if (!buf || count < 2) return 0;
    uint64_t e_orig = 0, e_diff = 0;
    for (int i = 0; i < count; i++) {
        int32_t v = buf[i];
        e_orig += (uint64_t)(v * v);
        if (i > 0) {
            int32_t d = v - buf[i - 1];
            e_diff += (uint64_t)(d * d);
        }
    }
    if (e_orig == 0) return 0;
    /* 一阶差分能量≈2*(1-corr)倍原能量。白噪样本独立 → 比值≈2.0(512)；
     * 棕噪强相关 → 比值远小于 1.0 */
    return (int32_t)((e_diff << 8) / e_orig);
}
