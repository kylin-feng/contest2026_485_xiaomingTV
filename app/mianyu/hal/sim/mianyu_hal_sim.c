/* SPDX-License-Identifier: Apache-2.0
 *
 * 眠语 · HAL 后端：PC 模拟器（无硬件）
 *
 * 把「真机主循环」搬到 PC 上跑：用虚拟时钟 + 合成呼吸信号 + 内存存储，
 * 让 app/mianyu/mianyu_app_main.c 在没有任何硬件/RKOS 的普通电脑上也能
 * 完整走一遍「22:30 哄睡 → 入睡 → 淡出 → 夜醒安抚 → 07:00 晨唤」。
 *
 * 这是评审 clone 后「make app」一键自证真机主循环正确性的入口。
 * 真机后端见 hal/sf32lb52/mianyu_hal_vela.c，两者实现同一套 my_hal_* 符号。
 *
 * 时间模型：本后端维护一个虚拟时钟，只在 my_hal_sleep_ms() 里推进。
 * 主循环每 100ms 调一次 sleep_ms(100)，虚拟时钟就 +100ms；my_hal_time_now()
 * 读它，my_hal_mic_read() 按当前虚拟时刻合成 100ms 呼吸信号。真实设备上，
 * 这 100ms 是 RTC 自己走的，MIC 是硬件采的，主循环代码一行不变。
 */
#include "mianyu_hal.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ===================== 虚拟时钟 ===================== */

static my_datetime_t s_now;       /* 当前虚拟时刻（由 s_clock_ms 派生） */
static int64_t      s_start_min;  /* 起点 22:05:00 的绝对分钟 */
static int64_t      s_clock_ms;   /* 自起点累计的虚拟毫秒（唯一时间源） */

/* ===================== 合成信号（复用测试标定模型） ===================== */

static uint32_t s_rng = 0xC0FFEEu;

static uint32_t rng_next(void)
{
    uint32_t x = s_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    s_rng = x;
    return x;
}

static float rng_uni(void)
{
    return (float)(rng_next() >> 8) / 8388608.0f - 1.0f;
}

/* 一个"身体状态"：呼吸率/调幅/音量/体动。随虚拟时刻切换，模拟一整晚。 */
typedef struct {
    float breath_bpm;    /* 0 = 无呼吸周期（说话/环境噪声） */
    float depth;         /* 呼吸包络调幅深度 */
    float mic_amp;       /* MIC 样本幅度（PCM 域，与测试标定一致=6000） */
    float imu_activity;  /* 体动（g）：0.04=静卧 0.9=清醒活动 */
} body_t;

/* 按虚拟时刻返回身体状态——这是"剧本"，真实设备上换成真实传感器数据。
 * 用「相对起点的绝对分钟」而非 hour*60+minute：剧本跨午夜（22:05→次日07:00），
 * 小时直接比较会在 22:46 之后失效（1366 > 90 恒真，落到晨醒分支）。 */
static body_t sim_body(const my_datetime_t *dt)
{
    int64_t elapsed = my_datetime_to_minutes(dt) - s_start_min;
    body_t b;

    if (elapsed < 25) {
        /* 22:05–22:29 清醒看书：说话/体动多，不误判入睡 */
        b = (body_t){ .breath_bpm = 0, .depth = 0, .mic_amp = 6000.0f, .imu_activity = 0.6f };
    } else if (elapsed < 41) {
        /* 22:30 躺下，呼吸渐规律、体动减少（入睡过程） */
        b = (body_t){ .breath_bpm = 16, .depth = 0.5f, .mic_amp = 6000.0f, .imu_activity = 0.04f };
    } else if (elapsed < 205) {
        /* 22:46–01:29 深睡（跨午夜） */
        b = (body_t){ .breath_bpm = 14, .depth = 0.5f, .mic_amp = 6000.0f, .imu_activity = 0.03f };
    } else if (elapsed < 220) {
        /* 01:30–01:44 夜醒：说话/翻身（无周期呼吸，靠 MIC 塌缩检出） */
        b = (body_t){ .breath_bpm = 0, .depth = 0, .mic_amp = 6000.0f, .imu_activity = 0.9f };
    } else if (elapsed < 535) {
        /* 01:45–06:59 二次入睡 */
        b = (body_t){ .breath_bpm = 15, .depth = 0.5f, .mic_amp = 6000.0f, .imu_activity = 0.03f };
    } else {
        /* 07:00 起 晨醒（仍平躺，呼吸浅、偶有体动） */
        b = (body_t){ .breath_bpm = 0, .depth = 0, .mic_amp = 6000.0f, .imu_activity = 0.35f };
    }
    return b;
}

/* 合成 MIC 样本：呼吸正弦包络 + 扰动，再乘白噪声载波。 */
static my_pcm_t s_mic_buf[MY_ENV_FRAME_SAMPLES];
static int64_t   s_total_samples;   /* 已合成的绝对样本序号（保证正弦相位连续） */

/* ===================== 存储（内存后端，掉电即失） ===================== */

static my_task_t         s_tasks[MY_TASK_MAX];
static int               s_task_count;

static my_sleep_record_t s_records[MY_MEM_MAX_DAYS];
static int               s_rec_count;
static int               s_rec_write_idx;

static int sim_sched_load(my_task_t *tasks, int max_tasks, void *ctx)
{
    (void)ctx;
    int n = s_task_count < max_tasks ? s_task_count : max_tasks;
    memcpy(tasks, s_tasks, (size_t)n * sizeof(my_task_t));
    return n;
}

static my_err_t sim_sched_save(const my_task_t *tasks, int count, void *ctx)
{
    (void)ctx;
    if (count < 0 || count > MY_TASK_MAX) return MY_ERR_PARAM;
    s_task_count = count;
    memcpy(s_tasks, tasks, (size_t)count * sizeof(my_task_t));
    return MY_OK;
}

static my_err_t sim_mem_save(const my_sleep_record_t *records, int count,
                             int write_idx, void *ctx)
{
    (void)ctx;
    if (count < 0 || count > MY_MEM_MAX_DAYS) return MY_ERR_PARAM;
    s_rec_count = count;
    s_rec_write_idx = write_idx;
    memcpy(s_records, records, (size_t)count * sizeof(my_sleep_record_t));
    return MY_OK;
}

static int sim_mem_load(my_sleep_record_t *records, int max_records,
                        int *write_idx, void *ctx)
{
    (void)ctx;
    int n = s_rec_count < max_records ? s_rec_count : max_records;
    memcpy(records, s_records, (size_t)n * sizeof(my_sleep_record_t));
    *write_idx = s_rec_write_idx;
    return n;
}

static my_sched_store_t s_sched_store = { sim_sched_load, sim_sched_save, NULL };
static my_mem_store_t   s_mem_store   = { sim_mem_save, sim_mem_load, NULL };

/* ===================== 生命周期 ===================== */

my_err_t my_hal_init(void)
{
    /* 虚拟时钟从 22:05 起（与 night_demo 剧本一致） */
    s_now = (my_datetime_t){ .year = 2026, .month = 9, .day = 4,
                             .hour = 22, .minute = 5, .second = 0 };
    my_datetime_normalize(&s_now);
    s_start_min = my_datetime_to_minutes(&s_now);
    s_clock_ms = 0;
    s_total_samples = 0;
    s_task_count = 0;
    s_rec_count = 0;
    s_rec_write_idx = 0;
    return MY_OK;
}

void my_hal_deinit(void) { /* 无资源可释放 */ }

/* 由累计虚拟毫秒回推当前 datetime（绝对分钟 + 秒）。 */
static void sync_clock(void)
{
    int64_t abs_min = s_start_min + s_clock_ms / 60000;
    int     sec     = (int)((s_clock_ms % 60000) / 1000);
    my_datetime_from_minutes(abs_min, &s_now);
    s_now.second = sec;
}

void my_hal_sleep_ms(int ms)
{
    /* 虚拟时钟推进（真实设备这里 sleep 真实毫秒，RTC 自己走） */
    if (ms > 0) s_clock_ms += ms;
    sync_clock();
}

/* ===================== 时间 ===================== */

my_err_t my_hal_time_now(my_datetime_t *dt)
{
    if (!dt) return MY_ERR_PARAM;
    *dt = s_now;
    return MY_OK;
}

/* ===================== 音频输出 ===================== */

int my_hal_audio_play(const my_pcm_t *buf, int count)
{
    (void)buf;
    /* 模拟器无 DAC：直接丢弃。真实后端写 DMA 环形缓冲。 */
    return count;
}

void my_hal_audio_stop(void) { /* 无缓冲可清 */ }

/* ===================== 麦克风输入 ===================== */

int my_hal_mic_read(my_pcm_t *buf, int max_count)
{
    if (!buf || max_count <= 0) return MY_ERR_PARAM;
    int n = max_count < MY_ENV_FRAME_SAMPLES ? max_count : MY_ENV_FRAME_SAMPLES;
    if (n > MY_ENV_FRAME_SAMPLES) n = MY_ENV_FRAME_SAMPLES;

    body_t body = sim_body(&s_now);
    /* 相位基准：用虚拟时钟的绝对秒，保证正弦相位跨 tick 连续 */
    double t0 = (double)s_clock_ms / 1000.0;

    for (int i = 0; i < n; i++) {
        double t = t0 + (double)i / (double)MY_SAMPLE_RATE;
        float e = 1.0f;
        if (body.breath_bpm > 0.0f) {
            float f = body.breath_bpm / 60.0f;
            e += body.depth * (float)sin(2.0 * M_PI * f * t);
        }
        e += 0.02f * rng_uni();
        if (e < 0.05f) e = 0.05f;
        buf[i] = (my_pcm_t)(e * body.mic_amp * rng_uni());
    }
    s_total_samples += n;
    return n;
}

/* ===================== 体动（IMU，可选） ===================== */

bool my_hal_imu_available(void)
{
    /* DevKit-LCD 无板载 IMU；模拟器虽能合成，但为如实对应目标硬件，返回 false，
     * 让主循环走「MIC 单判据」——这正是真机的实际形态。 */
    return false;
}

my_err_t my_hal_imu_read(float *ax, float *ay, float *az)
{
    (void)ax; (void)ay; (void)az;
    return MY_ERR_UNSUPPORTED;
}

/* ===================== 呼吸引导灯 ===================== */

static int s_light_pct;   /* 最近一次亮度（主循环读回用于日志） */

void my_hal_light_set(int level_pct)
{
    /* 模拟器无屏：仅记录。真实后端映射到 LVGL 呼吸光晕 opacity。 */
    s_light_pct = level_pct < 0 ? 0 : (level_pct > 100 ? 100 : level_pct);
}

/* ===================== 存储后端注入 ===================== */

const my_sched_store_t *my_hal_sched_store(void) { return &s_sched_store; }
const my_mem_store_t   *my_hal_mem_store(void)   { return &s_mem_store; }
