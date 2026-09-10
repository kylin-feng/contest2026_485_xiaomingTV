/* SPDX-License-Identifier: Apache-2.0
 *
 * 眠语 · 主动任务调度层实现
 * 设计动机与健壮性策略见 mianyu_schedule.h。
 *
 * 日期运算采用 Howard Hinnant 的 days_from_civil / civil_from_days
 * （公历 <-> 连续日数的经典无分支算法，覆盖全 int 年份范围），
 * 星期用 Sakamoto 算法。两者都是公开验证过的实现，避免自己造闰年 bug。
 */
#include "mianyu_schedule.h"
#include <string.h>
#include <stdlib.h>

#define MIN_PER_DAY 1440

/* ===================== 公历 <-> 连续日数 ===================== */

static bool is_leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int days_in_month(int y, int m)
{
    static const int dim[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m < 1 || m > 12) return 0;
    if (m == 2 && is_leap(y)) return 29;
    return dim[m - 1];
}

/* 公历(y,m,d) → 自 1970-01-01 的天数（可为负）。Howard Hinnant 算法。 */
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);                       /* [0,399] */
    unsigned doy = (153 * (m + (m > 2 ? -3u : 9u)) + 2) / 5 + d - 1; /* [0,365] */
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;            /* [0,146096] */
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

/* 连续天数 → 公历(y,m,d)。Howard Hinnant 算法（days_from_civil 的逆）。 */
static void civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);                     /* [0,146096] */
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  /* [0,399] */
    int yi = (int)yoe + (int)(era * 400);
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);          /* [0,365] */
    unsigned mp = (5 * doy + 2) / 153;                               /* [0,11]，0=March */
    unsigned di = doy - (153 * mp + 2) / 5 + 1;                      /* [1,31] */
    /* March-based 月序还原成日历月：mp 0..9 = 3..12 月（+3），
     * mp 10,11 = 1,2 月（-9）。这里写成 mp<10?+3:-9 的无负数等价式，
     * 避免 unsigned 下溢。（曾误写 +9，导致 1/2 月变成 19/20 月，
     * 触发 months 位图越界移位 + cron_next 死循环，被 probe 抓出。） */
    unsigned mi = mp < 10 ? mp + 3u : mp - 9u;                       /* [1,12] */
    *y = yi + (mi <= 2);
    *m = mi;
    *d = di;
}

/* Sakamoto：返回 0=周日 .. 6=周六 */
static int sakamoto_wday(int y, int m, int d)
{
    static const int t[12] = {0,3,2,5,0,3,5,1,4,6,2,4};
    if (m < 3) y--;
    return (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
}

my_err_t my_datetime_normalize(my_datetime_t *dt)
{
    if (!dt) return MY_ERR_PARAM;
    if (dt->year < 1970 || dt->month < 1 || dt->month > 12) return MY_ERR_PARAM;
    if (dt->day < 1 || dt->day > days_in_month(dt->year, dt->month)) return MY_ERR_PARAM;
    if (dt->hour < 0 || dt->hour > 23) return MY_ERR_PARAM;
    if (dt->minute < 0 || dt->minute > 59) return MY_ERR_PARAM;
    if (dt->second < 0 || dt->second > 59) return MY_ERR_PARAM;
    dt->wday = sakamoto_wday(dt->year, dt->month, dt->day);
    return MY_OK;
}

int64_t my_datetime_to_minutes(const my_datetime_t *dt)
{
    if (!dt) return -1;
    if (my_datetime_normalize((my_datetime_t *)dt) != MY_OK) return -1;
    int64_t days = days_from_civil(dt->year, (unsigned)dt->month, (unsigned)dt->day);
    return days * MIN_PER_DAY + dt->hour * 60 + dt->minute;
}

my_err_t my_datetime_from_minutes(int64_t minutes, my_datetime_t *dt)
{
    if (!dt || minutes < 0) return MY_ERR_PARAM;
    int64_t days = minutes / MIN_PER_DAY;
    int rem = (int)(minutes % MIN_PER_DAY);
    int y; unsigned m, d;
    civil_from_days(days, &y, &m, &d);
    dt->year = y; dt->month = (int)m; dt->day = (int)d;
    dt->hour = rem / 60;
    dt->minute = rem % 60;
    dt->second = 0;
    dt->wday = sakamoto_wday(y, (int)m, (int)d);
    return MY_OK;
}

/* ===================== cron 字段解析 ===================== */

/* 解析 [p, pend) 内的一段十进制整数，全为数字且非空才成功 */
static bool parse_uint_span(const char *p, const char *pend, int *out)
{
    if (p >= pend) return false;
    int v = 0;
    for (const char *q = p; q < pend; q++) {
        if (*q < '0' || *q > '9') return false;
        v = v * 10 + (*q - '0');
        if (v > 100000) return false;   /* 溢出保护 */
    }
    *out = v;
    return true;
}

/* 解析单个逗号项（如 "5" / "1-3" / "*​/10" / "1-30/5" / "5/10"）到位图 */
static my_err_t parse_item(const char *p, const char *pend,
                           int lo, int hi, uint64_t *bits)
{
    if (p >= pend) return MY_ERR_PARAM;

    /* 分离步进部分 */
    const char *slash = NULL;
    for (const char *q = p; q < pend; q++) if (*q == '/') { slash = q; break; }
    const char *range_end = slash ? slash : pend;

    int step = 1;
    if (slash) {
        if (!parse_uint_span(slash + 1, pend, &step)) return MY_ERR_PARAM;
        if (step < 1) return MY_ERR_PARAM;
    }

    int start, end;
    if (range_end - p == 1 && *p == '*') {
        start = lo; end = hi;                      /* '*' → 全范围 */
    } else {
        const char *dash = NULL;
        for (const char *q = p; q < range_end; q++) if (*q == '-') { dash = q; break; }
        if (dash) {
            if (!parse_uint_span(p, dash, &start)) return MY_ERR_PARAM;
            if (!parse_uint_span(dash + 1, range_end, &end)) return MY_ERR_PARAM;
        } else {
            if (!parse_uint_span(p, range_end, &start)) return MY_ERR_PARAM;
            /* "N/S" = 从 N 到 hi 步进 S；"N" = 仅 N */
            end = slash ? hi : start;
        }
    }

    if (start < lo || end > hi || start > end) return MY_ERR_PARAM;
    for (int v = start; v <= end; v += step) {
        *bits |= (uint64_t)1 << v;
    }
    return MY_OK;
}

/* 解析一个字段（逗号分隔的多个项）。restricted 记录是否非 '*'。 */
static my_err_t parse_field(const char *s, int lo, int hi,
                            uint64_t *bits, bool *restricted)
{
    *bits = 0;
    if (!s || *s == '\0') return MY_ERR_PARAM;

    if (strcmp(s, "*") == 0) {
        for (int v = lo; v <= hi; v++) *bits |= (uint64_t)1 << v;
        *restricted = false;
        return MY_OK;
    }
    *restricted = true;

    const char *p = s;
    while (*p) {
        const char *comma = strchr(p, ',');
        const char *item_end = comma ? comma : p + strlen(p);
        if (parse_item(p, item_end, lo, hi, bits) != MY_OK) return MY_ERR_PARAM;
        if (!comma) break;
        p = comma + 1;
        if (*p == '\0') return MY_ERR_PARAM;   /* 尾随逗号 */
    }
    return MY_OK;
}

/* 把整个表达式切成 5 个字段（空格分隔），返回各字段起止指针。
 * 不修改输入。字段数 != 5 → MY_ERR_PARAM。 */
static my_err_t split_fields(const char *expr, const char *f[5][2])
{
    int n = 0;
    const char *p = expr;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;       /* 跳前导空白 */
        if (*p == '\0') break;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (n >= 5) return MY_ERR_PARAM;            /* 超过 5 字段 */
        f[n][0] = start;
        f[n][1] = p;
        n++;
    }
    return n == 5 ? MY_OK : MY_ERR_PARAM;
}

/* 从 [start,end) 复制成 C 字符串到 buf（长度 cap）。仅用于按字段解析。 */
static void span_to_str(const char *start, const char *end, char *buf, size_t cap)
{
    size_t n = (size_t)(end - start);
    if (n >= cap) n = cap - 1;
    memcpy(buf, start, n);
    buf[n] = '\0';
}

my_err_t my_cron_parse(my_cron_t *c, const char *expr)
{
    if (!c) return MY_ERR_PARAM;
    memset(c, 0, sizeof(*c));
    c->valid = false;
    if (!expr) return MY_ERR_PARAM;

    const char *f[5][2];
    if (split_fields(expr, f) != MY_OK) return MY_ERR_PARAM;

    char field[MY_CRON_MAX_EXPR];
    uint64_t bits;
    bool restricted;

    /* 分 0-59 */
    span_to_str(f[0][0], f[0][1], field, sizeof(field));
    if (parse_field(field, 0, 59, &bits, &restricted) != MY_OK) return MY_ERR_PARAM;
    c->minutes = bits;

    /* 时 0-23 */
    span_to_str(f[1][0], f[1][1], field, sizeof(field));
    if (parse_field(field, 0, 23, &bits, &restricted) != MY_OK) return MY_ERR_PARAM;
    c->hours = (uint32_t)bits;

    /* 日 1-31 */
    span_to_str(f[2][0], f[2][1], field, sizeof(field));
    if (parse_field(field, 1, 31, &bits, &restricted) != MY_OK) return MY_ERR_PARAM;
    c->dom = (uint32_t)bits;
    c->dom_restricted = restricted;

    /* 月 1-12 */
    span_to_str(f[3][0], f[3][1], field, sizeof(field));
    if (parse_field(field, 1, 12, &bits, &restricted) != MY_OK) return MY_ERR_PARAM;
    c->months = (uint16_t)bits;

    /* 周 0-7（7 折叠为 0=周日） */
    span_to_str(f[4][0], f[4][1], field, sizeof(field));
    if (parse_field(field, 0, 7, &bits, &restricted) != MY_OK) return MY_ERR_PARAM;
    if (bits & ((uint64_t)1 << 7)) bits |= 1;        /* 7 → 0 */
    bits &= 0x7F;                                     /* 只留 0-6 */
    c->dow = (uint8_t)bits;
    c->dow_restricted = restricted;

    c->valid = true;
    return MY_OK;
}

/* 日/周匹配（Vixie cron 并集语义：两者都受限时取 OR） */
static bool day_field_match(const my_cron_t *c, const my_datetime_t *dt)
{
    bool dom_hit = (c->dom >> dt->day) & 1u;
    bool dow_hit = (c->dow >> dt->wday) & 1u;
    if (c->dom_restricted && c->dow_restricted) return dom_hit || dow_hit;
    if (c->dom_restricted) return dom_hit;
    if (c->dow_restricted) return dow_hit;
    return true;   /* 两者皆 '*' */
}

bool my_cron_match(const my_cron_t *c, const my_datetime_t *dt)
{
    if (!c || !dt || !c->valid) return false;
    if (dt->minute < 0 || dt->minute > 59) return false;
    if (dt->hour < 0 || dt->hour > 23) return false;
    if (dt->month < 1 || dt->month > 12) return false;
    if (!((c->minutes >> dt->minute) & 1u)) return false;
    if (!((c->hours >> dt->hour) & 1u)) return false;
    if (!((c->months >> dt->month) & 1u)) return false;
    return day_field_match(c, dt);
}

my_err_t my_cron_next(const my_cron_t *c, const my_datetime_t *from,
                      my_datetime_t *next, int max_scan_minutes)
{
    if (!c || !from || !next || !c->valid || max_scan_minutes <= 0) return MY_ERR_PARAM;

    int64_t base = my_datetime_to_minutes(from);
    if (base < 0) return MY_ERR_PARAM;
    int64_t t = base + 1;                     /* 严格晚于 from 那一分钟 */
    int64_t limit = base + max_scan_minutes;

    while (t <= limit) {
        my_datetime_t cur;
        if (my_datetime_from_minutes(t, &cur) != MY_OK) return MY_ERR_PARAM;

        if (!((c->months >> cur.month) & 1u)) {
            /* 跳到下月 1 号 00:00：日历跳变，用 civil 运算 */
            int y = cur.year, m = cur.month + 1;
            if (m > 12) { m = 1; y++; }
            t = days_from_civil(y, (unsigned)m, 1u) * MIN_PER_DAY;
            continue;
        }
        if (!day_field_match(c, &cur)) {
            t = (t / MIN_PER_DAY + 1) * MIN_PER_DAY;   /* 跳到次日 00:00 */
            continue;
        }
        if (!((c->hours >> cur.hour) & 1u)) {
            t = (t / 60 + 1) * 60;                     /* 跳到下一个整点 */
            continue;
        }
        if (!((c->minutes >> cur.minute) & 1u)) {
            t += 1;                                    /* 下一分钟 */
            continue;
        }
        *next = cur;
        return MY_OK;
    }
    return MY_ERR_TIMEOUT;   /* 窗口内无命中（如 "0 0 30 2 *" 2月30日永不存在） */
}

/* ===================== 任务匹配（cron 优先，失败走 fallback） ===================== */

static bool task_match_minute(const my_task_t *task, const my_datetime_t *dt)
{
    if (task->cron.valid) return my_cron_match(&task->cron, dt);
    /* fallback：每日固定 HH:MM */
    if (task->fb_hour < 0) return false;
    return dt->hour == task->fb_hour && dt->minute == task->fb_minute;
}

/* ===================== 调度器 ===================== */

my_err_t my_sched_init(my_sched_t *s, const my_sched_store_t *store,
                       my_task_cb_t on_fire, void *cb_user)
{
    if (!s) return MY_ERR_PARAM;
    memset(s, 0, sizeof(*s));
    s->on_fire = on_fire;
    s->cb_user = cb_user;
    if (store && store->load && store->save) {
        s->store = *store;
        s->store_present = true;
    }
    s->has_last_dt = false;
    return MY_OK;
}

int my_sched_add_task(my_sched_t *s, my_task_kind_t kind, const char *expr,
                      int fb_hour, int fb_minute, my_catchup_t catchup,
                      int catchup_window_min)
{
    if (!s) return MY_ERR_PARAM;
    if (s->task_count >= MY_TASK_MAX) return MY_ERR_NOMEM;
    if (kind < 0 || kind >= MY_TASK_KIND_MAX) return MY_ERR_PARAM;

    my_task_t *t = &s->tasks[s->task_count];
    memset(t, 0, sizeof(*t));
    t->enabled = true;
    t->kind = kind;
    t->catchup = catchup;
    t->catchup_window_min = catchup_window_min < 0 ? 0 : catchup_window_min;
    t->last_fire_min = -1;
    t->fb_hour = fb_hour;
    t->fb_minute = fb_minute;

    if (expr && expr[0]) {
        /* 有界复制到 cron_expr（存盘用原文，便于 UI 回显/编辑） */
        size_t n = strlen(expr);
        if (n >= MY_CRON_MAX_EXPR) n = MY_CRON_MAX_EXPR - 1;
        memcpy(t->cron_expr, expr, n);
        t->cron_expr[n] = '\0';
        my_cron_parse(&t->cron, t->cron_expr);   /* 失败则 cron.valid=false，走 fallback */
    } else {
        t->cron.valid = false;
    }

    int idx = s->task_count++;
    s->dirty = true;
    return idx;
}

my_err_t my_sched_load_defaults(my_sched_t *s)
{
    if (!s) return MY_ERR_PARAM;
    s->task_count = 0;   /* 覆盖现有任务 */

    /* 睡前流程：每晚 22:30。cron 坏了也 fallback 到 22:30。
     * catchup ONCE / 回看 120 分钟：23:00 才开机也应补一次哄睡。 */
    if (my_sched_add_task(s, MY_TASK_BEDTIME, "30 22 * * *",
                          22, 30, MY_CATCHUP_ONCE, 120) < 0) return MY_ERR_NOMEM;
    /* 晨间唤醒：每天 7:00。catchup NEVER：过了点就别再吵人。 */
    if (my_sched_add_task(s, MY_TASK_MORNING_WAKE, "0 7 * * *",
                          7, 0, MY_CATCHUP_NEVER, 0) < 0) return MY_ERR_NOMEM;
    /* 午休小睡：工作日 13:00（周一到周五 = dow 1-5）。 */
    if (my_sched_add_task(s, MY_TASK_NAP, "0 13 * * 1-5",
                          13, 0, MY_CATCHUP_NEVER, 0) < 0) return MY_ERR_NOMEM;

    s->dirty = true;
    return MY_OK;
}

my_err_t my_sched_load(my_sched_t *s)
{
    if (!s) return MY_ERR_PARAM;
    if (!s->store_present) return MY_OK;   /* 无后端：保持空表 */

    int n = s->store.load(s->tasks, MY_TASK_MAX, s->store.ctx);
    if (n < 0) return MY_ERR_IO;           /* 读取失败：调用方回退默认表 */
    if (n > MY_TASK_MAX) n = MY_TASK_MAX;

    s->task_count = n;
    /* 后端只还原原始字段，解析结果由核心层重算（单一事实来源）。
     * last_fire_min/fire_count 是运行期状态、不存盘，载入后统一重置。 */
    for (int i = 0; i < n; i++) {
        my_task_t *t = &s->tasks[i];
        if (t->cron_expr[0]) my_cron_parse(&t->cron, t->cron_expr);
        else t->cron.valid = false;
        t->last_fire_min = -1;
        t->fire_count = 0;
    }
    s->dirty = false;                      /* 刚载入，无需回写 */
    return MY_OK;
}

my_err_t my_sched_enable(my_sched_t *s, int index, bool enabled)
{
    if (!s || index < 0 || index >= s->task_count) return MY_ERR_PARAM;
    s->tasks[index].enabled = enabled;
    s->dirty = true;
    return MY_OK;
}

my_err_t my_sched_set_expr(my_sched_t *s, int index, const char *expr)
{
    if (!s || index < 0 || index >= s->task_count || !expr) return MY_ERR_PARAM;
    my_task_t *t = &s->tasks[index];
    size_t n = strlen(expr);
    if (n >= MY_CRON_MAX_EXPR) n = MY_CRON_MAX_EXPR - 1;
    memset(t->cron_expr, 0, sizeof(t->cron_expr));
    memcpy(t->cron_expr, expr, n);
    my_cron_parse(&t->cron, t->cron_expr);   /* 解析失败自动落 fallback */
    t->last_fire_min = -1;                   /* 改作息后重新计触发 */
    s->dirty = true;
    return MY_OK;
}

const my_task_t *my_sched_get_task(const my_sched_t *s, int index)
{
    if (!s || index < 0 || index >= s->task_count) return NULL;
    return &s->tasks[index];
}

int my_sched_task_count(const my_sched_t *s)
{
    return s ? s->task_count : 0;
}

my_err_t my_sched_start(my_sched_t *s, const my_datetime_t *now)
{
    if (!s || !now) return MY_ERR_PARAM;
    int64_t now_min = my_datetime_to_minutes(now);
    if (now_min < 0) return MY_ERR_PARAM;

    for (int i = 0; i < s->task_count; i++) {
        my_task_t *t = &s->tasks[i];
        if (!t->enabled || t->catchup != MY_CATCHUP_ONCE || t->catchup_window_min <= 0)
            continue;
        /* 回看 [now-window, now) 是否错过了触发点。错过则此刻补触发一次。 */
        bool missed = false;
        int64_t from = now_min - t->catchup_window_min;
        if (from < 0) from = 0;
        for (int64_t m = from; m < now_min; m++) {
            my_datetime_t dt;
            if (my_datetime_from_minutes(m, &dt) != MY_OK) continue;
            if (task_match_minute(t, &dt)) { missed = true; break; }
        }
        if (missed && s->on_fire) {
            s->on_fire(t, now, true, s->cb_user);
            t->fire_count++;
        }
        /* 无论是否补触发，都把 last_fire 锁到当前分钟，避免本分钟重复触发 */
        t->last_fire_min = now_min;
    }
    s->last_dt = *now;
    s->has_last_dt = true;
    s->started = true;
    return MY_OK;
}

int my_sched_tick(my_sched_t *s, const my_datetime_t *now)
{
    if (!s || !now) return MY_ERR_PARAM;
    int64_t now_min = my_datetime_to_minutes(now);
    if (now_min < 0) return MY_ERR_PARAM;

    int fired = 0;
    for (int i = 0; i < s->task_count; i++) {
        my_task_t *t = &s->tasks[i];
        if (!t->enabled) continue;
        if (!task_match_minute(t, now)) continue;
        if (t->last_fire_min == now_min) continue;   /* 同一分钟去重 */

        t->last_fire_min = now_min;
        t->fire_count++;
        fired++;
        if (s->on_fire) s->on_fire(t, now, false, s->cb_user);
    }
    s->last_dt = *now;
    s->has_last_dt = true;
    return fired;
}

my_err_t my_sched_flush(my_sched_t *s)
{
    if (!s) return MY_ERR_PARAM;
    if (!s->dirty || !s->store_present) return MY_OK;   /* 延迟写：无变更不落盘 */
    my_err_t e = s->store.save(s->tasks, s->task_count, s->store.ctx);
    if (e == MY_OK) s->dirty = false;
    return e;
}
