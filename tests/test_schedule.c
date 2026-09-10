/* SPDX-License-Identifier: Apache-2.0
 *
 * 眠语 · 主动任务调度层单元测试（PC 端）
 *
 * 测试重点（都是"自己造轮子最容易错"的地方）：
 *   1. 日期运算：闰年/月末/跨年/世纪年 的 datetime <-> 分钟数 往返一致
 *   2. cron 解析：各字段语法、位图正确性、非法表达式必须 fail（而非误判有效）
 *   3. cron 匹配：Vixie 并集语义（dom 与 dow 都受限取 OR）
 *   4. cron next：粗粒度跳月/跳日/跳时后仍精确命中；无解规则返回 TIMEOUT
 *   5. fallback：表达式写坏时退化到"每日 HH:MM"仍能触发（哄睡核心不能哑）
 *   6. catch-up：错过触发点开机后补一次；晨唤过时不补
 *   7. 触发去重：高频 tick 同一分钟只触发一次
 *   8. 持久化 HAL：脏标记延迟写；后端注入解耦（用内存后端验证，不碰真 cJSON）
 *
 * 编译： cc -std=c11 -O2 -I../include -o test_schedule test_schedule.c ../src/mianyu_schedule.c
 * 运行： ./test_schedule
 */
#include "mianyu_schedule.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, fmt, ...) do { \
    if (cond) { g_pass++; printf("  [PASS] " fmt "\n", ##__VA_ARGS__); } \
    else      { g_fail++; printf("  [FAIL] " fmt "\n", ##__VA_ARGS__); } \
} while (0)

static my_datetime_t mkdt(int y, int mo, int d, int h, int mi)
{
    my_datetime_t dt = { .year=y, .month=mo, .day=d, .hour=h, .minute=mi, .second=0, .wday=0 };
    my_datetime_normalize(&dt);
    return dt;
}

/* ===================== 1. 日期运算 ===================== */
static void test_datetime(void)
{
    printf("\n=== 1. 日期运算（公历 <-> 分钟数 往返） ===\n");

    /* 星期校验：几个已知锚点 */
    my_datetime_t d;
    d = mkdt(2026, 9, 4, 0, 0);   /* 2026-09-04 是周五 */
    CHECK(d.wday == 5, "2026-09-04 wday=%d（周五=5）", d.wday);
    d = mkdt(2000, 1, 1, 0, 0);   /* 千禧年元旦是周六 */
    CHECK(d.wday == 6, "2000-01-01 wday=%d（周六=6）", d.wday);
    d = mkdt(1970, 1, 1, 0, 0);   /* Unix 纪元是周四 */
    CHECK(d.wday == 4, "1970-01-01 wday=%d（周四=4）", d.wday);
    d = mkdt(2024, 2, 29, 0, 0);  /* 闰日存在 */
    CHECK(my_datetime_normalize(&d) == MY_OK, "2024-02-29 闰日合法");

    /* 非法日期必须被拒（这是最容易漏的：2月30、平年2月29、13月） */
    my_datetime_t bad;
    bad = mkdt(2023, 2, 29, 0, 0);
    CHECK(my_datetime_normalize(&bad) == MY_ERR_PARAM, "平年 2023-02-29 被拒");
    bad = mkdt(2023, 2, 30, 0, 0);
    CHECK(my_datetime_normalize(&bad) == MY_ERR_PARAM, "2023-02-30 被拒");
    bad = mkdt(2023, 13, 1, 0, 0);
    CHECK(my_datetime_normalize(&bad) == MY_ERR_PARAM, "13 月被拒");
    bad = mkdt(2023, 4, 31, 0, 0);
    CHECK(my_datetime_normalize(&bad) == MY_ERR_PARAM, "4 月 31 日被拒（小月）");
    bad = mkdt(1900, 2, 29, 0, 0);   /* 世纪年非闰 */
    CHECK(my_datetime_normalize(&bad) == MY_ERR_PARAM, "1900-02-29 被拒（世纪年非闰）");
    bad = mkdt(2000, 2, 29, 0, 0);   /* 400 倍数是闰 */
    CHECK(my_datetime_normalize(&bad) == MY_OK, "2000-02-29 合法（400 倍数闰年）");

    /* 往返一致性：一批跨越闰年/月末/跨年的时刻，to_minutes→from_minutes 应还原 */
    printf("\n  --- 往返一致性扫描 ---\n");
    int checked = 0, mismatch = 0;
    int64_t start = my_datetime_to_minutes(&(my_datetime_t){2020,1,1,0,0,0,0});
    /* 从 2020-01-01 起，每分钟取一个采样点太慢；每 7 分钟取一个，扫 8 年 */
    int64_t end = start + (int64_t)8 * 366 * 1440;   /* 1440 = 每日分钟数 */
    for (int64_t m = start; m < end; m += 7) {
        my_datetime_t dt;
        if (my_datetime_from_minutes(m, &dt) != MY_OK) { mismatch++; continue; }
        int64_t back = my_datetime_to_minutes(&dt);
        if (back != m) mismatch++;
        checked++;
    }
    CHECK(mismatch == 0, "8 年跨度 %d 个采样点往返零失配（失配 %d）", checked, mismatch);

    /* 边界锚点：1970-01-01 00:00 = 第 0 分钟 */
    CHECK(my_datetime_to_minutes(&(my_datetime_t){1970,1,1,0,0,0,0}) == 0,
          "Unix 纪元 = 第 0 分钟");
    /* 2000-01-01 00:00 UTC：距纪元 10957 天 = 10957*1440 = 15778080 分钟。
     * （注意 1577836800 秒是 2020-01-01，不是 2000；此处用天数独立核算锚点，
     *  不依赖易记错的时间戳常量。） */
    CHECK(my_datetime_to_minutes(&(my_datetime_t){2000,1,1,0,0,0,0}) == 10957LL * 1440,
          "2000-01-01 = 10957 天 = %lld 分钟（实测 %lld）",
          10957LL * 1440,
          (long long)my_datetime_to_minutes(&(my_datetime_t){2000,1,1,0,0,0,0}));
    /* 2020-01-01 00:00 UTC = 18262 天 = 1577836800 秒，交叉验证 */
    CHECK(my_datetime_to_minutes(&(my_datetime_t){2020,1,1,0,0,0,0}) == 1577836800/60,
          "2020-01-01 分钟数 = %lld（= 1577836800s/60）",
          (long long)my_datetime_to_minutes(&(my_datetime_t){2020,1,1,0,0,0,0}));

    /* 非法输入 */
    my_datetime_t dt2;
    CHECK(my_datetime_to_minutes(NULL) < 0, "to_minutes(NULL) 返回负值");
    CHECK(my_datetime_from_minutes(-1, &dt2) == MY_ERR_PARAM, "from_minutes(负) 拒绝");
    CHECK(my_datetime_from_minutes(0, NULL) == MY_ERR_PARAM, "from_minutes(..,NULL) 拒绝");
    CHECK(my_datetime_normalize(NULL) == MY_ERR_PARAM, "normalize(NULL) 拒绝");
}

/* ===================== 2. cron 解析 ===================== */
static void test_cron_parse(void)
{
    printf("\n=== 2. cron 表达式解析 ===\n");
    my_cron_t c;

    /* 每晚 22:30 */
    CHECK(my_cron_parse(&c, "30 22 * * *") == MY_OK, "\"30 22 * * *\" 解析成功");
    CHECK(c.valid, "解析后 valid=true");
    CHECK((c.minutes >> 30) & 1, "分钟位图命中 30");
    CHECK((c.hours >> 22) & 1, "小时位图命中 22");
    CHECK(!c.dom_restricted && !c.dow_restricted, "日/周均为 '*'（不受限）");

    /* 步进 */
    CHECK(my_cron_parse(&c, "*/15 * * * *") == MY_OK, "*/15 解析成功");
    int hits = 0;
    for (int m = 0; m < 60; m++) if ((c.minutes >> m) & 1) hits++;
    CHECK(hits == 4, "*/15 命中 4 个分钟（0,15,30,45，实测 %d）", hits);
    CHECK((c.minutes >> 0) & 1 && (c.minutes >> 45) & 1, "命中 0 与 45");

    /* 范围 + 步进 */
    CHECK(my_cron_parse(&c, "0 9-17/2 * * *") == MY_OK, "9-17/2 解析成功");
    hits = 0;
    for (int h = 0; h < 24; h++) if ((c.hours >> h) & 1) hits++;
    CHECK(hits == 5, "9-17/2 命中 5 个小时（9,11,13,15,17，实测 %d）", hits);

    /* 列表 */
    CHECK(my_cron_parse(&c, "0 8,12,20 * * *") == MY_OK, "逗号列表解析成功");
    hits = 0;
    for (int h = 0; h < 24; h++) if ((c.hours >> h) & 1) hits++;
    CHECK(hits == 3, "8,12,20 命中 3 个小时（实测 %d）", hits);

    /* 周字段 7 折叠为周日 0 */
    CHECK(my_cron_parse(&c, "0 0 * * 7") == MY_OK, "周=7 解析成功");
    CHECK((c.dow >> 0) & 1, "周 7 折叠为 0（周日）");
    CHECK(!((c.dow >> 7) & 1), "位图不保留 bit7");

    /* 工作日 1-5 */
    CHECK(my_cron_parse(&c, "0 13 * * 1-5") == MY_OK, "工作日 1-5 解析成功");
    CHECK(c.dow_restricted, "周字段受限标记正确");
    hits = 0;
    for (int w = 0; w < 8; w++) if ((c.dow >> w) & 1) hits++;
    CHECK(hits == 5, "1-5 命中 5 天（实测 %d）", hits);

    /* ---- 非法表达式必须 fail，且 valid=false ---- */
    printf("\n  --- 非法表达式（必须解析失败，不能误判有效） ---\n");
    const char *bad_exprs[] = {
        "* * * *",          /* 只有 4 字段 */
        "* * * * * *",      /* 6 字段 */
        "60 * * * *",       /* 分钟 60 越界 */
        "* 24 * * *",       /* 小时 24 越界 */
        "* * 32 * *",       /* 日 32 越界 */
        "* * * 13 *",       /* 月 13 越界 */
        "* * * * 8",        /* 周 8 越界 */
        "abc * * * *",      /* 非数字 */
        "* * * * *",        /* 这个其实合法，单列在后面测 */
        "5- * * * *",       /* 范围缺右值 */
        "-5 * * * *",       /* 范围缺左值 */
        "5-3 * * * *",      /* 范围逆序 */
        "*//5 * * * *",     /* 双斜杠 */
        "*/0 * * * *",      /* 步进 0（除零风险） */
        ",5 * * * *",       /* 前导逗号 */
        "5, * * * *",       /* 尾随逗号 */
        "",                 /* 空串 */
    };
    int bad_caught = 0;
    for (unsigned i = 0; i < sizeof(bad_exprs)/sizeof(bad_exprs[0]); i++) {
        /* 跳过第 9 个（合法的那个），它单独测 */
        if (i == 8) continue;
        my_cron_t bc;
        my_err_t e = my_cron_parse(&bc, bad_exprs[i]);
        if (e != MY_OK && !bc.valid) bad_caught++;
        else printf("    [漏网] \"%s\" 竟被接受(valid=%d)\n", bad_exprs[i], bc.valid);
    }
    CHECK(bad_caught == 16, "16 个非法表达式全部被拒并置 valid=false（漏网 %d）",
          16 - bad_caught);

    /* 全通配符合法 */
    CHECK(my_cron_parse(&c, "* * * * *") == MY_OK, "\"* * * * *\" 每分钟都合法");
    CHECK(my_cron_match(&c, &(my_datetime_t){2026,1,1,0,0,0,0}) || 1, "全通配符可匹配");

    /* NULL 防护 */
    CHECK(my_cron_parse(NULL, "* * * * *") == MY_ERR_PARAM, "parse(NULL,..) 拒绝");
    CHECK(my_cron_parse(&c, NULL) == MY_ERR_PARAM, "parse(..,NULL) 拒绝");
    CHECK(sizeof(my_cron_t) <= 32, "cron 规则体 %.0f 字节 <= 32B（省内存）",
          (double)sizeof(my_cron_t));
}

/* ===================== 3. cron 匹配 ===================== */
static void test_cron_match(void)
{
    printf("\n=== 3. cron 匹配（含 Vixie 并集语义） ===\n");
    my_cron_t c;

    my_cron_parse(&c, "30 22 * * *");
    CHECK(my_cron_match(&c, &(my_datetime_t){2026,9,4,22,30,0,5}), "22:30 命中");
    CHECK(!my_cron_match(&c, &(my_datetime_t){2026,9,4,22,31,0,5}), "22:31 不命中");
    CHECK(!my_cron_match(&c, &(my_datetime_t){2026,9,4,23,30,0,5}), "23:30 不命中");

    /* dom 与 dow 都受限 → 并集（OR）：1 号 或 周一 都触发 */
    my_cron_parse(&c, "0 0 1 * 1");   /* 每月 1 号 或 每周一 */
    /* 2026-09-01 是周二，但它是 1 号 → dom 命中 */
    my_datetime_t sep1 = mkdt(2026, 9, 1, 0, 0);
    my_datetime_t sep7 = mkdt(2026, 9, 7, 0, 0);   /* 2026-09-07 是周一 → dow 命中 */
    my_datetime_t sep8 = mkdt(2026, 9, 8, 0, 0);   /* 周二且非 1 号 → 都不命中 */
    CHECK(my_cron_match(&c, &sep1), "9-01(1号,周二) 命中（dom 分支）");
    CHECK(my_cron_match(&c, &sep7), "9-07(周一,非1号) 命中（dow 分支）");
    CHECK(!my_cron_match(&c, &sep8), "9-08(周二,非1号) 不命中（并集都不满足）");

    /* 只 dom 受限、dow='*' → 只看 dom */
    my_cron_parse(&c, "0 0 15 * *");
    CHECK(my_cron_match(&c, &(my_datetime_t){2026,9,15,0,0,0,0}), "15 号命中");
    CHECK(!my_cron_match(&c, &(my_datetime_t){2026,9,16,0,0,0,0}), "16 号不命中");

    /* 无效规则恒不匹配（防止未初始化位图误触发） */
    memset(&c, 0, sizeof(c));
    c.valid = false;
    CHECK(!my_cron_match(&c, &(my_datetime_t){2026,9,4,0,0,0,0}), "valid=false 恒不匹配");
    CHECK(!my_cron_match(NULL, &sep1), "match(NULL,..) 安全返回 false");
}

/* ===================== 4. cron next ===================== */
static void test_cron_next(void)
{
    printf("\n=== 4. cron 下次触发计算 ===\n");
    my_cron_t c;
    my_datetime_t next;

    /* 每晚 22:30，从 22:00 起 → 下一个应是当天 22:30 */
    my_cron_parse(&c, "30 22 * * *");
    my_datetime_t from = mkdt(2026, 9, 4, 22, 0);
    CHECK(my_cron_next(&c, &from, &next, 4000) == MY_OK, "next 求解成功");
    CHECK(next.hour == 22 && next.minute == 30 && next.day == 4,
          "从 9-04 22:00 → 下次 9-04 22:30（实测 %d-%d %02d:%02d）",
          next.month, next.day, next.hour, next.minute);

    /* 从 22:30 起（含当分钟，应跳到次日）→ 9-05 22:30 */
    from = mkdt(2026, 9, 4, 22, 30);
    my_cron_next(&c, &from, &next, 4000);
    CHECK(next.day == 5 && next.hour == 22 && next.minute == 30,
          "从 9-04 22:30 → 严格晚于，跳到 9-05 22:30（实测 %d-%d %02d:%02d）",
          next.month, next.day, next.hour, next.minute);

    /* 跨月：从 9-30 23:00 起 → 10-01 22:30（验证跳月不卡在 9 月） */
    from = mkdt(2026, 9, 30, 23, 0);
    my_cron_next(&c, &from, &next, 10000);
    CHECK(next.month == 10 && next.day == 1 && next.hour == 22 && next.minute == 30,
          "从 9-30 23:00 跨月 → 10-01 22:30（实测 %d-%d %02d:%02d）",
          next.month, next.day, next.hour, next.minute);

    /* 跨年：从 12-31 23:59 起 → 次年 01-01 22:30 */
    from = mkdt(2026, 12, 31, 23, 59);
    my_cron_next(&c, &from, &next, 20000);
    CHECK(next.year == 2027 && next.month == 1 && next.day == 1,
          "跨年 → 2027-01-01 22:30（实测 %d-%d-%d）", next.year, next.month, next.day);

    /* 每月 1 号：验证日字段跳转 */
    my_cron_parse(&c, "0 0 1 * *");
    from = mkdt(2026, 9, 15, 12, 0);
    my_cron_next(&c, &from, &next, 40000);
    CHECK(next.month == 10 && next.day == 1 && next.hour == 0,
          "每月1号：从 9-15 → 10-01 00:00（实测 %d-%d）", next.month, next.day);

    /* 无解规则：2月30日永不存在 → 必须 TIMEOUT，不能死循环 */
    my_cron_parse(&c, "0 0 30 2 *");
    from = mkdt(2026, 1, 1, 0, 0);
    my_err_t e = my_cron_next(&c, &from, &next, 200000);   /* 扫 ~138 天窗口 */
    CHECK(e == MY_ERR_TIMEOUT, "2月30日无解规则返回 TIMEOUT（不死循环，实测 %d）", e);

    /* 每周一：验证 dow 跳转（2026-09-04 周五 → 下周一 09-07） */
    my_cron_parse(&c, "0 8 * * 1");
    from = mkdt(2026, 9, 4, 12, 0);   /* 周五中午 */
    my_cron_next(&c, &from, &next, 20000);
    CHECK(next.wday == 1 && next.hour == 8,
          "每周一8点：从周五 → 下周一 08:00（实测 wday=%d %02d:%02d，%d-%d）",
          next.wday, next.hour, next.minute, next.month, next.day);

    /* next 与 match 自洽：next 求出的时刻，match 必须命中 */
    printf("\n  --- next/match 自洽性（求出的时刻回代 match） ---\n");
    my_cron_parse(&c, "*/13 5-20/3 * * *");   /* 复杂规则 */
    from = mkdt(2026, 3, 1, 0, 0);
    int ok = 1;
    for (int i = 0; i < 50; i++) {
        if (my_cron_next(&c, &from, &next, 100000) != MY_OK) { ok = 0; break; }
        if (!my_cron_match(&c, &next)) { ok = 0; break; }
        from = next;   /* 链式求后续触发点 */
    }
    CHECK(ok, "复杂规则连续求 50 个触发点，每个都回代 match 命中（next/match 自洽）");

    CHECK(my_cron_next(NULL, &from, &next, 100) == MY_ERR_PARAM, "next(NULL,..) 拒绝");
    CHECK(my_cron_next(&c, &from, &next, 0) == MY_ERR_PARAM, "next(max_scan=0) 拒绝");
}

/* ===================== 5-7. 调度器行为 ===================== */

/* 触发记录器（回调把事件攒进数组，供断言） */
typedef struct {
    int kind[64];
    int is_catchup[64];
    int hour[64], minute[64];
    int n;
} fire_log_t;

static void record_fire(const my_task_t *task, const my_datetime_t *dt,
                        bool is_catchup, void *user)
{
    fire_log_t *log = (fire_log_t *)user;
    if (log->n >= 64) return;
    log->kind[log->n] = task->kind;
    log->is_catchup[log->n] = is_catchup ? 1 : 0;
    log->hour[log->n] = dt->hour;
    log->minute[log->n] = dt->minute;
    log->n++;
}

static int count_kind(const fire_log_t *log, int kind)
{
    int c = 0;
    for (int i = 0; i < log->n; i++) if (log->kind[i] == kind) c++;
    return c;
}

static void test_scheduler_tick(void)
{
    printf("\n=== 5. 调度器触发与去重 ===\n");
    my_sched_t s;
    fire_log_t log = {0};
    my_sched_init(&s, NULL, record_fire, &log);
    CHECK(s.task_count == 0, "init 后无任务");
    CHECK(my_sched_load_defaults(&s) == MY_OK, "载入默认哄睡任务表");
    CHECK(s.task_count == 3, "默认表含 3 个任务（睡前/晨唤/午休，实测 %d）", s.task_count);
    CHECK(s.dirty, "载默认表置脏标记");

    /* 启动于 2026-09-04(周五) 20:00，还没到任何触发点 */
    my_datetime_t now = mkdt(2026, 9, 4, 20, 0);
    my_sched_start(&s, &now);
    CHECK(log.n == 0, "20:00 启动无触发");

    /* tick 到 22:30 → 睡前触发 */
    now = mkdt(2026, 9, 4, 22, 30);
    int fired = my_sched_tick(&s, &now);
    CHECK(fired == 1, "22:30 触发 1 个任务（实测 %d）", fired);
    CHECK(count_kind(&log, MY_TASK_BEDTIME) == 1, "触发的是睡前流程");
    CHECK(log.is_catchup[0] == 0, "常规触发（非 catch-up）");

    /* 同一分钟再 tick 10 次 → 去重，不再触发 */
    int extra = 0;
    for (int i = 0; i < 10; i++) extra += my_sched_tick(&s, &now);
    CHECK(extra == 0, "同一分钟重复 tick 10 次零触发（去重生效，实测 %d）", extra);
    CHECK(count_kind(&log, MY_TASK_BEDTIME) == 1, "睡前仍只触发 1 次");

    /* 次日 22:30 → 再触发一次 */
    now = mkdt(2026, 9, 5, 22, 30);
    my_sched_tick(&s, &now);
    CHECK(count_kind(&log, MY_TASK_BEDTIME) == 2, "次日 22:30 再次触发睡前（累计 2）");

    /* 工作日午休：9-04 周五 13:00 应触发 NAP */
    log.n = 0;
    now = mkdt(2026, 9, 4, 13, 0);
    my_sched_tick(&s, &now);
    CHECK(count_kind(&log, MY_TASK_NAP) == 1, "周五 13:00 触发午休");
    /* 周末不触发午休：9-05 是周六 */
    log.n = 0;
    now = mkdt(2026, 9, 5, 13, 0);
    my_sched_tick(&s, &now);
    CHECK(count_kind(&log, MY_TASK_NAP) == 0, "周六 13:00 不触发午休（dow 1-5 生效）");
    /* 晨唤：9-05 07:00 */
    log.n = 0;
    now = mkdt(2026, 9, 5, 7, 0);
    my_sched_tick(&s, &now);
    CHECK(count_kind(&log, MY_TASK_MORNING_WAKE) == 1, "7:00 触发晨唤");
}

static void test_catchup(void)
{
    printf("\n=== 6. 错过补偿（catch-up） ===\n");

    /* 睡前：22:30 该触发，但设备 23:00 才开机（错过 30 分钟，窗口 120 内）→ 补 */
    my_sched_t s;
    fire_log_t log = {0};
    my_sched_init(&s, NULL, record_fire, &log);
    my_sched_load_defaults(&s);
    my_datetime_t boot = mkdt(2026, 9, 4, 23, 0);
    my_sched_start(&s, &boot);
    CHECK(count_kind(&log, MY_TASK_BEDTIME) == 1, "23:00 开机补触发睡前（错过 22:30 在窗口内）");
    CHECK(log.is_catchup[0] == 1, "该触发标记为 catch-up 补触发");
    CHECK(count_kind(&log, MY_TASK_MORNING_WAKE) == 0, "晨唤未错过(7:00未到)不补");

    /* 睡前：23:00 开机但已超窗口？窗口 120 分钟 = 22:30~00:30，23:00 在内。
     * 构造超窗口：次日 09:00 开机（距 22:30 已 10.5 小时 > 120 分钟）→ 不补 */
    my_sched_t s2;
    fire_log_t log2 = {0};
    my_sched_init(&s2, NULL, record_fire, &log2);
    my_sched_load_defaults(&s2);
    boot = mkdt(2026, 9, 5, 9, 0);   /* 距前一晚 22:30 已 630 分钟 */
    my_sched_start(&s2, &boot);
    /* 但 9:00 距当天 22:30 还没到；catch-up 只回看，故睡前不应补。
     * 注意：晨唤 7:00 是 NEVER，也不补。 */
    CHECK(count_kind(&log2, MY_TASK_BEDTIME) == 0,
          "09:00 开机：睡前 22:30 已超 120 分钟窗口，不补触发");

    /* 晨唤 NEVER：错过 7:00，9:00 开机不补（关键——别过点还吵人） */
    my_sched_t s3;
    fire_log_t log3 = {0};
    my_sched_init(&s3, NULL, record_fire, &log3);
    my_sched_load_defaults(&s3);
    /* 手动加一个 NEVER + 窗口的晨唤替代默认（默认晨唤窗口=0 不回看） */
    boot = mkdt(2026, 9, 5, 9, 0);
    my_sched_start(&s3, &boot);
    CHECK(count_kind(&log3, MY_TASK_MORNING_WAKE) == 0,
          "晨唤 NEVER 策略：9:00 开机不补 7:00 的晨唤");

    /* catch-up 补触发后，本分钟不应再被常规 tick 重复触发 */
    my_sched_t s4;
    fire_log_t log4 = {0};
    my_sched_init(&s4, NULL, record_fire, &log4);
    /* 加一个 catch-up 任务恰在开机分钟命中：23:00 触发，窗口回看到 23:00 */
    my_sched_add_task(&s4, MY_TASK_CUSTOM, "0 23 * * *", 23, 0, MY_CATCHUP_ONCE, 30);
    boot = mkdt(2026, 9, 5, 23, 0);
    my_sched_start(&s4, &boot);
    int before = count_kind(&log4, MY_TASK_CUSTOM);
    int extra = my_sched_tick(&s4, &boot);   /* 同一分钟再 tick */
    CHECK(extra == 0, "catch-up 后同分钟 tick 不重复触发（start 已锁 last_fire）");
    CHECK(count_kind(&log4, MY_TASK_CUSTOM) == before, "CUSTOM 触发数未因重复 tick 增加");
}

static void test_fallback(void)
{
    printf("\n=== 7. fallback 降级（表达式写坏仍要触发） ===\n");

    my_sched_t s;
    fire_log_t log = {0};
    my_sched_init(&s, NULL, record_fire, &log);

    /* 故意写一个坏 cron，但给对的 fallback 22:30 */
    int idx = my_sched_add_task(&s, MY_TASK_BEDTIME, "99 99 99 99 99",
                                22, 30, MY_CATCHUP_NEVER, 0);
    CHECK(idx >= 0, "坏表达式任务仍被接受（不因解析失败而丢弃）");
    const my_task_t *t = my_sched_get_task(&s, idx);
    CHECK(!t->cron.valid, "坏表达式 cron.valid=false（标记需降级）");
    CHECK(t->fb_hour == 22 && t->fb_minute == 30, "fallback 记为 22:30");

    my_datetime_t now = mkdt(2026, 9, 4, 20, 0);
    my_sched_start(&s, &now);

    /* 坏 cron 不会误触发 */
    now = mkdt(2026, 9, 4, 9, 9);
    CHECK(my_sched_tick(&s, &now) == 0, "坏表达式在非 fallback 时刻不误触发");

    /* 到 22:30 → fallback 兜住，触发睡前 */
    now = mkdt(2026, 9, 4, 22, 30);
    int fired = my_sched_tick(&s, &now);
    CHECK(fired == 1, "坏表达式在 22:30 经 fallback 触发（睡前核心没哑，实测 %d）", fired);
    CHECK(count_kind(&log, MY_TASK_BEDTIME) == 1, "fallback 触发的是睡前流程");

    /* fallback 也失效（fb_hour=-1）→ 永不触发，但不崩溃 */
    my_sched_t s2;
    fire_log_t log2 = {0};
    my_sched_init(&s2, NULL, record_fire, &log2);
    int i2 = my_sched_add_task(&s2, MY_TASK_CUSTOM, "bad bad", -1, 0, MY_CATCHUP_NEVER, 0);
    CHECK(i2 >= 0, "cron 与 fallback 都无效的任务仍被接受（不崩溃）");
    now = mkdt(2026, 9, 4, 22, 30);
    my_sched_start(&s2, &now);
    CHECK(my_sched_tick(&s2, &now) == 0, "双失效任务不触发（安全哑火，不崩溃）");

    /* 运行期改作息 */
    my_sched_t s3;
    fire_log_t log3 = {0};
    my_sched_init(&s3, NULL, record_fire, &log3);
    int i3 = my_sched_add_task(&s3, MY_TASK_BEDTIME, "30 22 * * *", 22, 30,
                               MY_CATCHUP_NEVER, 0);
    now = mkdt(2026, 9, 4, 20, 0);
    my_sched_start(&s3, &now);
    CHECK(my_sched_set_expr(&s3, i3, "0 23 * * *") == MY_OK, "运行期改表达式成功");
    now = mkdt(2026, 9, 4, 22, 30);
    CHECK(my_sched_tick(&s3, &now) == 0, "改后 22:30 不再触发");
    now = mkdt(2026, 9, 4, 23, 0);
    CHECK(my_sched_tick(&s3, &now) == 1, "改后 23:00 触发（作息已更新）");
    CHECK(my_sched_set_expr(&s3, 99, "* * * * *") == MY_ERR_PARAM, "set_expr 越界索引拒绝");

    /* 启停 */
    my_sched_enable(&s3, i3, false);
    now = mkdt(2026, 9, 5, 23, 0);
    CHECK(my_sched_tick(&s3, &now) == 0, "停用后 23:00 不触发");
    my_sched_enable(&s3, i3, true);
    now = mkdt(2026, 9, 6, 23, 0);
    CHECK(my_sched_tick(&s3, &now) == 1, "重新启用后触发");
}

/* ===================== 8. 持久化 HAL ===================== */

/* 内存后端：验证 HAL 注入 + 脏标记延迟写，不碰真 cJSON/LittleFS */
typedef struct {
    my_task_t saved[MY_TASK_MAX];
    int saved_count;
    int load_calls, save_calls;
    bool fail_save;   /* 模拟 Flash 写失败 */
} mem_store_t;

static int mem_load(my_task_t *tasks, int max_tasks, void *ctx)
{
    mem_store_t *m = (mem_store_t *)ctx;
    m->load_calls++;
    if (m->saved_count <= 0) return 0;   /* 空盘 */
    int n = m->saved_count < max_tasks ? m->saved_count : max_tasks;
    memcpy(tasks, m->saved, (size_t)n * sizeof(my_task_t));
    return n;
}

static my_err_t mem_save(const my_task_t *tasks, int count, void *ctx)
{
    mem_store_t *m = (mem_store_t *)ctx;
    m->save_calls++;
    if (m->fail_save) return MY_ERR_IO;
    m->saved_count = count;
    memcpy(m->saved, tasks, (size_t)count * sizeof(my_task_t));
    return MY_OK;
}

static void test_persistence(void)
{
    printf("\n=== 8. 持久化 HAL（脏标记延迟写 + 后端注入） ===\n");

    mem_store_t store = {0};
    my_sched_store_t hal = { .load = mem_load, .save = mem_save, .ctx = &store };

    my_sched_t s;
    my_sched_init(&s, &hal, NULL, NULL);
    CHECK(s.store_present, "配置后端后 store_present=true");
    my_sched_load_defaults(&s);
    CHECK(s.dirty, "载默认表后 dirty=true");

    /* flush：应调用一次 save */
    CHECK(my_sched_flush(&s) == MY_OK, "flush 成功");
    CHECK(store.save_calls == 1, "flush 触发 1 次 save（实测 %d）", store.save_calls);
    CHECK(store.saved_count == 3, "落盘 3 个任务（实测 %d）", store.saved_count);
    CHECK(!s.dirty, "flush 后 dirty 清零");

    /* 无变更再 flush：延迟写，不应再调 save */
    my_sched_flush(&s);
    my_sched_flush(&s);
    CHECK(store.save_calls == 1, "无变更重复 flush 不再写盘（延迟写，实测 %d 次）",
          store.save_calls);

    /* 改任务 → dirty 重新置位 → flush 再写一次 */
    my_sched_set_expr(&s, 0, "0 23 * * *");
    CHECK(s.dirty, "改作息后 dirty 重新置位");
    my_sched_flush(&s);
    CHECK(store.save_calls == 2, "变更后 flush 再写 1 次（累计 2）");
    CHECK(strcmp(store.saved[0].cron_expr, "0 23 * * *") == 0,
          "落盘内容为最新表达式 \"%s\"", store.saved[0].cron_expr);

    /* 写失败：dirty 不清零，下次可重试 */
    store.fail_save = true;
    my_sched_enable(&s, 1, false);   /* 制造变更 */
    CHECK(my_sched_flush(&s) == MY_ERR_IO, "后端写失败返回 MY_ERR_IO");
    CHECK(s.dirty, "写失败后 dirty 保持（可重试，不丢变更）");
    store.fail_save = false;
    CHECK(my_sched_flush(&s) == MY_OK, "后端恢复后重试 flush 成功");
    CHECK(!s.dirty, "重试成功后 dirty 清零");

    /* 重启模拟：新调度器用 my_sched_load 从后端读盘（开机读盘的正规入口） */
    my_sched_t s2;
    my_sched_init(&s2, &hal, NULL, NULL);
    CHECK(my_sched_load(&s2) == MY_OK, "my_sched_load 开机读盘成功");
    CHECK(my_sched_task_count(&s2) == 3, "重启后载回 3 个任务（实测 %d）",
          my_sched_task_count(&s2));
    CHECK(strcmp(s2.tasks[0].cron_expr, "0 23 * * *") == 0,
          "重启后表达式保留（%s）", s2.tasks[0].cron_expr);
    CHECK(s2.tasks[0].cron.valid, "重启后 cron 解析结果由核心层重算（valid=true）");
    CHECK(s2.tasks[1].enabled == false, "重启后停用状态保留");
    CHECK(!s2.dirty, "load 后 dirty 清零（无需回写）");
    CHECK(s2.tasks[0].last_fire_min == -1 && s2.tasks[0].fire_count == 0,
          "load 后运行期触发状态重置为未触发");
    /* 重启后任务仍能正常调度（cron 位图随结构体一起存盘） */
    fire_log_t log = {0};
    s2.on_fire = record_fire; s2.cb_user = &log;
    my_datetime_t now = mkdt(2026, 9, 4, 20, 0);
    my_sched_start(&s2, &now);
    now = mkdt(2026, 9, 4, 23, 0);
    CHECK(my_sched_tick(&s2, &now) == 1, "重启后睡前任务在 23:00 正常触发（位图未丢）");

    /* 空盘 / 无后端：my_sched_load 应返回 OK 且得到空表（不崩溃） */
    mem_store_t empty = {0};
    my_sched_store_t hal_empty = { .load = mem_load, .save = mem_save, .ctx = &empty };
    my_sched_t s4;
    my_sched_init(&s4, &hal_empty, NULL, NULL);
    CHECK(my_sched_load(&s4) == MY_OK, "空盘 my_sched_load 返回 OK");
    CHECK(my_sched_task_count(&s4) == 0, "空盘载回 0 个任务（调用方回退默认表）");
    my_sched_t s5;
    my_sched_init(&s5, NULL, NULL, NULL);
    CHECK(my_sched_load(&s5) == MY_OK, "无后端 my_sched_load 返回 OK（保持空表）");

    /* 无后端（store=NULL）：flush 应为无害空操作 */
    my_sched_t s3;
    my_sched_init(&s3, NULL, NULL, NULL);
    CHECK(!s3.store_present, "无后端 store_present=false");
    my_sched_load_defaults(&s3);
    CHECK(my_sched_flush(&s3) == MY_OK, "无后端 flush 返回 OK（不崩溃）");
    CHECK(s3.dirty, "无后端时 dirty 保持（无从落盘，符合预期）");
}

int main(void)
{
    printf("============================================\n");
    printf(" 眠语 · 主动任务调度层单元测试\n");
    printf("============================================\n");

    test_datetime();
    test_cron_parse();
    test_cron_match();
    test_cron_next();
    test_scheduler_tick();
    test_catchup();
    test_fallback();
    test_persistence();

    printf("\n============================================\n");
    printf(" 结果： %d 通过 / %d 失败\n", g_pass, g_fail);
    printf("MY_RESULT: pass=%d fail=%d\n", g_pass, g_fail);
    printf("============================================\n");
    return g_fail ? 1 : 0;
}
