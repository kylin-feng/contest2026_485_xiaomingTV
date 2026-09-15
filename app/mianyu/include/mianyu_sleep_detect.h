/* SPDX-License-Identifier: Apache-2.0
 *
 * 安眠科技 · 入睡判定（核心 IP，以 MIC 为主判据，IMU 可选增强）
 *
 * 为什么是这个算法（技术报告 3.3 / 3.5）：
 *   黄山派 SF32LB525UC6 是 Cortex-M33，无独立 HiFi4 DSP，不能跑重型频域分析。
 *   因此采用「轻量时域判据」而非 FFT：
 *     ① MIC 呼吸：100ms 帧 RMS 包络（降到 10Hz）→ 30s 窗归一化自相关
 *        → 在 0.15~0.5Hz（9~30 次/分）滞后范围找峰，峰高=呼吸规律度，峰位=呼吸率
 *     ② IMU 体动：加速度幅值去重力 → 滑动窗平均绝对偏差 = 活动量
 *     ③ 融合：规律呼吸 + 低体动 同时满足才判入睡，带迟滞状态机防抖
 *
 * 这一模块直接产出技术报告 3.5 节「健康类作品必填」的量化指标：
 *   误报率（醒着判成睡着 → 声音提前停）、漏报率（睡着没判出 → 整夜播放）、
 *   识别准确率。tests/test_sleep_detect.c 用合成信号跑混淆矩阵给出这三个数。
 *
 * 平台无关：分析层只依赖 <math.h> 的 sqrtf/fabsf，可在 PC 上完整单元测试。
 * 真机上 MIC/IMU 数据由 HAL 喂进来（hal/sf32lb52），模拟器由 Mock 喂（hal/sim）。
 */
#ifndef MIANYU_SLEEP_DETECT_H
#define MIANYU_SLEEP_DETECT_H

#include "mianyu_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 采样与窗口参数 ---- */
#define MY_ENV_RATE_HZ        10      /* 包络率：100ms 一帧 */
#define MY_ENV_FRAME_SAMPLES  (MY_SAMPLE_RATE / MY_ENV_RATE_HZ)  /* 1600 */
#define MY_ANALYSIS_WINDOW_S  30      /* 自相关分析窗 30 秒 */
#define MY_ENV_WINDOW         (MY_ENV_RATE_HZ * MY_ANALYSIS_WINDOW_S)  /* 300 帧 */
#define MY_ANALYSIS_HOP       20      /* 每 20 帧（2 秒）跑一次分析 */

/* 呼吸周期搜索范围（10Hz 包络下的滞后样本数）：
 *   lag 20 → 周期 2.0s → 30 次/分（上限）
 *   lag 80 → 周期 8.0s → 7.5 次/分（下限）
 * 成人睡眠呼吸率典型 10~16 次/分，落在此区间中部。 */
#define MY_LAG_MIN            20
#define MY_LAG_MAX            80

/* ---- 判定状态 ---- */
typedef enum {
    MY_SLEEP_AWAKE   = 0,   /* 清醒：体动多 / 呼吸不规律 */
    MY_SLEEP_DROWSY  = 1,   /* 困倦：呼吸趋规律，体动减少（入睡过渡） */
    MY_SLEEP_ASLEEP  = 2,   /* 入睡：规律呼吸 + 低体动，稳定持续 */
} my_sleep_state_t;

/* ---- 阈值配置（可调，默认值经合成信号标定） ---- */
typedef struct {
    float regularity_enter;   /* 呼吸规律度进入困倦阈值，默认 0.45 */
    float regularity_asleep;  /* 呼吸规律度判入睡阈值，默认 0.60 */
    float regularity_wake;    /* 呼吸规律度低于此值视为醒转证据（MIC 判据），默认 0.35。
                               * 目标板 DevKit-LCD 无板载 IMU，夜醒只能靠「呼吸节律塌缩」
                               * （醒来说话/翻身 → 无周期呼吸 → 自相关峰消失）检测。
                               * 与 movement_active 是 OR 关系：任一路达标即计醒转证据。 */
    float movement_still;     /* 体动低于此值视为"静卧"，默认 0.12 */
    float movement_active;    /* 体动高于此值视为"清醒活动"，默认 0.35 */
    int   sustain_drowsy;     /* 连续多少窗证据进入困倦，默认 2 */
    int   sustain_asleep;     /* 连续多少窗证据判定入睡，默认 3（约 6s） */
    int   wake_movement_win;  /* 连续多少窗醒转证据判夜醒/清醒，默认 2 */
} my_sleep_cfg_t;

/* ---- 单窗分析指标（可观测，写进测试报告） ---- */
typedef struct {
    float regularity;     /* 呼吸规律度 0~1（自相关峰高） */
    float breath_rate;    /* 呼吸率，次/分；0 表示未检出有效峰 */
    float movement;       /* 体动活动量 0~1 */
    float confidence;     /* 入睡置信度 0~1（融合分） */
    bool  peak_valid;     /* 自相关峰是否有效（局部极大且高于噪声底） */
} my_sleep_metrics_t;

/* ---- 统计（对位 3.5 误报/漏报） ---- */
typedef struct {
    uint32_t windows_total;      /* 累计分析窗数 */
    uint32_t asleep_windows;     /* 判为 ASLEEP 的窗数 */
    uint32_t transitions_to_asleep; /* AWAKE/DROWSY → ASLEEP 次数（入睡事件） */
    uint32_t night_wake_events;  /* ASLEEP 期间的夜醒事件数 */
    uint32_t first_asleep_sec;   /* 从启动到首次判入睡的秒数，0=未入睡 */
} my_sleep_stats_t;

typedef struct {
    my_sleep_cfg_t     cfg;
    /* 包络环形缓冲（Q15 能量，float 存储便于自相关；300 帧 ≈ 1.2KB） */
    float              env[MY_ENV_WINDOW];
    int                env_count;     /* 已填充帧数，未满窗前置 0 */
    int                env_head;      /* 环形写指针 */
    /* 当前帧能量累加器 */
    int32_t            frame_acc;     /* 100ms 内样本平方和（Q 域见实现） */
    int                frame_n;
    /* IMU 体动 */
    float              grav[3];       /* 重力估计（低通） */
    float              mov_acc;       /* 活动量累加（去重力幅值的绝对偏差） */
    int                mov_n;
    float              movement;      /* 最近一窗活动量 0~1 */
    /* 状态机 */
    my_sleep_state_t   state;
    int                drowsy_streak;
    int                asleep_streak;
    int                wake_streak;
    int                hop_counter;   /* 距上次分析的帧数 */
    uint32_t           elapsed_sec;   /* 启动至今秒数（按包络帧累计） */
    bool               ever_asleep;
    my_sleep_metrics_t last;
    my_sleep_stats_t   stats;
} my_sleep_detector_t;

MY_STATIC_ASSERT(sizeof(my_sleep_detector_t) < 4096, sleep_det_state_fits_4k);

/* 用默认配置初始化 */
my_err_t my_sleep_init(my_sleep_detector_t *d);
/* 用自定义配置初始化 */
my_err_t my_sleep_init_cfg(my_sleep_detector_t *d, const my_sleep_cfg_t *cfg);

/* 喂入 MIC PCM。内部按 100ms 帧累计能量，满一窗自动触发分析。
 * 可任意分块喂（如每次 320 样本 = 20ms），内部自行攒帧。 */
my_err_t my_sleep_feed_mic(my_sleep_detector_t *d, const my_pcm_t *pcm, int count);

/* 喂入一帧 IMU（单位 g，三轴）。典型 50Hz 调用。
 * 内部低通估重力并累计去重力活动量。 */
my_err_t my_sleep_feed_imu(my_sleep_detector_t *d, float ax, float ay, float az);

/* 查询当前状态与最近指标（不触发计算） */
my_sleep_state_t my_sleep_get_state(const my_sleep_detector_t *d);
const my_sleep_metrics_t *my_sleep_get_metrics(const my_sleep_detector_t *d);
const my_sleep_stats_t   *my_sleep_get_stats(const my_sleep_detector_t *d);

/* 复位状态机（如用户主动重新开始睡前流程），保留配置 */
void my_sleep_reset_state(my_sleep_detector_t *d);

#ifdef __cplusplus
}
#endif
#endif /* MIANYU_SLEEP_DETECT_H */
