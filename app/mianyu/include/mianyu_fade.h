/* SPDX-License-Identifier: Apache-2.0
 *
 * 安眠科技 · 音量淡出曲线引擎
 *
 * ============================ 为什么这个模块值得单独做 ============================
 *
 * 哄睡产品的「淡出」不是工程细节，是体验核心：声音必须在用户无察觉的情况下
 * 消失。做得不对会出现三种典型翻车，本模块逐一针对：
 *
 *   翻车 1：线性降音量 → 前半程听不出变化，最后几秒突然没了，用户被"惊醒"。
 *     原因：响度感知是对数的。幅度线性下降时，dB 下降速率在低幅度段急剧变快
 *     （50%→-6dB，10%→-20dB，2%→-34dB）。
 *     对策：【dB 域线性】= 幅度指数衰减，感知上匀速变轻。
 *
 *   翻车 2：用硬件音量的整数百分比做淡出 → 低音量段台阶感。
 *     原因：15 分钟淡出 / 100 步 = 每 9 秒跳一档；在 level=2→1 时是 6dB 跳变，
 *     在安静卧室里非常明显。
 *     对策：引擎输出 Q15 增益，【直接乘在 PCM 样本上】（软件音量），
 *     分辨率 1/32768 ≈ 0.00026dB。硬件百分比音量只做粗调（见 my_fade_gain_pct）。
 *
 *   翻车 3：暂停/恢复、智能体开口说话时的"咔哒"声。
 *     原因：增益瞬间跳变 = 阶跃信号 = 宽频瞬态。夜里尤其刺耳。
 *     对策：所有增益变化都走斜坡。淡入用 smoothstep（首尾导数为 0，天然无冲击），
 *     暂停/闪避用 duck_ms 短斜坡。
 *
 * 额外做的两件音频工程细节：
 *   • 指数尾部转线性归零：指数衰减永不触零，会在 -60dB 处无限拖尾，
 *     导致"关了但还在响"。故末段 tail_ms 改线性斜坡，保证在 fade_out_ms
 *     时刻【精确等于 0】并锁死（gain==0 时直接写零样本，不做乘法）。
 *   • TPDF 抖动（可关）：淡出末段增益很低，PCM 重定量化失真相对信号变得可闻。
 *     三角概率密度抖动把量化失真转成恒定本底噪声，听感更干净。
 *     代价是 ±1 LSB 本底（约 -90dBFS），在卧室环境远低于噪声门限。
 *
 * ============================ 为什么可跨比赛复用 ============================
 *
 * 时间全部由调用方喂入（my_fade_tick(delta_ms)），模块内部【不调用任何 OS 时间函数】。
 * 好处：① PC 上可完整单元测试（tests/test_fade.c 就是这么跑的）；
 *       ② 换平台只换喂 tick 的地方；③ 暂停语义天然正确（停止喂 tick 即可）。
 * 只依赖 mianyu_common.h + math.h 的 exp2f/log2f，无平台头文件。
 */
#ifndef MIANYU_FADE_H
#define MIANYU_FADE_H

#include "mianyu_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Q15 满幅：32768 = 1.0。用 int32 存储以允许 32768（int16 存不下） */
#define MY_GAIN_FULL_Q15   32768

/* ---- 淡出阶段 ---- */
typedef enum {
    MY_FADE_STOPPED = 0,   /* 未开始 / 已彻底静音，输出 0 */
    MY_FADE_IN,            /* 淡入中（smoothstep） */
    MY_FADE_HOLD,          /* 保持目标音量（正常播放） */
    MY_FADE_OUT,           /* 淡出中（dB 线性 + 末段线性归零） */
    MY_FADE_DONE,          /* 淡出完成，输出 0 并锁死 */
} my_fade_phase_t;

/* ---- 配置 ---- */
typedef struct {
    int32_t fade_in_ms;      /* 淡入时长，0 = 无淡入直接到目标。默认 2000 */
    int32_t hold_ms;         /* 保持时长，0 = 无限（须显式调 start_fade_out）。默认 0 */
    int32_t fade_out_ms;     /* 淡出总时长。默认 900000（15 分钟，产品设计值） */
    int32_t tail_ms;         /* 淡出末段线性归零时长，须 < fade_out_ms。默认 3000 */
    int32_t floor_db;        /* 指数段下限（负数 dB），之后转线性。默认 -60 */
    int32_t duck_gain_q15;   /* 闪避目标增益。默认 8192（= -12dB，语音可懂） */
    int32_t duck_ms;         /* 闪避/暂停的进出斜坡时长。默认 300 */
    int32_t target_q15;      /* 播放目标音量。默认 22938（= 70%，与噪声模块标定一致） */
    bool    dither_tpdf;     /* 是否启用 TPDF 抖动，默认 true */
} my_fade_cfg_t;

/* ---- 引擎状态 ---- */
typedef struct {
    my_fade_cfg_t   cfg;

    /* 场景包络（fade_in / hold / fade_out 三段时间线） */
    my_fade_phase_t phase;
    int32_t         scene_ms;        /* 当前阶段内已流逝时间 */
    int32_t         scene_gain_q15;  /* 场景包络输出增益（不含 duck/pause） */
    /* 淡出起点：从当前音量开始淡出，而非从满幅（支持淡入中途触发淡出） */
    int32_t         fo_start_gain_q15;
    int32_t         fo_exp_ms;       /* 指数(dB线性)段时长，0 = 跳过直接走线性 */
    float           fo_start_db;
    /* 淡出线性收尾段（保证在 fade_out_ms 时刻精确归零，不留指数拖尾） */
    int32_t         fo_lin_start_gain_q15;  /* 线性段起点增益 */
    int32_t         fo_lin_ms;              /* 线性段时长 */

    /* 闪避包络（智能体开口说话时压低背景音） */
    bool            duck_active;
    int32_t         duck_ms_elapsed;
    int32_t         duck_gain_q15;   /* 当前闪避增益，MY_GAIN_FULL_Q15 = 不闪避 */
    int32_t         duck_from_q15;   /* 本次斜坡起点 */

    /* 暂停包络（与闪避独立，可同时生效，总增益 = 场景 × 闪避 × 暂停） */
    bool            paused;
    int32_t         pause_gain_q15;  /* MY_GAIN_FULL_Q15 = 未暂停 */
    int32_t         pause_from_q15;
    int32_t         pause_ms_elapsed;

    /* 累计运行时间（不含暂停期），用于统计与 UI */
    int64_t         played_ms;
    uint32_t        rng;             /* 抖动用 PRNG */
} my_fade_t;

MY_STATIC_ASSERT(sizeof(my_fade_t) < 256, fade_state_is_tiny);

/* ---- 默认配置 ---- */
void my_fade_default_cfg(my_fade_cfg_t *cfg);

/* 初始化。cfg 传 NULL 用默认配置。初始化后处于 STOPPED，输出 0。 */
my_err_t my_fade_init(my_fade_t *f, const my_fade_cfg_t *cfg);

/* ---- 播放控制 ---- */
/* 开始播放：走 fade_in → hold。若 hold_ms>0，到时自动进入 fade_out。
 * 若当前正在播放，重新开始（重播场景）。 */
my_err_t my_fade_start(my_fade_t *f);

/* 立即开始淡出（入睡判定触发时调用）。
 *
 * 从【当前实际可闻音量】起淡（= 场景 × 闪避 × 暂停），并把闪避/暂停
 * 两个乘子"烘焙"进曲线起点后置为中性、解除闪避与暂停。
 *
 * 为什么要烘焙：若不烘焙，淡出途中智能体说完话释放闪避，duck 乘子会从
 * -12dB 拉回 0dB，总增益瞬间反弹（夜里音量突然变大 = 惊醒用户）。
 * 烘焙后总增益严格单调不增，这是 tests/test_fade.c 组 8 专门守的不变量。
 *
 * 副作用：在暂停途中触发淡出会被解除暂停，从当前可闻音量继续淡出
 * （"要关机了"覆盖"暂停"，语义合理）。
 *
 * 已 STOPPED/DONE 时调用为无害空操作；正在淡出时调用不重启曲线。 */
my_err_t my_fade_start_fade_out(my_fade_t *f);

/* 暂停：场景时间线冻结（不推进 scene_ms），增益短斜坡降到 0。
 * 语义 = "声音停了但进度没丢"。恢复后从同一进度继续淡出。
 * 注意：暂停途中若触发 my_fade_start_fade_out，暂停会被解除（见上）。 */
my_err_t my_fade_pause(my_fade_t *f);

/* 恢复：增益斜坡回到暂停前的场景增益，随后时间线继续推进。
 * 淡出剩余时长与暂停前一致（关键：不因暂停而缩短或重置）。 */
my_err_t my_fade_resume(my_fade_t *f);

/* 闪避：智能体要说话时压低背景音。active=true 进入，false 退出。
 * 可重复调用，斜坡从当前值起算（说话中途再触发不会跳变）。 */
my_err_t my_fade_set_duck(my_fade_t *f, bool active);

/* ---- 时间推进（唯一的时间来源，由调用方按音频循环周期喂入） ----
 * delta_ms: 距上次 tick 的毫秒数，须 >= 0。
 * 典型用法：音频回调每 20ms 一帧 → my_fade_tick(&f, 20)。 */
my_err_t my_fade_tick(my_fade_t *f, int32_t delta_ms);

/* ---- 增益查询 ---- */
/* 当前总增益 Q15（场景 × 闪避 × 暂停），0..32768 */
int32_t  my_fade_gain_q15(const my_fade_t *f);
/* 换算成 0..100 百分比，供只接受整数音量的硬件驱动（粗调用） */
int      my_fade_gain_pct(const my_fade_t *f);
/* 当前增益的 dB 值（相对满幅），用于日志/曲线绘制；0 增益返回负无穷大 */
float    my_fade_gain_db(const my_fade_t *f);
/* 当前阶段 */
my_fade_phase_t my_fade_get_phase(const my_fade_t *f);
/* 淡出剩余毫秒；非淡出阶段返回 0 */
int32_t  my_fade_remaining_ms(const my_fade_t *f);
/* 已累计播放时长（不含暂停），毫秒 */
int64_t  my_fade_played_ms(const my_fade_t *f);

/* ---- 增益施加到 PCM（就地） ----
 * gain_q15==0 时直接写零样本（不做乘法、不加抖动），保证彻底静音。
 * count 为样本数。返回处理的样本数，<0 为错误码。
 * 16kHz/320 样本(20ms) 在 M33@240MHz 上约 <5us。 */
int my_fade_apply(my_fade_t *f, my_pcm_t *buf, int count, int32_t gain_q15);

/* 便捷版：用引擎当前增益施加（内部会调 my_fade_gain_q15） */
int my_fade_apply_current(my_fade_t *f, my_pcm_t *buf, int count);

#ifdef __cplusplus
}
#endif
#endif /* MIANYU_FADE_H */
