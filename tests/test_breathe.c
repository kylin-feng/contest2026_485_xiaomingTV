/* SPDX-License-Identifier: Apache-2.0
 *
 * 安眠科技 · 呼吸节律灯光引擎单元测试（PC 端）
 *
 * 测试重点（对位技术报告 3.3"光引导呼吸"）：
 *   1. 默认 4-7-8 配置：周期 19s，四段时长精确
 *   2. 波形不变量：
 *      - 吸气段单调上升（不回落）
 *      - 屏息段恒为峰值（不变，引导"停住"）
 *      - 呼气段单调下降（比吸气慢，时长 8s）
 *      - 谷值 >0 且等于 trough_pct（灯不全灭）
 *   3. 段边界无跳变：边界前后 1ms 亮度差 ≤ 1（smoothstep 连续性）
 *   4. 循环：tick 越过一个完整周期后回到同相位同亮度（可重复性）
 *   5. 参数校验：非法时长/亮度被拒
 *   6. 结构体预算
 *
 * 编译： cc -std=c11 -O2 -I../include -o test_breathe test_breathe.c ../src/mianyu_breathe.c
 * 运行： ./test_breathe
 */
#include "mianyu_breathe.h"
#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, fmt, ...) do { \
    if (cond) { g_pass++; printf("  [PASS] " fmt "\n", ##__VA_ARGS__); } \
    else      { g_fail++; printf("  [FAIL] " fmt "\n", ##__VA_ARGS__); } \
} while (0)

/* 推进到指定时刻（delta 累加） */
static void advance_to(my_breathe_t *b, int32_t ms)
{
    /* 直接从 0 起推进：为测任意时刻，先 reset 再一次性 tick */
    my_breathe_reset(b);
    my_breathe_tick(b, ms);
}

static int level_at(const my_breathe_cfg_t *cfg, int32_t ms)
{
    my_breathe_t b;
    my_breathe_init(&b, cfg);
    advance_to(&b, ms);
    return my_breathe_level_pct(&b);
}

static my_breath_phase_t phase_at(const my_breathe_cfg_t *cfg, int32_t ms)
{
    my_breathe_t b;
    my_breathe_init(&b, cfg);
    advance_to(&b, ms);
    return my_breathe_get_phase(&b);
}

/* ===================== 1. 默认 4-7-8 节奏 ===================== */
static void test_default_478(void)
{
    printf("\n=== 1. 默认 4-7-8 呼吸法（19s 周期）===\n");
    my_breathe_cfg_t cfg;
    my_breathe_default_cfg(&cfg);
    CHECK(cfg.inhale_ms == 4000 && cfg.hold_ms == 7000 && cfg.exhale_ms == 8000,
          "默认 吸4/屏7/呼8");
    my_breathe_t b;
    my_breathe_init(&b, &cfg);
    CHECK(my_breathe_cycle_ms(&b) == 19000, "周期 19s（实测 %d）", my_breathe_cycle_ms(&b));

    /* 相位边界 */
    CHECK(phase_at(&cfg, 0)      == MY_BREATH_INHALE, "t=0 吸气");
    CHECK(phase_at(&cfg, 3999)   == MY_BREATH_INHALE, "t=3999 仍吸气");
    CHECK(phase_at(&cfg, 4000)   == MY_BREATH_HOLD,   "t=4000 进屏息");
    CHECK(phase_at(&cfg, 4001)   == MY_BREATH_HOLD,   "t=4001 屏息中");
    CHECK(phase_at(&cfg, 10999)  == MY_BREATH_HOLD,   "t=10999 屏息末");
    CHECK(phase_at(&cfg, 11000)  == MY_BREATH_EXHALE, "t=11000 进呼气");
    CHECK(phase_at(&cfg, 18999)  == MY_BREATH_EXHALE, "t=18999 呼气末");
    CHECK(phase_at(&cfg, 19000)  == MY_BREATH_INHALE, "t=19000 新周期吸气（循环）");
    printf("  峰值 %d%%, 谷值 %d%%\n", cfg.peak_pct, cfg.trough_pct);
}

/* ===================== 2. 波形不变量 ===================== */
static void test_waveform(void)
{
    printf("\n=== 2. 波形不变量（单调/恒值/谷值）===\n");
    my_breathe_cfg_t cfg;
    my_breathe_default_cfg(&cfg);
    cfg.peak_pct = 60; cfg.trough_pct = 4;

    /* 吸气段：每 500ms 采样，应单调不减且整体上升 */
    int prev = level_at(&cfg, 0);
    CHECK(prev == 4, "t=0 亮度 = 谷值 4%%（实测 %d）", prev);
    int rose = 0, fell = 0;
    for (int ms = 500; ms <= 3500; ms += 500) {
        int l = level_at(&cfg, ms);
        if (l > prev) rose++;
        if (l < prev) fell++;
        prev = l;
    }
    CHECK(fell == 0, "吸气段无回落（采样 7 点，回落 %d 次）", fell);
    CHECK(rose >= 5, "吸气段整体上升（上升 %d 次）", rose);
    int peak_at_end_inhale = level_at(&cfg, 4000);
    CHECK(peak_at_end_inhale == 60, "吸气结束亮度 = 峰值 60%%（实测 %d）",
          peak_at_end_inhale);

    /* 屏息段：恒为峰值 */
    int hold_steady = 1;
    for (int ms = 4500; ms <= 10500; ms += 1000) {
        if (level_at(&cfg, ms) != 60) hold_steady = 0;
    }
    CHECK(hold_steady, "屏息段恒为峰值 60%%（7 个采样点）");

    /* 呼气段：单调不减是不可能的，应单调不增且整体下降 */
    prev = level_at(&cfg, 11000);
    CHECK(prev == 60, "呼气起点 = 峰值（实测 %d）", prev);
    rose = fell = 0;
    for (int ms = 11500; ms <= 18500; ms += 1000) {
        int l = level_at(&cfg, ms);
        if (l > prev) rose++;
        if (l < prev) fell++;
        prev = l;
    }
    CHECK(rose == 0, "呼气段无回升（采样 8 点，回升 %d 次）", rose);
    CHECK(fell >= 6, "呼气段整体下降（下降 %d 次）", fell);

    /* 谷值：呼气结束回到 trough，且 >0（灯不全灭） */
    int end_exhale = level_at(&cfg, 18999);
    CHECK(end_exhale == 4, "呼气结束亮度 = 谷值 4%%（实测 %d）", end_exhale);
    CHECK(end_exhale > 0, "谷值 >0：灯保持微光，不会'突然灭了'");
}

/* ===================== 3. 段边界连续性（防跳变） ===================== */
static void test_boundary_smooth(void)
{
    printf("\n=== 3. 段边界无亮度跳变（smoothstep 连续）===\n");
    my_breathe_cfg_t cfg;
    my_breathe_default_cfg(&cfg);
    cfg.peak_pct = 80; cfg.trough_pct = 10;   /* 拉大动态范围放大跳变 */

    /* 三个边界：吸→屏(4000)、屏→呼(11000)、呼→吸(19000) */
    struct { int32_t at; const char *name; } bounds[] = {
        { 4000,  "吸→屏" }, { 11000, "屏→呼" }, { 19000, "呼→吸(循环)" },
    };
    int worst = 0;
    for (unsigned i = 0; i < sizeof(bounds)/sizeof(bounds[0]); i++) {
        int before = level_at(&cfg, bounds[i].at - 1);
        int after  = level_at(&cfg, bounds[i].at);
        int d = after > before ? after - before : before - after;
        if (d > worst) worst = d;
        printf("  [%s] 前 %d%% → 后 %d%%（差 %d）\n", bounds[i].name, before, after, d);
        CHECK(d <= 1, "%s 边界亮度差 ≤1%%（实测 %d）", bounds[i].name, d);
    }
}

/* ===================== 4. 循环可重复性 ===================== */
static void test_cycle_repeat(void)
{
    printf("\n=== 4. 循环可重复（同相位同亮度）===\n");
    my_breathe_cfg_t cfg;
    my_breathe_default_cfg(&cfg);

    /* 0 与 19000、1000 与 20000 应同亮度同相位 */
    CHECK(level_at(&cfg, 0) == level_at(&cfg, 19000), "t=0 与 t=19000 同亮度");
    CHECK(phase_at(&cfg, 0) == phase_at(&cfg, 19000), "t=0 与 t=19000 同相位");
    CHECK(level_at(&cfg, 1000) == level_at(&cfg, 20000), "t=1000 与 t=20000 同亮度");
    CHECK(phase_at(&cfg, 1000) == phase_at(&cfg, 20000), "t=1000 与 t=20000 同相位");

    /* 大跨度 tick：一次 tick 38000ms 与两次 19000ms 等价（累计正确） */
    my_breathe_t b1, b2;
    my_breathe_init(&b1, &cfg);
    my_breathe_init(&b2, &cfg);
    my_breathe_tick(&b1, 38000);
    my_breathe_tick(&b2, 19000);
    my_breathe_tick(&b2, 19000);
    CHECK(my_breathe_level_pct(&b1) == my_breathe_level_pct(&b2),
          "一次 tick 38s == 两次 19s（亮度一致）");
    CHECK(my_breathe_get_phase(&b1) == my_breathe_get_phase(&b2),
          "一次 tick 38s == 两次 19s（相位一致）");
}

/* ===================== 5. 参数校验 ===================== */
static void test_param_check(void)
{
    printf("\n=== 5. 参数校验 ===\n");
    my_breathe_t b;
    my_breathe_cfg_t cfg;
    my_breathe_default_cfg(&cfg);

    my_breathe_cfg_t bad = cfg; bad.inhale_ms = -1;
    CHECK(my_breathe_init(&b, &bad) == MY_ERR_PARAM, "负时长被拒");

    bad = cfg; bad.hold_ms = -5;
    CHECK(my_breathe_init(&b, &bad) == MY_ERR_PARAM, "负 hold 被拒");

    bad = cfg; bad.inhale_ms = 0; bad.exhale_ms = 0;
    CHECK(my_breathe_init(&b, &bad) == MY_ERR_PARAM, "吸呼全 0（灯不动）被拒");

    bad = cfg; bad.peak_pct = 101;
    CHECK(my_breathe_init(&b, &bad) == MY_ERR_PARAM, "峰值 >100 被拒");

    bad = cfg; bad.peak_pct = 30; bad.trough_pct = 50;
    CHECK(my_breathe_init(&b, &bad) == MY_ERR_PARAM, "谷值高于峰值被拒");

    bad = cfg; bad.peak_pct = 30; bad.trough_pct = 30;
    CHECK(my_breathe_init(&b, &bad) == MY_OK, "峰谷相等合法（恒亮模式）");

    CHECK(my_breathe_tick(&b, -1) == MY_ERR_PARAM, "负 delta 被拒");
    CHECK(my_breathe_tick(NULL, 100) == MY_ERR_PARAM, "tick(NULL) 被拒");
    CHECK(my_breathe_init(NULL, &cfg) == MY_ERR_PARAM, "init(NULL) 被拒");
    CHECK(my_breathe_init(&b, NULL) == MY_OK, "cfg=NULL 用默认 4-7-8");
}

/* ===================== 6. 结构体预算与空 tick ===================== */
static void test_sizes(void)
{
    printf("\n=== 6. 结构体预算 ===\n");
    CHECK(sizeof(my_breathe_t) < 64, "引擎状态 %.0f 字节 < 64B（LVGL 对象 user_data 都够放）",
          (double)sizeof(my_breathe_t));
    /* 空 tick 不改变亮度 */
    my_breathe_t b;
    my_breathe_cfg_t cfg;
    my_breathe_default_cfg(&cfg);
    my_breathe_init(&b, &cfg);
    int l0 = my_breathe_level_pct(&b);
    my_breathe_tick(&b, 0);
    CHECK(my_breathe_level_pct(&b) == l0, "tick(0) 不改变亮度");
}

int main(void)
{
    printf("============================================\n");
    printf(" 安眠科技 · 呼吸节律灯光引擎单元测试\n");
    printf(" 默认 4-7-8：吸4s 屏7s 呼8s = 19s 周期\n");
    printf("============================================\n");

    test_default_478();
    test_waveform();
    test_boundary_smooth();
    test_cycle_repeat();
    test_param_check();
    test_sizes();

    printf("\n============================================\n");
    printf(" 结果： %d 通过 / %d 失败\n", g_pass, g_fail);
    printf("============================================\n");
    printf("MY_RESULT: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
