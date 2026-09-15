/* SPDX-License-Identifier: Apache-2.0
 *
 * 安眠科技 · 噪声合成模块
 *
 * 为什么用算法合成而不是播放音频文件（技术报告 3.4 / 4.5）：
 *   黄山派仅 16MB NOR Flash，ROOT 分区默认 8480KB，LittleFS sector 4KB。
 *   一条 30 秒雨声 WAV（16kHz/16bit/mono）≈ 940KB，存不下几条。
 *   本模块用 PRNG + 滤波器在端侧实时合成白/粉/棕噪，存储占用 0KB，
 *   同时绕开 openvela 音频框架在编解码环节的已知 crash 风险。
 *
 * 算法选型：
 *   白噪 white  : xorshift32 PRNG → 平坦频谱
 *   粉噪 pink   : Paul Kellet 精简滤波器法（每样本 6 次乘加，无状态历史数组）
 *                 相比 Voss-McCartney 分形法省内存、省分支预测开销
 *   棕噪 brown  : 白噪一阶积分（带泄漏防漂移），-6dB/octave
 *
 * 定点实现：全程 Q15/Q20 定点，不依赖 FPU（SF32LB52 Cortex-M33 有 FPU，
 *   但定点可保证在任意目标平台结果一致，也便于 PC 端单元测试复现）。
 */
#ifndef MIANYU_NOISE_GEN_H
#define MIANYU_NOISE_GEN_H

#include "mianyu_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MY_NOISE_WHITE = 0,
    MY_NOISE_PINK  = 1,
    MY_NOISE_BROWN = 2,
    MY_NOISE_KIND_MAX
} my_noise_kind_t;

/* 粉噪 Kellet 滤波器状态：b0..b6 + 上一轮白噪输入 */
#define MY_PINK_STAGES 6

typedef struct {
    my_noise_kind_t kind;
    uint32_t        rng_state;                 /* xorshift32 状态，0 视为未播种 */
    int32_t         pink_acc[MY_PINK_STAGES];  /* Q20 累积 */
    int32_t         brown_state;               /* 棕噪一阶低通状态，Q15 */
    /* DC blocker（一阶高通，R=0.995 → 截止约 13Hz@16kHz）：
     * 挡住粉/棕噪定点运算累积的直流偏移，不影响 20Hz 以上可听频段 */
    int32_t         dc_x1;                     /* 上一输入样本 */
    int32_t         dc_y1;                     /* 上一输出样本（Q15 域放大） */
    int32_t         level_q15;                 /* 输出幅度缩放，Q15：32768 = 1.0 */
} my_noise_gen_t;

MY_STATIC_ASSERT(sizeof(my_noise_gen_t) < 64, noise_gen_state_is_tiny);

/* 初始化。seed 相同则输出序列完全一致（便于测试与"记住用户偏好的声音"）。
 * level_pct: 0..100，映射到 level_q15。 */
my_err_t my_noise_init(my_noise_gen_t *g, my_noise_kind_t kind, uint32_t seed, int level_pct);

/* 运行期切换噪声类型，保留 RNG 状态（听感连续，不会"咔"一下重来） */
my_err_t my_noise_set_kind(my_noise_gen_t *g, my_noise_kind_t kind);

/* 设置输出幅度 0..100（百分比） */
my_err_t my_noise_set_level(my_noise_gen_t *g, int level_pct);

/* 生成 count 个 PCM 样本到 out。out 容量至少 count 个 int16_t。
 * 返回实际写入样本数（<0 为错误码）。
 * 16kHz 下 20ms 帧 = 320 样本，本函数处理 320 样本在 M33@240MHz 上约 <10us。 */
int my_noise_render(my_noise_gen_t *g, my_pcm_t *out, int count);

/* ---- 供单元测试使用的可观测接口 ---- */
/* 计算一段 PCM 的直流偏移（均值），粉/棕噪若滤波器设计不当会漂出很大 DC */
int32_t my_noise_measure_dc(const my_pcm_t *buf, int count);

/* 计算 RMS（Q8 定点返回，即 rms*256），用于验证幅度归一化是否稳定 */
int32_t my_noise_measure_rms_q8(const my_pcm_t *buf, int count);

/* 统计削顶（|sample| >= 32767）样本数，>0 说明溢出，是 bug */
int my_noise_count_clipped(const my_pcm_t *buf, int count);

/* 一阶差分能量 / 原信号能量 的比值（Q8），用于粗略判别频谱倾斜：
 *   白噪 ≈ 2.0（高频能量足）  棕噪 << 1（能量集中在低频）
 *   这是不引入 FFT 的轻量频谱判别法，符合"无 HiFi4 DSP 用轻量判据"的设计原则 */
int32_t my_noise_measure_hf_ratio_q8(const my_pcm_t *buf, int count);

#ifdef __cplusplus
}
#endif
#endif /* MIANYU_NOISE_GEN_H */
