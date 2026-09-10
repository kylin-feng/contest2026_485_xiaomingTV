/* SPDX-License-Identifier: Apache-2.0
 *
 * 眠语 · openvela 应用入口（真机主循环，平台无关）
 *
 * ============================ 这个文件是干什么的 ============================
 *
 * 这是「主动式哄睡智能体」的 app 主循环，运行在 openvela / NuttX 上。
 * 它把五个核心模块（调度/噪声/入睡判定/淡出/记忆）按真机的真实节奏串起来：
 *
 *   ① 时间源：RTC → my_datetime_t → my_sched_tick（cron 主动触发）
 *   ② 麦克风：MEMS MIC → my_sleep_feed_mic（入睡判定）
 *   ③ 体动：  IMU（若有）→ my_sleep_feed_imu（双模态增强）
 *   ④ 事件：  入睡 → 淡出 + 灯灭；夜醒 → 低音量安抚 + 微光
 *   ⑤ 音频：  噪声合成 → 淡出增益 → my_hal_audio_play 到 DAC
 *   ⑥ 灯光：  呼吸引擎 → my_hal_light_set 到 LVGL
 *   ⑦ 记忆：  晨唤 → 一晚记录落盘（my_mem_add + flush）
 *
 * ============================ 为什么平台无关 ============================
 *
 * 本文件【不含任何 openvela / NuttX / SDK 头文件】——所有硬件访问都走
 * hal/mianyu_hal.h 的 my_hal_* 接口。因此在 PC 上链接 hal/sim 就能
 * `make app` 跑通整晚；上真机链接 hal/sf32lb52 就落进 openvela 工程。
 * 主循环一行代码不变。
 *
 * ============================ 编译 ============================
 *   PC 模拟：make app        （链接 hal/sim，跑一整晚压缩演示）
 *   真机：   openvela 工程内编译本文件 + hal/sf32lb52（见 Kconfig/CMakeLists）
 *
 * 与 demo/night_demo.c 的关系：night_demo 是【自包含】的单文件演示（信号
 * 合成写死在文件里）；本文件是【真机主循环】，信号来自 HAL。两者跑的是
 * 同一条逻辑链，night_demo 用来快速解释，本文件用来真正落地。
 */
#include "mianyu_hal.h"
#include "mianyu_sleep_detect.h"
#include "mianyu_schedule.h"
#include "mianyu_sleep_memory.h"
#include "mianyu_fade.h"
#include "mianyu_noise_gen.h"
#include "mianyu_breathe.h"
#include <stdio.h>
#include <string.h>

/* PC 模拟：跑一晚到 07:10 后结束（便于 CI/评审一键自证）。
 * 真机编译时置 0，主循环永不退出（设备常开）。 */
#ifndef MIANYU_DEMO_BOUNDED
#define MIANYU_DEMO_BOUNDED 1
#endif

#define TICK_MS        100                       /* 主循环节拍 100ms */
#define SAMPLES_PER_TICK (MY_SAMPLE_RATE / (1000 / TICK_MS))   /* 1600 */

/* ============================ 设备全局状态 ============================ */

typedef struct {
    my_sleep_detector_t det;
    my_sched_t          sched;
    my_sleep_memory_t   mem;
    my_fade_t           fade;
    my_noise_gen_t      noise;
    my_breathe_t        breathe;
    my_datetime_t       now;

    bool    light_on;            /* 灯是否亮（亮=按 breathe 引擎输出亮度） */
    bool    faded_out;           /* 本晚是否已淡出（避免入睡后重复触发） */
    int     aid_kind;            /* 当晚主用手段（偏好学习喂给记忆） */
    int64_t session_start_min;   /* 睡前流程起点（绝对分钟） */
    int     night_wake_count;    /* 夜醒次数（喂给记忆） */
    int     log_tick;            /* 每 5s 打一行状态的计数 */
} app_t;

static void print_status(app_t *a)
{
    const my_sleep_metrics_t *m = my_sleep_get_metrics(&a->det);
    my_sleep_state_t st = my_sleep_get_state(&a->det);
    const char *sn = st == MY_SLEEP_AWAKE ? "清醒" :
                     st == MY_SLEEP_DROWSY ? "困倦" : "入睡";
    int vol = my_fade_gain_pct(&a->fade);
    int lamp = a->light_on ? my_breathe_level_pct(&a->breathe) : 0;
    printf("  %02d:%02d:%02d  [%s] 规律度=%.2f 呼吸率=%5.1f/分 体动=%.3f  音量=%d%% 灯=%d%%\n",
           a->now.hour, a->now.minute, a->now.second, sn,
           m->regularity, m->breath_rate, m->movement, vol, lamp);
}

/* ============================ cron 回调（真机动作入口） ============================ */

static void on_task_fire(const my_task_t *task, const my_datetime_t *dt,
                         bool is_catchup, void *user)
{
    app_t *a = (app_t *)user;

    switch (task->kind) {
    case MY_TASK_BEDTIME:
        printf("[%02d:%02d:%02d] ★ 睡前流程启动：棕噪 70%% 淡入 + 呼吸引导灯%s\n",
               dt->hour, dt->minute, dt->second, is_catchup ? "（补触发）" : "");
        my_noise_set_kind(&a->noise, MY_NOISE_BROWN);
        my_noise_set_level(&a->noise, 70);
        a->aid_kind = MY_AID_BROWN;
        a->session_start_min = my_datetime_to_minutes(dt);
        my_fade_start(&a->fade);
        /* 呼吸引导灯：4-7-8 峰值 60，谷值 4（睡前低亮，不全灭） */
        a->breathe.cfg.peak_pct = 60; a->breathe.cfg.trough_pct = 4;
        a->light_on = true;
        a->faded_out = false;
        my_breathe_reset(&a->breathe);
        /* 重置入睡判定：让 first_asleep_sec 正好等于「从睡前流程开始的潜伏期」 */
        my_sleep_reset_state(&a->det);
        break;

    case MY_TASK_MORNING_WAKE: {
        printf("[%02d:%02d:%02d] ★ 晨唤：光渐亮 + 一晚记录落盘 + 睡眠简报\n",
               dt->hour, dt->minute, dt->second);
        /* 晨唤模拟"光渐亮"：峰值拉高 */
        a->breathe.cfg.peak_pct = 90; a->breathe.cfg.trough_pct = 10;
        a->light_on = true;
        my_breathe_reset(&a->breathe);

        /* 把这一晚写进记忆档案 */
        const my_sleep_stats_t *st = my_sleep_get_stats(&a->det);
        my_sleep_record_t rec;
        memset(&rec, 0, sizeof(rec));
        rec.date_key = dt->year * 10000 + dt->month * 100 + dt->day;
        rec.bedtime_min = (int32_t)(a->session_start_min % 1440);
        rec.sleep_onset_sec = (int32_t)st->first_asleep_sec;   /* 潜伏期 */
        /* 总睡眠时长（估计）：晨唤 - 睡前 - 潜伏期 - 夜醒折损（每夜醒约 15 分钟） */
        int64_t awake_min = my_datetime_to_minutes(dt);
        int64_t elapsed = awake_min - a->session_start_min;
        rec.total_sleep_min = (int32_t)(elapsed - st->first_asleep_sec / 60
                                        - a->night_wake_count * 15);
        rec.night_wake_count = (uint8_t)a->night_wake_count;
        rec.aid_used = (uint8_t)a->aid_kind;
        rec.quality = (uint8_t)(a->night_wake_count == 0 ? 88 : 80);
        my_mem_add(&a->mem, &rec);
        my_mem_flush(&a->mem);

        /* 晨间简报（记忆聚合输出） */
        my_sleep_summary_t sum;
        my_mem_summarize(&a->mem, 0, &sum);
        printf("  有效记录   : %d 晚\n", sum.valid_days);
        printf("  平均入睡潜伏: %d 秒\n", (int)sum.avg_onset_sec);
        printf("  平均睡眠时长: %d 分钟\n", (int)sum.avg_total_min);
        printf("  平均夜醒    : %.2f 次\n", sum.avg_wake_count / 100.0);
        printf("  入睡最快手段: %s\n",
               sum.best_aid == MY_AID_BROWN ? "棕噪" :
               sum.best_aid == MY_AID_WHITE ? "白噪" :
               sum.best_aid == MY_AID_PINK  ? "粉噪" : "（暂无）");
        /* 晨唤 = 本晚会话结束：复位检测器，避免把「晨醒」误判成第二次夜醒，
         * 也为下一晚的入睡判定清空状态。 */
        my_sleep_reset_state(&a->det);
        break;
    }

    case MY_TASK_NIGHT_COMFORT:
        /* 夜间巡检：无事件时静默，不打扰 */
        break;

    default:
        break;
    }
}

/* ============================ 主循环 ============================ */

int mianyu_main(int argc, FAR char *argv[])
{
    if (my_hal_init() != MY_OK) {
        printf("HAL 初始化失败，退出\n");
        return 1;
    }

    app_t a;
    memset(&a, 0, sizeof(a));

    /* 五个核心模块初始化 */
    my_sleep_init(&a.det);
    my_fade_cfg_t fcfg;
    my_fade_default_cfg(&fcfg);
    fcfg.fade_out_ms = 20000;   /* 演示用 20s 淡出（真机产品值 15 分钟） */
    fcfg.fade_in_ms  = 2000;
    my_fade_init(&a.fade, &fcfg);
    my_noise_init(&a.noise, MY_NOISE_BROWN, 42, 70);
    my_breathe_init(&a.breathe, NULL);
    my_mem_init(&a.mem, my_hal_mem_store());
    my_mem_load(&a.mem);
    my_sched_init(&a.sched, my_hal_sched_store(), on_task_fire, &a);
    /* 开机读盘：有历史作息则用，空盘/读盘失败则回退默认作息 */
    if (my_sched_load(&a.sched) != MY_OK || my_sched_task_count(&a.sched) == 0)
        my_sched_load_defaults(&a.sched);

    printf("==============================================================\n");
    printf(" 安眠科技 · 主动式哄睡智能体 —— 主循环（HAL 接入版）\n");
    printf(" 无任何用户指令，全靠 cron + 传感器事件驱动一整晚\n");
    printf("==============================================================\n");

    my_hal_time_now(&a.now);
    my_sched_start(&a.sched, &a.now);

    printf("[%02d:%02d:%02d] 设备就绪，等待睡前时刻\n", a.now.hour, a.now.minute, a.now.second);

    /* 主循环：每 100ms 一 tick */
    while (1) {
        my_hal_sleep_ms(TICK_MS);            /* 推进时间（真机 sleep 真实 100ms） */
        my_hal_time_now(&a.now);
        my_sched_tick(&a.sched, &a.now);     /* cron 触发检查 */

        /* ① 麦克风 → 入睡判定 */
        my_pcm_t mic[SAMPLES_PER_TICK];
        int got = my_hal_mic_read(mic, SAMPLES_PER_TICK);
        if (got > 0) my_sleep_feed_mic(&a.det, mic, got);

        /* ② IMU → 双模态增强（有则喂，无则纯 MIC 判据） */
        if (my_hal_imu_available()) {
            float ax, ay, az;
            if (my_hal_imu_read(&ax, &ay, &az) == MY_OK)
                my_sleep_feed_imu(&a.det, ax, ay, az);
        }

        /* ③ 入睡事件：淡出 + 灯灭（零播报，光害最小化） */
        my_sleep_state_t st = my_sleep_get_state(&a.det);
        if (st == MY_SLEEP_ASLEEP && !a.faded_out) {
            a.faded_out = true;
            /* transitions_to_asleep==1 是首次入睡，才报潜伏期；
             * 之后（夜醒复睡）first_asleep_sec 按设计不重置，再报会误导。 */
            if (my_sleep_get_stats(&a.det)->transitions_to_asleep <= 1) {
                int lat = (int)my_sleep_get_stats(&a.det)->first_asleep_sec;
                printf("[%02d:%02d:%02d] 入睡（潜伏 %d 秒）→ 20s 淡出 + 灯灭\n",
                       a.now.hour, a.now.minute, a.now.second, lat);
            } else {
                printf("[%02d:%02d:%02d] 二次入睡 → 20s 淡出 + 灯灭\n",
                       a.now.hour, a.now.minute, a.now.second);
            }
            my_fade_start_fade_out(&a.fade);
            a.light_on = false;
        }

        /* ④ 夜醒事件：极低音量安抚 + 微光（不亮屏、不追问） */
        uint32_t wk = my_sleep_get_stats(&a.det)->night_wake_events;
        if (wk > (uint32_t)a.night_wake_count) {
            a.night_wake_count = (int)wk;
            printf("[%02d:%02d:%02d] ★ 夜醒安抚：极低音量棕噪 20%% + 微光\n",
                   a.now.hour, a.now.minute, a.now.second);
            my_noise_set_level(&a.noise, 20);
            my_fade_start(&a.fade);
            a.breathe.cfg.peak_pct = 20; a.breathe.cfg.trough_pct = 3;
            a.light_on = true;
            a.faded_out = false;         /* 允许再次入睡后二次淡出 */
            my_breathe_reset(&a.breathe);
        }

        /* ⑤ 音频：渲染噪声 → 施加增益 → 出声 */
        my_fade_tick(&a.fade, TICK_MS);
        my_fade_phase_t ph = my_fade_get_phase(&a.fade);
        if (ph == MY_FADE_IN || ph == MY_FADE_HOLD || ph == MY_FADE_OUT) {
            my_pcm_t out[SAMPLES_PER_TICK];
            my_noise_render(&a.noise, out, SAMPLES_PER_TICK);
            my_fade_apply(&a.fade, out, SAMPLES_PER_TICK, my_fade_gain_q15(&a.fade));
            my_hal_audio_play(out, SAMPLES_PER_TICK);
        }

        /* ⑥ 灯光：呼吸引擎驱动（灯亮才上屏） */
        my_breathe_tick(&a.breathe, TICK_MS);
        my_hal_light_set(a.light_on ? my_breathe_level_pct(&a.breathe) : 0);

        /* ⑦ 每 5 秒一行状态（真机可关，演示保留） */
        if (++a.log_tick >= 5000 / TICK_MS) { a.log_tick = 0; print_status(&a); }

#if MIANYU_DEMO_BOUNDED
        /* 晨唤简报已由 MORNING_WAKE 回调打出；次日 07:10 后结束演示 */
        if (a.now.hour == 7 && a.now.minute >= 10) break;
#endif
    }

    my_sched_flush(&a.sched);
    my_mem_flush(&a.mem);
    printf("==============================================================\n");
    printf(" 主循环结束（真机编译 MIANYU_DEMO_BOUNDED=0 后为常开循环）。\n");
    printf("==============================================================\n");
    my_hal_deinit();
    return 0;
}
