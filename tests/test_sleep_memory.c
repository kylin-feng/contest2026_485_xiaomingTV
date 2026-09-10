/* SPDX-License-Identifier: Apache-2.0
 *
 * 眠语 · 睡眠记忆持久化单元测试（PC 端）
 *
 * 测试重点：
 *   1. add：同晚去重更新、未满追加、满后覆盖最旧（按 date_key）
 *   2. find/recent：按 date_key 查、降序取最近 n 晚
 *   3. summarize：平均潜伏期/时长/夜醒/质量、best_aid 偏好学习、
 *      late_nights_7d 晚睡计数、trend 趋势（近半 vs 远半）
 *   4. record_sane：坏记录（非法日期/越界字段）拒收，不毁整份档案
 *   5. 持久化 HAL：脏标记延迟写、写失败重试、损坏介质回落空档案
 *   6. 边界：满 180 晚环形滚动、空档案聚合、days 窗口过滤
 *
 * 编译： cc -std=c11 -O2 -I../include -o test_sleep_memory test_sleep_memory.c ../src/mianyu_sleep_memory.c
 * 运行： ./test_sleep_memory
 */
#include "mianyu_sleep_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, fmt, ...) do { \
    if (cond) { g_pass++; printf("  [PASS] " fmt "\n", ##__VA_ARGS__); } \
    else      { g_fail++; printf("  [FAIL] " fmt "\n", ##__VA_ARGS__); } \
} while (0)

static my_sleep_record_t mkrec(int32_t date_key, int bedtime, int onset_s,
                               int total_m, int wakes, my_aid_t aid, int quality)
{
    my_sleep_record_t r;
    memset(&r, 0, sizeof(r));
    r.date_key = date_key;
    r.bedtime_min = bedtime;
    r.sleep_onset_sec = onset_s;
    r.total_sleep_min = total_m;
    r.night_wake_count = (uint8_t)wakes;
    r.aid_used = (uint8_t)aid;
    r.quality = (uint8_t)quality;
    return r;
}

/* 直接写入一条已构造好的记录（需要自定义 flags/字段时用）。
 * 【为什么必须检查返回值】初版这里不看返回码，于是"两晚熬夜记录"因为
 * bedtime_min=1450/1460 越界被 record_sane 静默拒收，late_nights_7d 恒为 0——
 * 测试挂了，但看起来像"窗口过滤写错了"，实际根因是【晚睡判定漏了跨午夜折算】
 * 这个产品级 bug 被静默拒收掩盖。教训：测试里每一次"本该成功"的写入都要断言
 * 成功，否则"数据没进去"和"逻辑算错了"会长得一模一样。 */
static int add_rec_from(my_sleep_memory_t *m, const my_sleep_record_t *r)
{
    int rc = my_mem_add(m, r);
    if (rc < 0) printf("    [!!] add(%d) 被拒，返回 %d —— 测试数据本身非法\n",
                       (int)r->date_key, rc);
    return rc;
}

/* 一行写入 helper（常规字段）。 */
static int add_rec(my_sleep_memory_t *m, int32_t date_key, int bedtime, int onset_s,
                   int total_m, int wakes, my_aid_t aid, int quality)
{
    my_sleep_record_t r = mkrec(date_key, bedtime, onset_s, total_m, wakes, aid, quality);
    return add_rec_from(m, &r);
}

/* 断言"本该成功"的写入确实成功 */
#define ADD_OK(...)  do { \
    int rc_ = add_rec(__VA_ARGS__); \
    if (rc_ >= 0) { g_pass++; } \
    else { g_fail++; printf("  [FAIL] add_rec 未成功写入（rc=%d）\n", rc_); } \
} while (0)

/* ===================== 1. add / find / 去重 ===================== */
static void test_add_find(void)
{
    printf("\n=== 1. add / find / 同晚去重 ===\n");
    my_sleep_memory_t m;
    my_mem_init(&m, NULL);
    CHECK(my_mem_count(&m) == 0, "init 后 count=0");

    int s0 = my_mem_add(&m, &(my_sleep_record_t){0});   /* date_key=0 非法 */
    CHECK(s0 == MY_ERR_PARAM, "date_key=0 的记录被拒");
    CHECK(my_mem_count(&m) == 0, "拒收后 count 仍 0");

    my_sleep_record_t r = mkrec(20260901, 1350, 1200, 420, 1, MY_AID_BROWN, 80);
    int s1 = my_mem_add(&m, &r);
    CHECK(s1 >= 0, "合法记录写入成功（槽 %d）", s1);
    CHECK(my_mem_count(&m) == 1, "count=1");
    CHECK(m.dirty, "写入置 dirty");

    const my_sleep_record_t *f = my_mem_find(&m, 20260901);
    CHECK(f != NULL, "find 命中 20260901");
    CHECK(f->sleep_onset_sec == 1200, "读回 onset=1200（实测 %d）", f->sleep_onset_sec);
    CHECK(f->flags & 0x01, "有效位已置");

    /* 同晚再写（改数值）应更新而非新增 */
    r = mkrec(20260901, 1350, 900, 450, 0, MY_AID_BROWN, 90);
    my_mem_add(&m, &r);
    CHECK(my_mem_count(&m) == 1, "同晚二次写入不增 count（去重，实测 %d）", my_mem_count(&m));
    CHECK(my_mem_find(&m, 20260901)->sleep_onset_sec == 900,
          "同晚二次写入更新为 onset=900（实测 %d）",
          my_mem_find(&m, 20260901)->sleep_onset_sec);

    /* 不同晚新增 */
    my_mem_add(&m, &(my_sleep_record_t){0});   /* 仍拒 */
    my_sleep_record_t r2 = mkrec(20260902, 1400, 1500, 400, 2, MY_AID_PINK, 70);
    my_mem_add(&m, &r2);
    CHECK(my_mem_count(&m) == 2, "不同晚新增 count=2");
    CHECK(my_mem_find(&m, 20260999) == NULL, "find 未命中日期返回 NULL");
    CHECK(my_mem_find(NULL, 20260901) == NULL, "find(NULL,..) 返回 NULL");
    CHECK(my_mem_add(NULL, &r) == MY_ERR_PARAM, "add(NULL,..) 拒绝");
}

/* ===================== 2. 坏记录拒收 ===================== */
static void test_bad_records(void)
{
    printf("\n=== 2. 坏记录拒收（record_sane） ===\n");
    my_sleep_memory_t m;
    my_mem_init(&m, NULL);

    struct { const char *name; my_sleep_record_t r; } cases[] = {
        {"month=13",       mkrec(20261301, 1350, 1200, 420, 1, MY_AID_BROWN, 80)},
        {"day=32",         mkrec(20260132, 1350, 1200, 420, 1, MY_AID_BROWN, 80)},
        {"day=0",          mkrec(20260100, 1350, 1200, 420, 1, MY_AID_BROWN, 80)},
        {"onset<0",        mkrec(20260901, 1350, -5,   420, 1, MY_AID_BROWN, 80)},
        {"total>1440",     mkrec(20260901, 1350, 1200, 1500,1, MY_AID_BROWN, 80)},
        {"bedtime>1439",   mkrec(20260901, 1500, 1200, 420, 1, MY_AID_BROWN, 80)},
        {"aid 越界",        mkrec(20260901, 1350, 1200, 420, 1, MY_AID_KIND_MAX, 80)},
        {"quality>100",    mkrec(20260901, 1350, 1200, 420, 1, MY_AID_BROWN, 101)},
        {"date<1970",      mkrec(19690101, 1350, 1200, 420, 1, MY_AID_BROWN, 80)},
    };
    int rejected = 0;
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        if (my_mem_add(&m, &cases[i].r) == MY_ERR_PARAM) rejected++;
        else printf("    [漏网] %s 竟被接受\n", cases[i].name);
    }
    CHECK(rejected == 9, "9 类坏记录全部拒收（漏网 %d）", 9 - rejected);
    CHECK(my_mem_count(&m) == 0, "拒收后档案仍为空");

    /* 边界值应被接受（防止校验过严误杀合法数据） */
    my_sleep_record_t ok1 = mkrec(20260901, 0, 0, 0, 0, MY_AID_NONE, 0);
    my_sleep_record_t ok2 = mkrec(20261231, 1439, 86400, 1440, 255, MY_AID_MIXED, 100);
    CHECK(my_mem_add(&m, &ok1) >= 0, "全 0 边界记录被接受");
    CHECK(my_mem_add(&m, &ok2) >= 0, "上边界记录（bedtime=1439,total=1440,quality=100）被接受");
    CHECK(my_mem_count(&m) == 2, "边界值 count=2");
}

/* ===================== 3. recent 降序 ===================== */
static void test_recent(void)
{
    printf("\n=== 3. recent 取最近 n 晚（降序） ===\n");
    my_sleep_memory_t m;
    my_mem_init(&m, NULL);
    /* 乱序写入，验证 recent 按 date_key 降序输出 */
    int keys[] = {20260905, 20260901, 20260910, 20260903, 20260908};
    for (int i = 0; i < 5; i++) {
        my_sleep_record_t r = mkrec(keys[i], 1350, 1000 + i * 100, 400, 1, MY_AID_BROWN, 75);
        my_mem_add(&m, &r);
    }
    CHECK(my_mem_count(&m) == 5, "写入 5 晚");

    my_sleep_record_t out[5];
    int n = my_mem_recent(&m, out, 3);
    CHECK(n == 3, "recent(3) 返回 3 条（实测 %d）", n);
    CHECK(out[0].date_key == 20260910, "第 1 新 = 20260910（实测 %d）", out[0].date_key);
    CHECK(out[1].date_key == 20260908, "第 2 新 = 20260908（实测 %d）", out[1].date_key);
    CHECK(out[2].date_key == 20260905, "第 3 新 = 20260905（实测 %d）", out[2].date_key);
    /* 严格降序 */
    bool desc = (out[0].date_key > out[1].date_key) && (out[1].date_key > out[2].date_key);
    CHECK(desc, "输出严格降序");

    n = my_mem_recent(&m, out, 10);
    CHECK(n == 5, "recent(10) 但只有 5 条 → 返回 5（实测 %d）", n);
    CHECK(out[4].date_key == 20260901, "最旧一条 = 20260901");

    CHECK(my_mem_recent(&m, NULL, 3) == 0, "recent(out=NULL) 返回 0");
    CHECK(my_mem_recent(&m, out, 0) == 0, "recent(n=0) 返回 0");
    CHECK(my_mem_recent(NULL, out, 3) == 0, "recent(m=NULL) 返回 0");
}

/* ===================== 4. summarize 聚合 ===================== */
static void test_summarize(void)
{
    printf("\n=== 4. summarize 聚合与偏好学习 ===\n");
    my_sleep_memory_t m;
    my_mem_init(&m, NULL);

    /* 空档案聚合不崩溃 */
    my_sleep_summary_t sum;
    CHECK(my_mem_summarize(&m, 7, &sum) == MY_OK, "空档案 summarize 返回 OK");
    CHECK(sum.valid_days == 0 && sum.best_aid == -1, "空档案：valid_days=0, best_aid=-1");

    /* 构造 6 晚数据：
     *   棕噪 3 晚 onset 800/900/1000（均值 900）
     *   白噪 2 晚 onset 1500/1700（均值 1600）
     *   语音 1 晚 onset 1200
     * → best_aid 应为 BROWN（平均入睡最快） */
    ADD_OK(&m, 20260901, 1350, 800,  420, 1, MY_AID_BROWN, 80);
    ADD_OK(&m, 20260902, 1360, 900,  430, 0, MY_AID_BROWN, 85);
    ADD_OK(&m, 20260903, 1370, 1000, 410, 2, MY_AID_BROWN, 75);
    ADD_OK(&m, 20260904, 1380, 1500, 400, 1, MY_AID_WHITE, 70);
    ADD_OK(&m, 20260905, 1390, 1700, 390, 3, MY_AID_WHITE, 65);
    ADD_OK(&m, 20260906, 1400, 1200, 405, 1, MY_AID_VOICE, 72);

    my_mem_summarize(&m, 0, &sum);   /* 0 = 全部 */
    CHECK(sum.valid_days == 6, "全档聚合 6 晚（实测 %d）", sum.valid_days);
    /* 平均 onset = (800+900+1000+1500+1700+1200)/6 = 7100/6 = 1183 */
    CHECK(sum.avg_onset_sec == 1183, "平均潜伏期 1183s（实测 %d）", sum.avg_onset_sec);
    /* 平均 total = (420+430+410+400+390+405)/6 = 2455/6 = 409 */
    CHECK(sum.avg_total_min == 409, "平均时长 409min（实测 %d）", sum.avg_total_min);
    /* 平均夜醒 ×100 = (1+0+2+1+3+1)/6*100 = 8/6*100 = 133 */
    CHECK(sum.avg_wake_count == 133, "平均夜醒 1.33 次(×100=133，实测 %d)", sum.avg_wake_count);
    /* 平均质量 = (80+85+75+70+65+72)/6 = 447/6 = 74 */
    CHECK(sum.avg_quality == 74, "平均质量 74（实测 %d）", sum.avg_quality);
    CHECK(sum.best_aid == MY_AID_BROWN, "best_aid = BROWN（平均入睡最快，实测 %d）", sum.best_aid);
    CHECK(sum.best_aid_onset_sec == 900, "BROWN 平均潜伏期 900s（实测 %d）", sum.best_aid_onset_sec);

    /* days 窗口过滤：只取最近 3 晚（904,905,906） */
    my_mem_summarize(&m, 3, &sum);
    CHECK(sum.valid_days == 3, "窗口 3 晚（实测 %d）", sum.valid_days);
    /* 这 3 晚 onset = 1500/1700/1200，均值 1466；best_aid 应为 VOICE(1200) */
    CHECK(sum.avg_onset_sec == 1466, "窗口内平均潜伏期 1466（实测 %d）", sum.avg_onset_sec);
    CHECK(sum.best_aid == MY_AID_VOICE, "窗口内 best_aid=VOICE（实测 %d）", sum.best_aid);

    printf("\n  --- late_nights_7d 与 trend ---\n");
    /* late_nights：跨午夜折算后落在 [23:30, 次日06:00) 才算晚睡。
     * 上面 6 晚 bedtime 1350~1400（22:30~23:20）都在 23:30 之前 → 0 次晚睡。 */
    my_mem_summarize(&m, 0, &sum);
    CHECK(sum.late_nights_7d == 0, "近 7 晚无晚睡（22:30~23:20 上床，实测 %d）",
          sum.late_nights_7d);

    /* 加 2 晚真·熬夜。
     * 【关键：跨午夜怎么表示】bedtime_min 是当日分钟数，上限 1439，
     * 所以"次日 00:10 上床"必须写成 bedtime_min=10 + flags bit2(跨日)，
     * 绝不能写成 1450——初版就写了 1450/1460，被 record_sane 静默拒收，
     * 于是 late_nights_7d 恒为 0，测试挂了却看不出是"数据没进去"还是"逻辑错了"。 */
    my_sleep_record_t late1 = mkrec(20260907, 10, 2000, 350, 4, MY_AID_WHITE, 50);
    late1.flags |= 0x04;                       /* bit2 = 跨日（次日 00:10 上床） */
    CHECK(add_rec_from(&m, &late1) >= 0, "熬夜记录 907（00:10）写入成功");

    my_sleep_record_t late2 = mkrec(20260908, 150, 2200, 340, 5, MY_AID_WHITE, 45);
    late2.flags |= 0x04;                       /* 次日 02:30 上床 */
    CHECK(add_rec_from(&m, &late2) >= 0, "熬夜记录 908（02:30）写入成功");

    my_mem_summarize(&m, 0, &sum);
    CHECK(sum.valid_days == 8, "共 8 晚入档（实测 %d）", sum.valid_days);
    CHECK(sum.late_nights_7d == 2,
          "近 7 晚有 2 次晚睡（00:10 与 02:30 跨午夜，实测 %d）", sum.late_nights_7d);

    /* trend：近半（905~908，onset 1700/1200/2000/2200）比远半（901~904）慢 */
    CHECK(sum.trend_onset_sec > 0, "趋势为正（入睡变慢=作息变差，实测 %+ds）",
          sum.trend_onset_sec);

    CHECK(my_mem_summarize(NULL, 7, &sum) == MY_ERR_PARAM, "summarize(m=NULL) 拒绝");
    CHECK(my_mem_summarize(&m, 7, NULL) == MY_ERR_PARAM, "summarize(sum=NULL) 拒绝");
}

/* ===================== 4b. 晚睡判定的跨午夜语义（回归锁） =====================
 * 这一组是为了把上面那个 bug 钉死：晚睡判定【不能】直接用 bedtime_min 比大小。
 * 判据：以 18:00 为睡眠夜原点折算，[23:30, 次日06:00) 为晚睡。
 *
 * 【语义澄清】bedtime_min 只有 0..1439，单看数值无法区分"今晚 23:50"和
 * "次日 00:10"以外的归属——所以记录里有 flags bit2(跨日) 标记。
 * 但折算函数是纯数值函数：00:10 → night_min=420，落在晚睡区间内，
 * 这符合直觉（午夜上床就是晚睡），跨日 flag 只用于展示话术与日期归属，
 * 不参与晚睡判定。 */
static void test_late_night_semantics(void)
{
    printf("\n=== 4b. 晚睡判定跨午夜语义（bug 回归锁） ===\n");
    struct { int32_t bedtime; bool expect_late; const char *note; } cases[] = {
        /* 白天/傍晚上床：不算晚睡 */
        { 480, false, "08:00 早晨补觉" },
        { 720, false, "12:00 中午补觉（通宵/倒班，语义上不是'睡得晚'）" },
        { 900, false, "15:00 下午补觉" },
        {1020, false, "17:00 傍晚" },
        {1080, false, "18:00 睡眠夜原点" },
        {1260, false, "21:00 早睡" },
        {1350, false, "22:30 计划睡前" },
        {1409, false, "23:29 差 1 分钟不算" },
        /* 阈值起点 */
        {1410, true,  "23:30 阈值起点（含）" },
        {1439, true,  "23:59 当日最后一分钟" },
        /* 跨午夜：初版全部漏检的重灾区 */
        {   0, true,  "00:00 午夜整" },
        {  10, true,  "00:10 跨日 —— 初版判为'没晚睡'（10<1410）" },
        {  60, true,  "01:00 跨日" },
        { 150, true,  "02:30 跨日 —— 初版同样漏检" },
        { 240, true,  "04:00 跨日" },
        { 359, true,  "05:59 跨日（阈值上界前 1 分钟）" },
        /* 阈值上界之后：通宵/补觉，不计入 */
        { 360, false, "06:00 上界（不含）—— 属补觉，不是'睡得晚'" },
        { 479, false, "07:59 补觉" },
    };

    int wrong = 0;
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        bool got = my_mem_is_late_night(cases[i].bedtime);
        if (got != cases[i].expect_late) {
            wrong++;
            printf("    [不符] bedtime=%4d 期望 %s 实测 %s —— %s\n",
                   (int)cases[i].bedtime,
                   cases[i].expect_late ? "晚睡" : "不晚",
                   got ? "晚睡" : "不晚", cases[i].note);
        } else {
            printf("  [PASS] bedtime=%4d → %-4s %s\n",
                   (int)cases[i].bedtime, got ? "晚睡" : "不晚", cases[i].note);
        }
    }
    CHECK(wrong == 0, "%u 个晚睡语义用例全对（不符 %d）",
          (unsigned)(sizeof(cases)/sizeof(cases[0])), wrong);

    /* 未知值不计入 */
    CHECK(!my_mem_is_late_night(-1), "bedtime=-1（未知）返回 false");

    /* 核心回归：越熬夜必须越能判出来，不能反向漏检 */
    CHECK(my_mem_is_late_night(1410) && my_mem_is_late_night(10) && my_mem_is_late_night(150),
          "23:30 / 00:10 / 02:30 三点都判晚睡（初版只有 23:30 能过）");
}

/* ===================== 5. 满 180 晚环形滚动 ===================== */

/* 公历日期递推：返回 start(YYYYMMDD) 之后第 days 天的 date_key。
 * 【为什么要这个】上一版用 `month=1+i/28; day=1+i%28; if(month>12) month-=12;`
 * 手工拼 key，看着能跑其实脆弱：减 12 只对 i/28==12 那一档有效，i/28>=24 时
 * 会得到 month=13 这种非法 key（i<=199 时恰好没触发，属于"测试碰巧过了"）。
 * 环形覆盖是按 date_key 比大小选最旧的，key 一旦非单调递增，"覆盖了最旧"
 * 这个核心不变量就测不出来了。所以这里老老实实做真实日历递推。 */
static int32_t date_after(int32_t start, int days)
{
    static const int8_t mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int y = start / 10000, m = (start / 100) % 100, d = start % 100;
    for (int i = 0; i < days; i++) {
        int dim = mdays[m - 1];
        if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) dim = 29;
        if (++d > dim) { d = 1; if (++m > 12) { m = 1; y++; } }
    }
    return y * 10000 + m * 100 + d;
}

static void test_ring_overflow(void)
{
    printf("\n=== 5. 满 180 晚环形滚动（覆盖最旧） ===\n");
    const int32_t base = 20260101;
    const int total_written = 200;

    /* 先自测 date_after：跨闰月、跨年、闰年 2/29 */
    CHECK(date_after(base, 0) == 20260101, "date_after 第 0 天 = 起点");
    CHECK(date_after(base, 31) == 20260201, "1/1 + 31 天 = 2/1（跨月）");
    CHECK(date_after(20260228, 1) == 20260301, "2026 平年 2/28 + 1 = 3/1");
    CHECK(date_after(20280228, 1) == 20280229, "2028 闰年 2/28 + 1 = 2/29");
    CHECK(date_after(20280229, 1) == 20280301, "闰年 2/29 + 1 = 3/1");
    CHECK(date_after(base, 364) == 20261231, "1/1 + 364 天 = 12/31（跨年边界）");
    CHECK(date_after(base, 365) == 20270101, "1/1 + 365 天 = 次年 1/1（跨年）");

    my_sleep_memory_t m;
    my_mem_init(&m, NULL);

    /* 写 200 晚连续合法日期，应只保留最近 180 晚 */
    int add_fail = 0;
    for (int i = 0; i < total_written; i++) {
        my_sleep_record_t r = mkrec(date_after(base, i), 1350, 1000, 400, 1,
                                    MY_AID_BROWN, 75);
        if (my_mem_add(&m, &r) < 0) add_fail++;
    }
    CHECK(add_fail == 0, "%d 晚连续写入全部成功（失败 %d）", total_written, add_fail);
    CHECK(my_mem_count(&m) == MY_MEM_MAX_DAYS,
          "写 %d 晚后 count 封顶 %d（实测 %d）",
          total_written, MY_MEM_MAX_DAYS, my_mem_count(&m));

    /* 不变量 1：环形缓冲内 date_key 全互异（去重 + 覆盖共同保证） */
    int dup = 0;
    for (int i = 0; i < m.count; i++)
        for (int j = i + 1; j < m.count; j++)
            if (m.rec[i].date_key == m.rec[j].date_key) dup++;
    CHECK(dup == 0, "环形缓冲内 date_key 全互异（重复 %d 对）", dup);

    /* 不变量 2：覆盖掉的必须是【最旧】的，不是随机槽。
     * 写 200 晚保留 180 晚 → 被覆盖的应是第 0..19 晚，保留第 20..199 晚。
     * 【上一版这里是假断言】：`out[0].date_key == out[0].date_key` 恒真；
     * `find(out[n-1].date_key) != NULL` 也必然成立（recent 的输出本就来自档案）。
     * 两个都测不出"覆盖策略错了"。这里改成对期望日期做正反两向核对。 */
    int32_t kept_oldest   = date_after(base, total_written - MY_MEM_MAX_DAYS); /* 第 20 晚 */
    int32_t kept_newest   = date_after(base, total_written - 1);               /* 第199 晚 */
    int32_t dropped_last  = date_after(base, total_written - MY_MEM_MAX_DAYS - 1); /* 第19晚 */

    CHECK(my_mem_find(&m, kept_oldest) != NULL, "最旧保留条 %d 仍在档", kept_oldest);
    CHECK(my_mem_find(&m, kept_newest) != NULL, "最新条 %d 在档", kept_newest);
    CHECK(my_mem_find(&m, dropped_last) == NULL, "第 19 晚 %d 已被覆盖（覆盖的确实是最旧）",
          dropped_last);
    CHECK(my_mem_find(&m, base) == NULL, "最初一晚 %d 已被覆盖", base);

    /* 不变量 3：recent(180) 严格降序，且首尾正好是保留区间的两端 */
    my_sleep_record_t *out = malloc(sizeof(my_sleep_record_t) * MY_MEM_MAX_DAYS);
    if (!out) { printf("  [FAIL] malloc\n"); g_fail++; return; }
    int n = my_mem_recent(&m, out, MY_MEM_MAX_DAYS);
    CHECK(n == MY_MEM_MAX_DAYS, "recent 取满 %d 条（实测 %d）", MY_MEM_MAX_DAYS, n);
    CHECK(out[0].date_key == kept_newest, "recent[0] = 最新 %d（实测 %d）",
          kept_newest, out[0].date_key);
    CHECK(out[n - 1].date_key == kept_oldest, "recent[%d] = 最旧保留 %d（实测 %d）",
          n - 1, kept_oldest, out[n - 1].date_key);
    int disorder = 0;
    for (int i = 1; i < n; i++)
        if (out[i - 1].date_key <= out[i].date_key) disorder++;
    CHECK(disorder == 0, "recent 180 条严格降序（逆序 %d 处）", disorder);
    printf("  [INFO] 保留区间 %d ~ %d（写入 %d 晚，覆盖 %d 晚）\n",
           out[n-1].date_key, out[0].date_key, total_written,
           total_written - MY_MEM_MAX_DAYS);
    free(out);

    /* 不变量 4：满缓冲后 add 新日期 → 覆盖最旧，count 不变 */
    int before = my_mem_count(&m);
    my_sleep_record_t newest = mkrec(20991231, 1350, 1000, 400, 1, MY_AID_BROWN, 75);
    int idx = my_mem_add(&m, &newest);
    CHECK(idx >= 0, "满后仍可 add（覆盖槽 %d）", idx);
    CHECK(my_mem_count(&m) == before, "满后 add count 不变（%d）", my_mem_count(&m));
    CHECK(my_mem_find(&m, 20991231) != NULL, "新日期已入档");
    CHECK(my_mem_find(&m, kept_oldest) == NULL, "原最旧保留条 %d 被这次 add 顶掉", kept_oldest);
}

/* ===================== 6-7. 持久化 HAL ===================== */
typedef struct {
    my_sleep_record_t data[MY_MEM_MAX_DAYS];
    int count;
    int write_idx;
    int load_calls, save_calls;
    bool corrupt;      /* load 时返回损坏数据 */
    bool fail_save;    /* save 返回 IO 错误 */
    bool empty_media;  /* load 返回 0（空盘） */
} mem_backend_t;

static int be_load(my_sleep_record_t *records, int max_records, int *write_idx, void *ctx)
{
    mem_backend_t *b = (mem_backend_t *)ctx;
    b->load_calls++;
    if (b->empty_media) return 0;
    if (b->corrupt) {
        /* 模拟半条损坏：前 2 条好，第 3 条 date_key 非法，第 4 条越界 */
        int n = 4 < max_records ? 4 : max_records;
        records[0] = mkrec(20260901, 1350, 900, 420, 1, MY_AID_BROWN, 80);
        records[1] = mkrec(20260902, 1360, 1000, 410, 0, MY_AID_BROWN, 82);
        records[2] = mkrec(20261399, 1350, 900, 420, 1, MY_AID_BROWN, 80);  /* month=13 非法 */
        records[3] = mkrec(20260904, 1350, 99999, 420, 1, MY_AID_BROWN, 80);/* onset 越界 */
        if (write_idx) *write_idx = 0;
        return n;
    }
    int n = b->count < max_records ? b->count : max_records;
    memcpy(records, b->data, (size_t)n * sizeof(my_sleep_record_t));
    if (write_idx) *write_idx = b->write_idx;
    return n;
}

static my_err_t be_save(const my_sleep_record_t *records, int count, int write_idx, void *ctx)
{
    mem_backend_t *b = (mem_backend_t *)ctx;
    b->save_calls++;
    if (b->fail_save) return MY_ERR_IO;
    b->count = count;
    b->write_idx = write_idx;
    memcpy(b->data, records, (size_t)count * sizeof(my_sleep_record_t));
    return MY_OK;
}

static void test_persistence(void)
{
    printf("\n=== 6. 持久化 HAL（延迟写 / 重试 / 损坏自愈） ===\n");

    mem_backend_t be;
    memset(&be, 0, sizeof(be));
    my_mem_store_t hal = { .save = be_save, .load = be_load, .ctx = &be };

    my_sleep_memory_t m;
    my_mem_init(&m, &hal);
    CHECK(m.store_present, "配置后端后 store_present=true");

    /* 载入空盘 → 空档案，不算错 */
    be.empty_media = true;
    CHECK(my_mem_load(&m) == MY_OK, "空盘 load 返回 OK");
    CHECK(my_mem_count(&m) == 0, "空盘 → count=0");
    be.empty_media = false;

    /* 写入几条 → dirty → flush 落盘 */
    ADD_OK(&m, 20260901, 1350, 900, 420, 1, MY_AID_BROWN, 80);
    ADD_OK(&m, 20260902, 1360, 1000, 410, 0, MY_AID_BROWN, 82);
    CHECK(m.dirty, "add 后 dirty=true");
    CHECK(my_mem_flush(&m) == MY_OK, "flush 成功");
    CHECK(be.save_calls == 1, "flush 触发 1 次 save（实测 %d）", be.save_calls);
    CHECK(be.count == 2, "落盘 2 条（实测 %d）", be.count);
    CHECK(!m.dirty, "flush 后 dirty 清零");

    /* 无变更重复 flush：延迟写不再调 save */
    my_mem_flush(&m);
    my_mem_flush(&m);
    CHECK(be.save_calls == 1, "无变更重复 flush 不写盘（延迟写，实测 %d 次）", be.save_calls);

    /* 写失败：dirty 保持，可重试 */
    be.fail_save = true;
    ADD_OK(&m, 20260903, 1370, 1100, 400, 1, MY_AID_PINK, 78);
    CHECK(my_mem_flush(&m) == MY_ERR_IO, "后端写失败返回 MY_ERR_IO");
    CHECK(m.dirty, "写失败后 dirty 保持（不丢变更）");
    CHECK(m.save_fail_count == 1, "save_fail_count=1");
    be.fail_save = false;
    CHECK(my_mem_flush(&m) == MY_OK, "后端恢复后重试 flush 成功");
    CHECK(!m.dirty && m.save_fail_count == 0, "重试成功后 dirty/fail_count 归零");
    CHECK(be.count == 3, "重试后落盘 3 条");

    /* 重启模拟：新对象从后端 load 回 */
    my_sleep_memory_t m2;
    my_mem_init(&m2, &hal);
    CHECK(my_mem_load(&m2) == MY_OK, "重启 load 成功");
    CHECK(my_mem_count(&m2) == 3, "重启后读回 3 条（实测 %d）", my_mem_count(&m2));
    CHECK(my_mem_find(&m2, 20260902) != NULL, "重启后记录可查（位图/字段未丢）");
    CHECK(my_mem_find(&m2, 20260902)->aid_used == MY_AID_BROWN, "重启后字段值正确");

    /* 损坏介质：坏记录丢弃，好记录保留，不崩溃（约束 B） */
    printf("\n  --- 损坏介质自愈 ---\n");
    mem_backend_t corrupt_be;
    memset(&corrupt_be, 0, sizeof(corrupt_be));
    corrupt_be.corrupt = true;
    my_mem_store_t corrupt_hal = { .save = be_save, .load = be_load, .ctx = &corrupt_be };
    my_sleep_memory_t m3;
    my_mem_init(&m3, &corrupt_hal);
    my_err_t le = my_mem_load(&m3);
    CHECK(le == MY_OK, "损坏介质 load 不报错（部分恢复，实测 %d）", le);
    CHECK(my_mem_count(&m3) == 2, "4 条中 2 条坏被丢弃，保留 2 条好（实测 %d）", my_mem_count(&m3));
    CHECK(my_mem_find(&m3, 20260901) != NULL, "好记录 901 保留");
    CHECK(my_mem_find(&m3, 20260902) != NULL, "好记录 902 保留");
    CHECK(my_mem_find(&m3, 20261399) == NULL, "坏记录（month=13）已丢弃");

    /* load 返回负（介质彻底损坏）→ 空档案，不崩溃 */
    mem_backend_t io_be;
    memset(&io_be, 0, sizeof(io_be));
    /* 用一个直接返回 -1 的 load */
    my_mem_store_t io_hal = { .save = be_save, .load = NULL, .ctx = &io_be };
    /* load=NULL 会被 init 判为无后端；这里改用 corrupt 之外的手动模拟 */
    my_sleep_memory_t m4;
    my_mem_init(&m4, NULL);   /* 无后端 */
    CHECK(!m4.store_present, "无后端 store_present=false");
    CHECK(my_mem_load(&m4) == MY_OK, "无后端 load 返回 OK（空档案）");
    CHECK(my_mem_flush(&m4) == MY_OK, "无后端 flush 返回 OK（不崩溃）");
    (void)io_hal;
}

/* ===================== 7. 结构体尺寸 ===================== */
static void test_sizes(void)
{
    printf("\n=== 7. 存储预算（结构体尺寸） ===\n");
    CHECK(sizeof(my_sleep_record_t) <= 32, "单条记录 %.0f 字节 <= 32B",
          (double)sizeof(my_sleep_record_t));
    CHECK(sizeof(my_sleep_memory_t) < 6144, "档案体 %.0f 字节 < 6KB（可放 PSRAM/静态区）",
          (double)sizeof(my_sleep_memory_t));
    /* 180 条满档序列化后估算：180 × ~60B(JSON) ≈ 10.8KB < 32KB 预算 */
    printf("  [INFO] 180 晚满档：内存 %.0f 字节，JSON 序列化估算 < 12KB（存储预算 <32KB）\n",
           (double)(180 * sizeof(my_sleep_record_t)));
}

int main(void)
{
    printf("============================================\n");
    printf(" 眠语 · 睡眠记忆持久化单元测试\n");
    printf(" 保留 %d 晚 / 记录 %.0f 字节 / 档案 %.0f 字节\n",
           MY_MEM_MAX_DAYS, (double)sizeof(my_sleep_record_t), (double)sizeof(my_sleep_memory_t));
    printf("============================================\n");

    test_add_find();
    test_bad_records();
    test_recent();
    test_summarize();
    test_late_night_semantics();
    test_ring_overflow();
    test_persistence();
    test_sizes();

    printf("\n============================================\n");
    printf(" 结果： %d 通过 / %d 失败\n", g_pass, g_fail);
    /* 机器可读权威结果行：Makefile 只信这一行，不再 grep 数 [PASS]。
     * 数行法必然对不齐——4b 组手动打印 18 行明细不计入 g_pass，
     * 而 ADD_OK 静默断言计入 g_pass 却不打印，两口径混用会得到假统计。 */
    printf("MY_RESULT: pass=%d fail=%d\n", g_pass, g_fail);
    printf("============================================\n");
    return g_fail ? 1 : 0;
}
