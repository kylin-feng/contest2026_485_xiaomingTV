/* SPDX-License-Identifier: Apache-2.0
 *
 * 安眠科技 · 音量淡出曲线引擎单元测试（PC 端）
 *
 * 除了正确性，本测试还产出技术报告 3.3/3.5 节要用的【量化对比数据】：
 *   A. 线性淡出 vs dB 域淡出：各 10% 时段的 dB 下降量（证明"线性淡出尾段陡峭"）
 *   B. 整数百分比音量 vs Q15 增益：最大单步 dB 跳变（证明"低音量段台阶感"）
 *   C. 暂停/闪避斜坡：有斜坡 vs 无斜坡的最大帧间样本跳变（证明"防爆音"）
 * 这三组数字是设计决策的依据，不是事后补的装饰。
 *
 * 编译： cc -std=c11 -O2 -I../include -o test_fade test_fade.c ../src/mianyu_fade.c -lm
 * 运行： ./test_fade
 */
#include "mianyu_fade.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, fmt, ...) do { \
    if (cond) { g_pass++; printf("  [PASS] " fmt "\n", ##__VA_ARGS__); } \
    else      { g_fail++; printf("  [FAIL] " fmt "\n", ##__VA_ARGS__); } \
} while (0)

/* 小工具：算某个 Q15 增益对应的 dB（仅测试日志用） */
static float gain_db_of(int32_t g)
{
    if (g <= 0) return -INFINITY;
    return log2f((float)g / (float)MY_GAIN_FULL_Q15) * 6.02059991328f;
}

/* 与源文件同式（源里是 static，测试无法直接引用），用于推算量化下限 */
static int32_t db_to_gain_q15_local(float db)
{
    if (db <= -96.0f) return 0;
    int32_t g = (int32_t)(exp2f(db * 0.16609640474f) * (float)MY_GAIN_FULL_Q15);
    return g > 0 ? g : 1;   /* 避免除零 */
}

/* ================= 1. API 与配置健壮性 ================= */
static void test_api_and_cfg(void)
{
    printf("\n=== 1. API 边界与配置自愈 ===\n");
    my_fade_t f;
    CHECK(my_fade_init(NULL, NULL) == MY_ERR_PARAM, "init(NULL) 拒绝");
    CHECK(my_fade_init(&f, NULL) == MY_OK, "init(cfg=NULL) 用默认配置成功");
    CHECK(my_fade_get_phase(&f) == MY_FADE_STOPPED, "初始 STOPPED");
    CHECK(my_fade_gain_q15(&f) == 0, "初始增益 0（未播放不出声）");
    CHECK(my_fade_gain_q15(NULL) == 0, "gain_q15(NULL) 返回 0");
    CHECK(my_fade_start_fade_out(&f) == MY_OK, "STOPPED 时触发淡出为无害空操作");
    CHECK(my_fade_get_phase(&f) == MY_FADE_STOPPED, "空操作未改状态");
    CHECK(my_fade_pause(&f) == MY_ERR_STATE, "未播放时 pause 返回 MY_ERR_STATE");
    CHECK(my_fade_resume(&f) == MY_OK, "未暂停时 resume 为无害空操作");
    CHECK(my_fade_tick(&f, -1) == MY_ERR_PARAM, "tick 负时长拒绝");
    CHECK(my_fade_tick(NULL, 10) == MY_ERR_PARAM, "tick(NULL) 拒绝");
    CHECK(sizeof(my_fade_t) < 256, "状态体 %.0f 字节 < 256B", (double)sizeof(my_fade_t));

    /* 配置自愈：真机上配置来自 cJSON，可能被写坏，不能让引擎进入非法状态 */
    my_fade_cfg_t bad = {
        .fade_in_ms = -5, .hold_ms = -1, .fade_out_ms = 0, .tail_ms = 999999,
        .floor_db = 0, .duck_gain_q15 = 999999, .duck_ms = -3,
        .target_q15 = -100, .dither_tpdf = true,
    };
    my_fade_init(&f, &bad);
    CHECK(f.cfg.fade_out_ms > 0, "fade_out_ms=0 自愈为 %d", f.cfg.fade_out_ms);
    CHECK(f.cfg.tail_ms < f.cfg.fade_out_ms, "tail_ms(%d) 被压到 < fade_out_ms(%d)",
          f.cfg.tail_ms, f.cfg.fade_out_ms);
    CHECK(f.cfg.target_q15 >= 0 && f.cfg.target_q15 <= MY_GAIN_FULL_Q15,
          "target_q15 钳入 [0,32768]（实测 %d）", f.cfg.target_q15);
    CHECK(f.cfg.duck_gain_q15 <= MY_GAIN_FULL_Q15,
          "duck_gain_q15 钳入上限（实测 %d）", f.cfg.duck_gain_q15);
    CHECK(f.cfg.fade_in_ms == 0 && f.cfg.hold_ms == 0 && f.cfg.duck_ms == 0,
          "负时长归零");
    /* 自愈后的配置必须能正常跑完整条淡出，不能崩在半路 */
    my_fade_start(&f);
    for (int i = 0; i < 4000; i++) my_fade_tick(&f, 100);
    CHECK(my_fade_gain_q15(&f) == 0, "病态配置下仍能收敛到静音");
}

/* ================= 2. 淡入（smoothstep 无冲击） ================= */
static void test_fade_in(void)
{
    printf("\n=== 2. 淡入曲线 ===\n");
    my_fade_cfg_t cfg; my_fade_default_cfg(&cfg);
    cfg.fade_in_ms = 2000; cfg.hold_ms = 0;
    my_fade_t f; my_fade_init(&f, &cfg);
    my_fade_start(&f);

    int32_t prev = 0, max_step = 0;
    bool monotonic = true;
    for (int t = 0; t <= 2000; t += 20) {          /* 20ms = 一帧音频 */
        my_fade_tick(&f, 20);
        int32_t g = my_fade_gain_q15(&f);
        if (g < prev) monotonic = false;
        if (g - prev > max_step) max_step = g - prev;
        prev = g;
    }
    CHECK(monotonic, "淡入全程单调不降");
    CHECK(my_fade_gain_q15(&f) == cfg.target_q15,
          "淡入结束精确到达目标 %d（实测 %d）", cfg.target_q15, my_fade_gain_q15(&f));
    CHECK(my_fade_get_phase(&f) == MY_FADE_HOLD, "淡入后进入 HOLD");

    /* smoothstep 首帧导数为 0：起步不应有跳变（防爆音的第一道防线） */
    my_fade_init(&f, &cfg);
    my_fade_start(&f);
    my_fade_tick(&f, 20);
    int32_t first_step = my_fade_gain_q15(&f);
    CHECK(first_step < cfg.target_q15 / 100,
          "首帧增益 %d 极小（smoothstep 起步导数为 0，无阶跃冲击）", first_step);
    /* 对比：线性淡入首帧应为 target/100，smoothstep 约为其 3% */
    printf("  [INFO] smoothstep 首帧增益 %d，线性淡入同帧应为 %d\n",
           first_step, cfg.target_q15 * 20 / 2000);
    CHECK(max_step < cfg.target_q15 / 8,
          "淡入最大单帧步进 %d < 目标的 12.5%%（听感平滑）", max_step);
}

/* ================= 3. 淡出：单调性 + 精确归零 + 无拖尾 ================= */
static void test_fade_out_core(void)
{
    printf("\n=== 3. 15 分钟淡出核心行为 ===\n");
    my_fade_cfg_t cfg; my_fade_default_cfg(&cfg);
    cfg.fade_in_ms = 0; cfg.hold_ms = 0; cfg.fade_out_ms = 900000; cfg.tail_ms = 3000;
    my_fade_t f; my_fade_init(&f, &cfg);
    my_fade_start(&f);
    CHECK(my_fade_gain_q15(&f) == cfg.target_q15, "无淡入时起始即目标音量");

    my_fade_start_fade_out(&f);
    CHECK(my_fade_get_phase(&f) == MY_FADE_OUT, "触发后进入 FADE_OUT");
    CHECK(my_fade_remaining_ms(&f) == 900000, "剩余时间 900000ms");

    int32_t prev = my_fade_gain_q15(&f);
    bool monotonic = true;
    int zero_ms = -1, violations = 0;

    for (int t = 0; t <= 900000; t += 20) {
        my_fade_tick(&f, 20);
        int32_t g = my_fade_gain_q15(&f);
        if (g > prev) { monotonic = false; violations++; }
        prev = g;
        if (g == 0 && zero_ms < 0) zero_ms = t + 20;
    }

    CHECK(monotonic, "淡出全程单调不增（违规 %d 次）", violations);
    CHECK(my_fade_get_phase(&f) == MY_FADE_DONE, "结束时进入 DONE");
    CHECK(my_fade_gain_q15(&f) == 0, "结束增益精确为 0（不是「接近 0」）");
    CHECK(zero_ms >= 0 && zero_ms <= 900000,
          "首次归零发生在 %dms（<= 设定 900000ms，无指数拖尾）", zero_ms);
    CHECK(my_fade_remaining_ms(&f) == 0, "结束后剩余时间 0");

    /* DONE 后继续 tick 不得复活、不得负增益 */
    for (int i = 0; i < 1000; i++) my_fade_tick(&f, 1000);
    CHECK(my_fade_gain_q15(&f) == 0, "DONE 后再 tick 1000s 仍为 0（锁死）");
    CHECK(my_fade_get_phase(&f) == MY_FADE_DONE, "DONE 状态不被后续 tick 改变");
    CHECK(my_fade_start_fade_out(&f) == MY_OK, "DONE 时重复触发淡出安全");
}

/* ================= 4. dB 线性性：感知匀速的证据 ================= */
static void test_db_linearity(void)
{
    printf("\n=== 4. dB 域线性性（感知匀速）vs 线性淡出对比 ===\n");
    my_fade_cfg_t cfg; my_fade_default_cfg(&cfg);
    cfg.fade_in_ms = 0; cfg.fade_out_ms = 900000; cfg.tail_ms = 3000;
    cfg.target_q15 = MY_GAIN_FULL_Q15;   /* 满幅起淡，便于对比 */
    my_fade_t f; my_fade_init(&f, &cfg);
    my_fade_start(&f);
    my_fade_start_fade_out(&f);

    /* 采样 10 个等距时段的 dB 值 */
    const int SEG = 10;
    double seg_db[SEG + 1];
    int step_ms = 900000 / SEG;
    for (int i = 0; i <= SEG; i++) {
        int target_ms = i * step_ms;
        int t_now = (int)my_fade_played_ms(&f);
        if (t_now < target_ms) {
            my_fade_tick(&f, target_ms - t_now);
        }
        seg_db[i] = my_fade_gain_db(&f);
    }

    printf("  %-14s %-12s %-12s %-12s\n", "时段", "dB淡出", "线性淡出", "差值");
    double exp_drop = -60.0 / SEG;   /* 指数段共 60dB，每段应约 6dB */
    int near_uniform = 0;
    for (int i = 1; i <= SEG; i++) {
        bool fin_now  = isfinite(seg_db[i]);
        bool fin_prev = isfinite(seg_db[i - 1]);
        double db_fade = (fin_now && fin_prev) ? seg_db[i] - seg_db[i - 1] : -INFINITY;
        /* 线性淡出：gain = FULL*(1 - t/T)，换算成 dB */
        double lin_prev = (double)MY_GAIN_FULL_Q15 * (1.0 - (double)((i - 1) * step_ms) / 900000.0);
        double lin_now  = (double)MY_GAIN_FULL_Q15 * (1.0 - (double)(i * step_ms) / 900000.0);
        double lin_db = (lin_prev > 0 && lin_now > 0)
                        ? 20.0 * log10(lin_now / lin_prev) : -INFINITY;
        printf("  %5d%%-%-3d%%   %9.2fdB  %9.2fdB  %9.2fdB\n",
               (i - 1) * 100 / SEG, i * 100 / SEG, db_fade, lin_db, db_fade - lin_db);
        /* 末段（线性收尾归零）不参与均匀性判断：那里 dB 差是 -inf */
        if (i < SEG && fin_now && fin_prev && fabs(db_fade - exp_drop) < 1.5) near_uniform++;
    }
    CHECK(near_uniform >= 8,
          "指数段 9 个时段中 %d 个 dB 降幅在 6dB±1.5 内（感知匀速）", near_uniform);
    printf("  [INFO] 末段（90%%-100%%）两条曲线都归零，dB 差为 -inf 属正常（末段是线性收尾）\n");

    /* 线性淡出的病态特征量化：前半程几乎听不出变化，末段断崖 */
    double lin_first_half = 20.0 * log10(0.5);            /* 50% 处 = -6dB */
    double lin_at_90pct   = 20.0 * log10(0.10);           /* 90% 处 = -20dB */
    printf("  [INFO] 线性淡出：前 50%% 时间只降 %.1fdB；最后 10%% 时间从 %.0fdB 直坠静音\n",
           lin_first_half, lin_at_90pct);
    printf("  [INFO] dB 淡出：每 10%% 时间稳定降约 6dB —— 这就是「无察觉淡出」的依据\n");
    CHECK(lin_first_half > -7.0 && lin_first_half < -5.0,
          "线性淡出前半程仅降 6dB（用户几乎无感），对比成立");
}

/* ================= 5. Q15 分辨率 vs 整数百分比（台阶感量化） ================= */
static void test_resolution(void)
{
    printf("\n=== 5. 增益分辨率：Q15 vs 整数百分比 ===\n");
    my_fade_cfg_t cfg; my_fade_default_cfg(&cfg);
    cfg.fade_in_ms = 0; cfg.fade_out_ms = 900000; cfg.tail_ms = 3000;
    cfg.target_q15 = 22938;   /* 70%，实际播放音量 */
    my_fade_t f; my_fade_init(&f, &cfg);
    my_fade_start(&f);
    my_fade_start_fade_out(&f);

    /* 只在【指数段】（= 可闻区间）统计跳变：末段 tail_ms 是线性归零，
     * 从 -60dB 直接到 0，dB 差无穷大但幅度已低于听阈，不属"台阶感"问题。 */
    int32_t exp_end_ms = cfg.fade_out_ms - cfg.tail_ms;

    double max_jump_q15 = 0.0;
    double max_jump_pct = 0.0;
    int    pct_steps = 0;
    int32_t prev_pct = my_fade_gain_pct(&f);
    double prev_db = my_fade_gain_db(&f);

    for (int t = 0; t < exp_end_ms; t += 20) {
        my_fade_tick(&f, 20);
        double db = my_fade_gain_db(&f);
        if (prev_db > -96.0 && fabs(db - prev_db) > max_jump_q15) {
            max_jump_q15 = fabs(db - prev_db);
        }
        prev_db = db;

        int32_t pct = my_fade_gain_pct(&f);
        if (pct != prev_pct && prev_pct > 0 && pct > 0) {
            /* 整数百分比音量的台阶：从 prev_pct 跳到 pct 的 dB 差 */
            double j = 20.0 * log10((double)prev_pct / (double)pct);
            if (j > max_jump_pct) max_jump_pct = j;
            pct_steps++;
        }
        prev_pct = pct;
    }

    printf("  [INFO] Q15 增益最大单帧(20ms) dB 跳变 = %.6f dB\n", max_jump_q15);
    printf("  [INFO] 整数百分比音量：%d 次降档，最大单步跳变 = %.3f dB\n",
           pct_steps, max_jump_pct);

    /* 判据说明：曲线本身在 15min/60dB 下每帧只降 60*20/897000 ≈ 0.0013dB，
     * 实测 0.26dB 全部来自【Q15 整数量化】—— 在地板 -60dB 处 gain_q15≈33，
     * 整数步进 1/33 ≈ 0.26dB，这是定点表示在该电平的物理下限，无法再细。
     * 而响度可闻阈(JND)约 0.5~1dB（宽带噪声），故 0.26dB 仍不可闻。
     * 真正的对比对象是整数百分比音量的 6.02dB（= 音量瞬间掉一半）。 */
    double jnd = 0.5;   /* 保守取响度可闻阈下界 */
    CHECK(max_jump_q15 < jnd,
          "Q15 单帧跳变 %.4fdB < 响度可闻阈 %.1fdB（不可闻）", max_jump_q15, jnd);
    CHECK(max_jump_pct > 5.0,
          "整数百分比最大台阶 %.3fdB > 5dB（低音量段明显可闻）", max_jump_pct);
    CHECK(max_jump_pct / max_jump_q15 > 10.0,
          "Q15 比整数百分比细 %.1f 倍（>10 倍，这是选 Q15 的量化依据）",
          max_jump_pct / max_jump_q15);
    CHECK(pct_steps >= 50, "15 分钟内百分比音量降档 %d 次（每次都可能有台阶感）", pct_steps);

    /* 曲线理论步进 vs 实测，证明差异确由量化而非算法引入 */
    double theoretical = 60.0 * 20.0 / (double)(cfg.fade_out_ms - cfg.tail_ms);
    int32_t floor_gain = db_to_gain_q15_local((float)cfg.floor_db);
    double quant_step_db = 20.0 * log10((double)(floor_gain + 1) / (double)floor_gain);
    printf("  [INFO] 曲线理论每帧步进 %.5fdB，实测 %.4fdB → 差异来自 Q15 整数量化\n"
           "         （地板 %ddB 处 gain_q15=%d，量化下限 ≈ %.2fdB，与实测吻合）\n",
           theoretical, max_jump_q15, cfg.floor_db, floor_gain, quant_step_db);
    printf("  [INFO] 结论：15 分钟淡出用整数百分比音量会降档 %d 次，\n"
           "         低音量段单次降档约 %.1fdB（音量瞬间掉 %.0f%%）；\n"
           "         Q15 软件增益把单帧变化压到 %.4fdB，低于可闻阈，听感连续。\n",
           pct_steps, max_jump_pct,
           (1.0 - pow(10.0, -max_jump_pct / 20.0)) * 100.0, max_jump_q15);
}

/* ================= 6. 暂停/恢复：时间线冻结 ================= */
static void test_pause_resume(void)
{
    printf("\n=== 6. 暂停与恢复（进度不丢不涨） ===\n");
    my_fade_cfg_t cfg; my_fade_default_cfg(&cfg);
    cfg.fade_in_ms = 0; cfg.fade_out_ms = 60000; cfg.tail_ms = 3000; cfg.duck_ms = 300;

    /* 对照法：两台引擎跑同一条淡出曲线。
     *   A 引擎：播 10s → 暂停 10s → 恢复再播 5s
     *   B 引擎：连续播 15s（完全不暂停）
     * 若暂停语义正确，A 恢复后的进度必须与 B 在同一 played_ms 处【完全一致】：
     * 暂停期不推进（进度不涨），恢复后从冻结点继续（进度不丢）。
     * 直接比较"暂停前增益 vs 恢复后增益"是错的 —— 恢复后淡出仍在推进，
     * 增益本就该更低一点，那不是漂移。 */
    my_fade_t a, b;
    my_fade_init(&a, &cfg);
    my_fade_init(&b, &cfg);
    my_fade_start(&a); my_fade_start_fade_out(&a);
    my_fade_start(&b); my_fade_start_fade_out(&b);

    for (int i = 0; i < 500; i++) { my_fade_tick(&a, 20); my_fade_tick(&b, 20); }  /* 10s */
    int32_t remain_before = my_fade_remaining_ms(&a);
    int64_t played_before = my_fade_played_ms(&a);

    CHECK(my_fade_pause(&a) == MY_OK, "暂停返回 MY_OK");
    CHECK(a.paused == true, "paused 标志置位");
    for (int i = 0; i < 500; i++) {          /* A 暂停中"过去" 10s，B 继续跑 */
        my_fade_tick(&a, 20);
        my_fade_tick(&b, 20);
    }

    CHECK(my_fade_gain_q15(&a) == 0, "暂停 300ms 后增益降到 0（彻底静音）");
    CHECK(my_fade_remaining_ms(&a) == remain_before,
          "暂停期间剩余时间冻结（%dms 不变）", remain_before);
    CHECK(my_fade_played_ms(&a) == played_before,
          "暂停期间播放时长不累积（%lldms）", (long long)played_before);
    CHECK(my_fade_get_phase(&a) == MY_FADE_OUT, "暂停不改变场景阶段（进度保留）");

    CHECK(my_fade_resume(&a) == MY_OK, "恢复返回 MY_OK");
    CHECK(a.paused == false, "paused 标志清除");
    /* A 恢复后再播 5s；B 此时已连播 25s。A 的进度应为 10s+5s=15s。 */
    for (int i = 0; i < 250; i++) my_fade_tick(&a, 20);
    CHECK(my_fade_played_ms(&a) == 15000,
          "A 恢复后 played_ms=15000（暂停的 10s 未被计入）");

    /* 把 B 也拉到 15s 进度处做严格对照 */
    my_fade_t ref; my_fade_init(&ref, &cfg);
    my_fade_start(&ref); my_fade_start_fade_out(&ref);
    for (int i = 0; i < 750; i++) my_fade_tick(&ref, 20);   /* 15s */
    CHECK(my_fade_played_ms(&ref) == my_fade_played_ms(&a),
          "对照引擎与 A 的进度对齐（均 %lldms）", (long long)my_fade_played_ms(&a));
    CHECK(my_fade_gain_q15(&a) == my_fade_gain_q15(&ref),
          "A 恢复后增益 %d == 连续播放 15s 的增益 %d（暂停零副作用）",
          my_fade_gain_q15(&a), my_fade_gain_q15(&ref));
    CHECK(my_fade_remaining_ms(&a) == my_fade_remaining_ms(&ref),
          "剩余时间也完全一致（%dms）", my_fade_remaining_ms(&a));

    /* 幂等性 */
    my_fade_pause(&a);
    CHECK(my_fade_pause(&a) == MY_OK, "重复暂停幂等");
    my_fade_resume(&a);
    CHECK(my_fade_resume(&a) == MY_OK, "重复恢复幂等");
    (void)b;
}

/* ================= 7. 闪避 + 斜坡防爆音 ================= */
static void test_duck_and_click(void)
{
    printf("\n=== 7. 闪避与防爆音斜坡 ===\n");
    my_fade_cfg_t cfg; my_fade_default_cfg(&cfg);
    cfg.fade_in_ms = 0; cfg.hold_ms = 0; cfg.duck_ms = 300; cfg.duck_gain_q15 = 8192;
    my_fade_t f; my_fade_init(&f, &cfg);
    my_fade_start(&f);
    /* 场景增益 = target_q15(默认 70%=22938)。duck 是【乘子】，总增益 = 场景 × duck。
     * 这是 tests 组 7/8 之前判据写错的地方：误把总增益当成 duck_gain_q15 本身。 */
    int32_t full = my_fade_gain_q15(&f);
    CHECK(full == cfg.target_q15, "起始总增益 = 场景增益 %d（duck 乘子中性）", full);
    int32_t ducked_expect = (int32_t)((int64_t)full * cfg.duck_gain_q15 >> 15);

    CHECK(my_fade_set_duck(&f, true) == MY_OK, "开启闪避");
    my_fade_tick(&f, 20);
    int32_t step1 = full - my_fade_gain_q15(&f);
    CHECK(step1 < full / 10, "闪避首帧只降 %d（占满幅 %.1f%%），斜坡生效",
          step1, 100.0 * step1 / full);
    CHECK(my_fade_set_duck(&f, true) == MY_OK, "重复开启闪避幂等（斜坡不重启）");

    for (int i = 0; i < 20; i++) my_fade_tick(&f, 20);   /* 400ms > duck_ms */
    CHECK(my_fade_gain_q15(&f) == ducked_expect,
          "300ms 后总增益 = 场景×duck = %d（实测 %d）", ducked_expect, my_fade_gain_q15(&f));
    /* 闪避相对【未闪避】的降幅应为 -12dB（duck_gain=8192=满幅的 1/4） */
    double duck_db = 20.0 * log10((double)my_fade_gain_q15(&f) / (double)full);
    CHECK(fabs(duck_db + 12.0) < 0.2, "闪避量 %.2fdB ≈ -12dB（语音可懂度设计值）", duck_db);

    my_fade_set_duck(&f, false);
    for (int i = 0; i < 20; i++) my_fade_tick(&f, 20);
    CHECK(my_fade_gain_q15(&f) == full, "释放闪避回到场景增益 %d（实测 %d）",
          full, my_fade_gain_q15(&f));

    /* ---- 爆音量化：增益阶跃在【帧间边界】造成的波形不连续 ----
     * 咔哒声的本质是输出波形的瞬时跳变。增益突变发生在帧边界时，
     * 上一帧末样本到本帧首样本的落差就是一个阶跃，频谱上展开成宽频瞬态。
     *
     * 为什么用【恒定满幅直流】而不是正弦当测试信号（关键）：
     *   正弦自身的相邻样本跳变（300Hz 满幅约 3534）会掩盖增益阶跃 ——
     *   若突变点恰好落在正弦过零处，增益砍半也测不出跳变（我第一版就栽在这）。
     *   直流的自然跳变恒为 0，于是输出里任何跳变都【纯粹来自增益变化】，
     *   有/无斜坡的对照最干净：斜坡把阶跃分摊到 duck_ms 内的每帧台阶，
     *   无斜坡则是一帧内从满幅直接砍到 0。 */
    printf("\n  --- 爆音对比：恒定满幅信号 + 增益突变，连续流最大相邻样本跳变 ---\n");
    const int N = 320;   /* 20ms 一帧 */
    const int FRAMES = 30;
    const my_pcm_t DC_LEVEL = 30000;   /* 恒定满幅信号 */
    my_pcm_t *stream = malloc(sizeof(my_pcm_t) * N * FRAMES);
    if (!stream) { printf("  [FAIL] malloc\n"); g_fail++; return; }

    for (int half = 0; half < 2; half++) {
        bool with_ramp = (half == 0);
        my_fade_cfg_t c2; my_fade_default_cfg(&c2);
        c2.fade_in_ms = 0; c2.hold_ms = 0;
        c2.target_q15 = MY_GAIN_FULL_Q15;     /* 满幅场景，让 DC 对照量纲干净 */
        c2.duck_ms = with_ramp ? 300 : 0;     /* 0 = 无斜坡，增益瞬间跳变 */
        c2.duck_gain_q15 = 0;                 /* 闪避到静音，最坏情况 */
        my_fade_t g2; my_fade_init(&g2, &c2);
        my_fade_start(&g2);

        long idx = 0;
        /* 先满幅播 5 帧（建立"正在响"的状态），第 6 帧触发闪避到静音 */
        for (int fr = 0; fr < FRAMES; fr++) {
            if (fr == 5) my_fade_set_duck(&g2, true);
            my_fade_tick(&g2, 20);
            int32_t gain = my_fade_gain_q15(&g2);
            for (int i = 0; i < N; i++) stream[idx + i] = DC_LEVEL;   /* 恒定直流 */
            my_fade_apply(&g2, stream + idx, N, gain);                /* 就地施加，拼成连续流 */
            idx += N;
        }

        /* 测连续流的最大相邻样本跳变（跨帧边界也算，这才是爆音所在） */
        int32_t max_jump = 0;
        for (long i = 1; i < idx; i++) {
            int32_t d = abs((int32_t)stream[i] - (int32_t)stream[i - 1]);
            if (d > max_jump) max_jump = d;
        }
        /* 直流自然跳变 = 0，故 max_jump 全部来自增益变化。
         * 有斜坡：每帧台阶 ≈ DC×(20ms/300ms) = DC×0.067 ≈ 2000。
         * 无斜坡：突变帧一帧内 DC→0，跳变 = DC = 30000。 */
        printf("  [INFO] %-8s：连续流最大相邻样本跳变 %6d（直流自然跳变 0，全部来自增益）\n",
               with_ramp ? "有斜坡" : "无斜坡", max_jump);
        if (with_ramp) {
            CHECK(max_jump < DC_LEVEL / 5,
                  "有斜坡：跳变 %d < 满幅的 20%%（阶跃被分摊到 %dms 斜坡，无爆音）",
                  max_jump, c2.duck_ms);
        } else {
            CHECK(max_jump > DC_LEVEL * 4 / 5,
                  "无斜坡：跳变 %d > 满幅的 80%%（一帧内砍断 = 爆音）", max_jump);
        }
    }
    printf("  [INFO] 对照结论：斜坡把满幅阶跃的帧间跳变压到 1/%d，\n"
           "         这正是 duck_ms/pause 必须走斜坡、不能瞬间改增益的原因。\n",
           300 / 20);
    free(stream);
}

/* ================= 8. 关键用例：闪避中触发淡出不得反弹 ================= */
static void test_duck_then_fadeout_monotonic(void)
{
    printf("\n=== 8. 闪避中触发淡出（音量不得反弹） ===\n");
    my_fade_cfg_t cfg; my_fade_default_cfg(&cfg);
    cfg.fade_in_ms = 0; cfg.hold_ms = 0; cfg.fade_out_ms = 60000;
    cfg.tail_ms = 3000; cfg.duck_ms = 300; cfg.duck_gain_q15 = 8192;
    my_fade_t f; my_fade_init(&f, &cfg);
    my_fade_start(&f);
    int32_t full = my_fade_gain_q15(&f);   /* = target 22938 */

    my_fade_set_duck(&f, true);                 /* 智能体开始说话 */
    for (int i = 0; i < 20; i++) my_fade_tick(&f, 20);   /* 闪避到位 */
    int32_t ducked = my_fade_gain_q15(&f);      /* = full×8192>>15 ≈ 5734 */
    CHECK(ducked < full, "已进入闪避（总增益 %d < 满幅 %d）", ducked, full);
    CHECK(ducked == (int32_t)((int64_t)full * cfg.duck_gain_q15 >> 15),
          "闪避后总增益 = 场景×duck = %d（实测 %d）",
          (int32_t)((int64_t)full * cfg.duck_gain_q15 >> 15), ducked);

    my_fade_start_fade_out(&f);                 /* 此时判定用户入睡，开始淡出 */
    /* 触发淡出后引擎已把 duck 乘子"烘焙"进曲线并解除闪避，
     * 再显式 set_duck(false) 应为幂等空操作，不得让音量回升。 */
    CHECK(f.duck_active == false, "烘焙后 duck_active 已解除");
    CHECK(f.duck_gain_q15 == MY_GAIN_FULL_Q15, "烘焙后 duck 乘子归中性 1.0");
    CHECK(my_fade_gain_q15(&f) == ducked,
          "烘焙瞬间总增益连续（%d，未跳变）", my_fade_gain_q15(&f));
    my_fade_set_duck(&f, false);                /* 智能体说完话（应无副作用） */

    /* 淡出全程：总增益必须单调不增，绝不能弹回满幅 */
    int32_t prev = ducked, violations = 0, peak = ducked;
    for (int t = 0; t < 60000; t += 20) {
        my_fade_tick(&f, 20);
        int32_t g = my_fade_gain_q15(&f);
        if (g > prev) violations++;
        if (g > peak) peak = g;
        prev = g;
    }
    CHECK(violations == 0, "闪避烘焙叠加淡出：总增益单调不增（违规 %d 次）", violations);
    CHECK(peak <= ducked, "峰值增益 %d 未超过触发淡出时的 %d（无音量反弹）", peak, ducked);
    CHECK(my_fade_gain_q15(&f) == 0, "最终归零");
    printf("  [INFO] 这是「淡出触发时把闪避/暂停乘子烘焙进曲线起点」要防的坑：\n"
           "         若不烘焙，释放闪避会把 duck 乘子从 %.1fdB 拉回 0dB，\n"
           "         总增益瞬间反弹 %d→%d（约 +12dB），夜里会惊醒用户。\n",
           (double)gain_db_of(cfg.duck_gain_q15), ducked, full);
}

/* ================= 9. 从极低音量触发淡出（线性段兜底） ================= */
static void test_fadeout_from_low_gain(void)
{
    printf("\n=== 9. 低音量起淡（线性段兜底，仍须在设定时长归零） ===\n");
    /* 场景：用户把音量调到很低(-50dB)后按"关闭"，此时已低于 floor_db(-60dB)？
     * 这里构造起点低于地板的情况：先淡出到 -55dB，再重新触发一次淡出。 */
    my_fade_cfg_t cfg; my_fade_default_cfg(&cfg);
    cfg.fade_in_ms = 0; cfg.fade_out_ms = 20000; cfg.tail_ms = 3000; cfg.floor_db = -40;
    my_fade_t f; my_fade_init(&f, &cfg);
    my_fade_start(&f);
    my_fade_start_fade_out(&f);

    /* 跑完第一次淡出（应到 0），然后人为设一个低场景增益再触发第二次 */
    for (int t = 0; t < 20000; t += 20) my_fade_tick(&f, 20);
    CHECK(my_fade_gain_q15(&f) == 0, "第一次淡出归零");

    /* 重新播放并停在低音量：target=200 Q15 ≈ -44dB，低于地板 floor_db=-40dB。
     * 这模拟"用户把音量调到很轻后按关闭"——此时无指数段可走，
     * 必须由线性段兜底，否则会出现"关了但还在响"。 */
    cfg.target_q15 = 200;   /* ≈ -44dB */
    cfg.floor_db   = -40;
    my_fade_init(&f, &cfg);
    my_fade_start(&f);
    CHECK(f.cfg.target_q15 == 200, "低音量目标已设置（%d ≈ %.1fdB）",
          200, gain_db_of(200));
    my_fade_start_fade_out(&f);
    CHECK(f.fo_exp_ms == 0, "起点低于地板 → 跳过指数段（fo_exp_ms=0），整段线性归零");
    CHECK(f.fo_lin_ms == cfg.fade_out_ms, "线性段接管全部时长 %dms", f.fo_lin_ms);

    int32_t prev = my_fade_gain_q15(&f), mono = 1, zero_ms = -1;
    for (int t = 0; t <= cfg.fade_out_ms; t += 20) {
        my_fade_tick(&f, 20);
        int32_t g = my_fade_gain_q15(&f);
        if (g > prev) mono = 0;
        prev = g;
        if (g == 0 && zero_ms < 0) zero_ms = t + 20;
    }
    CHECK(mono, "低音量起淡同样单调不增");
    CHECK(my_fade_gain_q15(&f) == 0, "低音量起淡仍精确归零（线性段兜底生效）");
    CHECK(zero_ms >= 0 && zero_ms <= cfg.fade_out_ms,
          "在 %dms 内归零（未超出设定时长）", zero_ms);
}

/* ================= 10. apply() 正确性：无削顶 + 静音 + 抖动 ================= */
static void test_apply(void)
{
    printf("\n=== 10. 增益施加 apply() ===\n");
    const int N = 1600;
    my_pcm_t *buf = malloc(sizeof(my_pcm_t) * N);
    my_pcm_t *ref = malloc(sizeof(my_pcm_t) * N);
    if (!buf || !ref) { printf("  [FAIL] malloc\n"); g_fail++; free(buf); free(ref); return; }

    my_fade_t f; my_fade_init(&f, NULL);

    /* 满幅方波 → 施加满增益必须原样通过（不能溢出削顶） */
    for (int i = 0; i < N; i++) { ref[i] = buf[i] = (i % 2) ? MY_PCM_MAX : MY_PCM_MIN; }
    my_fade_apply(&f, buf, N, MY_GAIN_FULL_Q15);
    int same = 1;
    for (int i = 0; i < N; i++) if (buf[i] != ref[i]) same = 0;
    CHECK(same, "满增益下方波原样通过（未削顶/未溢出）");

    /* 零增益必须写纯零（哄睡硬要求：淡出结束后不得有本底噪声） */
    for (int i = 0; i < N; i++) buf[i] = MY_PCM_MAX;
    my_fade_apply(&f, buf, N, 0);
    int all_zero = 1, max_abs = 0;
    for (int i = 0; i < N; i++) {
        if (buf[i] != 0) all_zero = 0;
        if (abs(buf[i]) > max_abs) max_abs = abs(buf[i]);
    }
    CHECK(all_zero, "零增益输出全零（最大幅值 %d，抖动也未引入残留）", max_abs);

    /* 半增益幅度约减半，且不削顶 */
    for (int i = 0; i < N; i++) buf[i] = (my_pcm_t)(20000 * sin(2 * M_PI * 440.0 * i / MY_SAMPLE_RATE));
    my_fade_apply(&f, buf, N, MY_GAIN_FULL_Q15 / 2);
    int peak = 0, clipped = 0;
    for (int i = 0; i < N; i++) {
        if (abs(buf[i]) > peak) peak = abs(buf[i]);
        if (abs(buf[i]) >= MY_PCM_MAX) clipped++;
    }
    CHECK(peak < 10100 && peak > 9900, "半增益峰值 %d ≈ 20000/2（Q15 定标正确）", peak);
    CHECK(clipped == 0, "半增益无削顶样本");

    /* 抖动幅度必须限制在 ±1 LSB 量级，不能引入可闻噪声 */
    my_fade_cfg_t cfg; my_fade_default_cfg(&cfg);
    cfg.dither_tpdf = true;
    my_fade_init(&f, &cfg);
    for (int i = 0; i < N; i++) buf[i] = 0;
    my_fade_apply(&f, buf, N, MY_GAIN_FULL_Q15 / 2);   /* 静音信号 + 抖动 */
    int dither_peak = 0;
    double sum = 0, sumsq = 0;
    for (int i = 0; i < N; i++) {
        if (abs(buf[i]) > dither_peak) dither_peak = abs(buf[i]);
        sum += buf[i]; sumsq += (double)buf[i] * buf[i];
    }
    CHECK(dither_peak <= 1, "TPDF 抖动峰值 %d LSB（<= 1，符合三角抖动设计）", dither_peak);
    printf("  [INFO] 抖动本底：均值 %.4f LSB，RMS %.4f LSB（约 %.1fdBFS，远低于卧室噪声门限）\n",
           sum / N, sqrt(sumsq / N), 20 * log10(sqrt(sumsq / N) / 32768.0 + 1e-12));
    CHECK(fabs(sum / N) < 0.5, "抖动无直流偏置（均值 %.4f）", sum / N);

    /* 关闭抖动：静音信号必须输出全零（可对比抖动效果） */
    cfg.dither_tpdf = false;
    my_fade_init(&f, &cfg);
    for (int i = 0; i < N; i++) buf[i] = 0;
    my_fade_apply(&f, buf, N, MY_GAIN_FULL_Q15 / 2);
    int nz = 0;
    for (int i = 0; i < N; i++) if (buf[i] != 0) nz++;
    CHECK(nz == 0, "关闭抖动时静音信号输出全零（抖动开关生效）");

    /* 参数校验 */
    CHECK(my_fade_apply(NULL, buf, N, 1000) == MY_ERR_PARAM, "apply(f=NULL) 拒绝");
    CHECK(my_fade_apply(&f, NULL, N, 1000) == MY_ERR_PARAM, "apply(buf=NULL) 拒绝");
    CHECK(my_fade_apply(&f, buf, -1, 1000) == MY_ERR_PARAM, "apply(count<0) 拒绝");
    CHECK(my_fade_apply_current(NULL, buf, N) == MY_ERR_PARAM, "apply_current(NULL) 拒绝");

    /* 超范围增益钳位（真机上增益可能被外部算错） */
    for (int i = 0; i < N; i++) buf[i] = MY_PCM_MAX;
    my_fade_apply(&f, buf, N, MY_GAIN_FULL_Q15 * 4);
    int over_peak = 0, over_min = 0;
    for (int i = 0; i < N; i++) {
        if (buf[i] > over_peak) over_peak = buf[i];
        if (buf[i] < over_min)  over_min  = buf[i];
    }
    CHECK(over_peak <= MY_PCM_MAX && over_min >= MY_PCM_MIN,
          "超范围增益(4×满幅)被钳位：输出范围 [%d,%d] 未溢出", over_min, over_peak);
    CHECK(over_peak == MY_PCM_MAX, "钳位到满幅后仍保持 %d（不是被截断成 0）", over_peak);

    free(buf); free(ref);
}

/* ================= 11. 长时运行与 tick 粒度无关性 ================= */
static void test_tick_granularity(void)
{
    printf("\n=== 11. tick 粒度无关性（不同音频帧长结果一致） ===\n");
    /* 真机上音频回调周期可能是 20ms/40ms/甚至抖动。淡出曲线只依赖
     * 累计时间，不应因喂入粒度不同而产生差异。 */
    const int GRAN[] = { 5, 10, 20, 40, 100, 250 };
    double end_db[6];

    for (unsigned gi = 0; gi < sizeof(GRAN) / sizeof(GRAN[0]); gi++) {
        my_fade_cfg_t cfg; my_fade_default_cfg(&cfg);
        cfg.fade_in_ms = 0; cfg.fade_out_ms = 60000; cfg.tail_ms = 3000;
        my_fade_t f; my_fade_init(&f, &cfg);
        my_fade_start(&f);
        my_fade_start_fade_out(&f);
        /* 跑到 30s（淡出中点） */
        int total = 0;
        while (total < 30000) { my_fade_tick(&f, GRAN[gi]); total += GRAN[gi]; }
        end_db[gi] = my_fade_gain_db(&f);
        printf("  [INFO] tick=%3dms → 30s 处增益 %.4fdB (%d Q15)\n",
               GRAN[gi], end_db[gi], my_fade_gain_q15(&f));
    }
    double spread = 0;
    for (unsigned i = 1; i < sizeof(GRAN) / sizeof(GRAN[0]); i++) {
        double d = fabs(end_db[i] - end_db[0]);
        if (d > spread) spread = d;
    }
    /* 允许最多一个 tick 的时间偏差（250ms 粒度 → 最坏 250ms/60000ms*60dB=0.25dB） */
    CHECK(spread < 0.30, "不同 tick 粒度最大差异 %.4fdB < 0.30dB（曲线与粒度解耦）", spread);
}

/* ================= 12. 整夜长时运行不溢出 ================= */
static void test_long_run_overflow(void)
{
    printf("\n=== 12. 整夜长时运行（溢出防护） ===\n");
    my_fade_cfg_t cfg; my_fade_default_cfg(&cfg);
    cfg.fade_in_ms = 0; cfg.hold_ms = 0;   /* 无限保持 */
    my_fade_t f; my_fade_init(&f, &cfg);
    my_fade_start(&f);

    /* 模拟设备忘关：HOLD 状态连续跑 10 天 */
    for (int i = 0; i < 10 * 24 * 3600; i++) my_fade_tick(&f, 1000);
    CHECK(my_fade_get_phase(&f) == MY_FADE_HOLD, "10 天后仍在 HOLD（scene_ms 已钳位）");
    CHECK(f.scene_ms <= 86400000, "scene_ms 钳在 1 天内（实测 %d），未溢出为负", f.scene_ms);
    CHECK(f.scene_ms >= 0, "scene_ms 非负");
    CHECK(my_fade_gain_q15(&f) == cfg.target_q15, "增益仍正确（钳位不影响行为）");
    CHECK(my_fade_played_ms(&f) > 0, "played_ms 正常累积（int64，%lldms）",
          (long long)my_fade_played_ms(&f));

    /* 闪避/暂停计时器长期开启也不溢出 */
    my_fade_set_duck(&f, true);
    my_fade_pause(&f);
    for (int i = 0; i < 10 * 24 * 3600; i++) my_fade_tick(&f, 1000);
    CHECK(f.duck_ms_elapsed <= cfg.duck_ms, "duck 计时钳位（%d <= %d）",
          f.duck_ms_elapsed, cfg.duck_ms);
    CHECK(f.pause_ms_elapsed <= cfg.duck_ms, "pause 计时钳位（%d <= %d）",
          f.pause_ms_elapsed, cfg.duck_ms);
    CHECK(my_fade_gain_q15(&f) == 0, "暂停 10 天后仍静音（无回绕跳变）");

    my_fade_resume(&f);
    my_fade_set_duck(&f, false);
    my_fade_tick(&f, 400);
    CHECK(my_fade_gain_q15(&f) == cfg.target_q15, "恢复后增益正确回到目标");
}

int main(void)
{
    printf("============================================\n");
    printf(" 安眠科技 · 音量淡出曲线引擎单元测试\n");
    printf(" Q15 满幅 %d / 默认淡出 %dms(15min) / 末段 %dms\n",
           MY_GAIN_FULL_Q15, 900000, 3000);
    printf("============================================\n");

    test_api_and_cfg();
    test_fade_in();
    test_fade_out_core();
    test_db_linearity();
    test_resolution();
    test_pause_resume();
    test_duck_and_click();
    test_duck_then_fadeout_monotonic();
    test_fadeout_from_low_gain();
    test_apply();
    test_tick_granularity();
    test_long_run_overflow();

    printf("\n============================================\n");
    printf(" 结果： %d 通过 / %d 失败\n", g_pass, g_fail);
    printf("MY_RESULT: pass=%d fail=%d\n", g_pass, g_fail);
    printf("============================================\n");
    return g_fail ? 1 : 0;
}
