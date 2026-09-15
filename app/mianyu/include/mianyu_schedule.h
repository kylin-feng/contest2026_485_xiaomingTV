/* SPDX-License-Identifier: Apache-2.0
 *
 * 安眠科技 · 主动任务调度层（cron 定时引擎 + 哄睡场景编排）
 *
 * ====================== 为什么"主动式"需要一个独立调度层 ======================
 *
 * 普通音箱是【被动】的：用户说"放白噪音"，它才放。哄睡智能体要【主动】：
 *   • 22:30 自动开始睡前流程（不必用户每晚手动开启）
 *   • 检测到入睡 → 自动淡出（这个由 sleep_detect 事件驱动，不走时间）
 *   • 凌晨 3 点检测到夜醒 → 主动安抚
 *   • 浅睡期或 7:00 智能唤醒
 * 主动的前提是"设备自己知道现在几点、该做什么"。这一层就是把
 * 「时间」翻译成「该触发哪个哄睡动作」的编排器。
 *
 * ====================== 分层与可移植性 ======================
 *
 * 本层拆成两半，只有前半是核心 IP、需要单元测试：
 *   ① cron 引擎（纯逻辑，平台无关）：表达式解析 + 时刻匹配 + 下次触发计算。
 *      不含任何 OS/时间/存储调用，时间由调用方喂入 → PC 上可完整测试。
 *   ② 持久化（走 HAL 接口注入）：任务表要存盘（重启不丢用户的作息设置），
 *      但存到哪、用什么格式是平台相关的：真机 = LittleFS + cJSON，
 *      PC 测试 = 内存后端。核心层只依赖 my_sched_store_t 抽象接口，
 *      不 #include cJSON，保证这一层在任何比赛/平台都能原样复用。
 *
 * ====================== 三个关键健壮性设计 ======================
 *
 *   A. 降级 fallback：cron 字符串被写坏（用户手滑、配置损坏）时，
 *      任务不退化为"永不触发"，而是回落到「每日 HH:MM」简单比对。
 *      睡前流程是核心功能，绝不能因一个格式错误就整晚不工作。
 *   B. 错过补偿 catch-up：设备可能在触发时刻处于关机/深睡。开机后
 *      回看 catchup 窗口内是否错过了触发点，按策略补一次（睡前该补，
 *      晨唤过时不补）—— 每个任务单独配 catchup 行为。
 *   C. 触发去重：调度器可能被高频调用（每秒/每帧），同一分钟内
 *      一个任务只能触发一次，靠 last_fire 分钟序号锁存。
 */
#ifndef MIANYU_SCHEDULE_H
#define MIANYU_SCHEDULE_H

#include "mianyu_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== 平台无关时间表示 =====================
 * 不用 <time.h> 的 struct tm：真机时间来源是 RTC/网络授时，字段与语义
 * 各平台不一；自定义结构让 cron 逻辑与"时间从哪来"彻底解耦。 */
typedef struct {
    int year;     /* 1970.. */
    int month;    /* 1..12 */
    int day;      /* 1..31 */
    int hour;     /* 0..23 */
    int minute;   /* 0..59 */
    int second;   /* 0..59（cron 匹配到分钟，秒仅用于去重/日志） */
    int wday;     /* 0=周日 .. 6=周六；由 my_datetime_normalize 自动算出 */
} my_datetime_t;

/* 从 year/month/day 用 Sakamoto 算法补全 wday，并校验字段合法性。
 * 调用方只需填 y/m/d/h/mi/s，wday 交给它。返回 MY_OK 或 MY_ERR_PARAM。 */
my_err_t my_datetime_normalize(my_datetime_t *dt);

/* datetime → 自 1970-01-01 起的绝对分钟数（int64，足够 ~1.4 万亿年）。
 * 用于"遍历过去/未来若干分钟找触发点"，把日期运算化简成整数加减。
 * 非法输入返回负值。 */
int64_t my_datetime_to_minutes(const my_datetime_t *dt);

/* 绝对分钟数 → datetime（my_datetime_to_minutes 的逆）。返回 MY_OK/MY_ERR_PARAM。 */
my_err_t my_datetime_from_minutes(int64_t minutes, my_datetime_t *dt);

/* ===================== cron 表达式引擎 =====================
 * 支持标准 5 字段："分 时 日 月 周"
 *   每字段语法：* | N | A-B | *​/S | A-B/S | 逗号列表（以上任意组合）
 *   周字段 0 和 7 都表示周日。
 *   dom 与 dow 都受限时取【并集】（Vixie cron 语义）。
 * 不支持：秒字段、@yearly 等宏、L/W/# 等扩展（哄睡场景用不到，
 *   留这些反而增加解析器出 bug 的面积）。 */

#define MY_CRON_MAX_EXPR 64

typedef struct {
    uint64_t minutes;      /* bit i (0..59) 置位 = 命中该分钟 */
    uint32_t hours;        /* bit i (0..23) */
    uint32_t dom;          /* bit i (1..31)；bit0 不用 */
    uint16_t months;       /* bit i (1..12)；bit0 不用 */
    uint8_t  dow;          /* bit i (0..6) */
    bool     dom_restricted;   /* dom 字段是否非 '*'（决定并集语义） */
    bool     dow_restricted;   /* dow 字段是否非 '*' */
    bool     valid;            /* 解析是否成功；false 时须走 fallback */
} my_cron_t;

MY_STATIC_ASSERT(sizeof(my_cron_t) <= 32, cron_rule_is_compact);

/* 解析 cron 表达式。成功返回 MY_OK 且 c->valid=true；
 * 失败返回 MY_ERR_PARAM 且 c->valid=false（调用方据此启用 fallback）。
 * 不修改输入字符串。 */
my_err_t my_cron_parse(my_cron_t *c, const char *expr);

/* 判断某时刻是否命中规则。c->valid=false 时恒返回 false（不误触发）。 */
bool my_cron_match(const my_cron_t *c, const my_datetime_t *dt);

/* 求 from 之后（不含 from 当分钟）最近一次命中时刻，写入 next。
 * 搜索上限 max_scan_minutes（防无解规则死循环，如 "0 0 30 2 *" 永不出现）。
 * 找到返回 MY_OK；窗口内无命中返回 MY_ERR_TIMEOUT。 */
my_err_t my_cron_next(const my_cron_t *c, const my_datetime_t *from,
                      my_datetime_t *next, int max_scan_minutes);

/* ===================== 主动任务定义 ===================== */

typedef enum {
    MY_TASK_BEDTIME = 0,        /* 睡前流程：定时启动白噪+暖光 */
    MY_TASK_MORNING_WAKE,       /* 晨间唤醒：定时或浅睡期 */
    MY_TASK_NIGHT_COMFORT,      /* 夜醒安抚（多为事件驱动，也可定时巡检） */
    MY_TASK_NAP,                /* 午休小睡 */
    MY_TASK_CUSTOM,             /* 用户自定义 */
    MY_TASK_KIND_MAX
} my_task_kind_t;

/* 错过补偿策略 */
typedef enum {
    MY_CATCHUP_NEVER = 0,       /* 过时不补（晨唤：7 点没醒就算了，别 9 点吵人） */
    MY_CATCHUP_ONCE,            /* 补触发一次（睡前：23 点开机仍该哄睡） */
} my_catchup_t;

#define MY_TASK_MAX 8

typedef struct {
    bool           enabled;
    my_task_kind_t kind;
    char           cron_expr[MY_CRON_MAX_EXPR];  /* 原始表达式（存盘用） */
    my_cron_t      cron;                          /* 解析结果 */
    /* fallback：cron 解析失败时启用的"每日 HH:MM"简单规则 */
    int            fb_hour;        /* 0..23，-1 = fallback 也不可用 */
    int            fb_minute;      /* 0..59 */
    my_catchup_t   catchup;
    int            catchup_window_min;  /* 回看多少分钟找错过的触发点 */
    /* 运行期状态（不存盘） */
    int64_t        last_fire_min;  /* 上次触发的绝对分钟，-1 = 从未 */
    uint32_t       fire_count;     /* 累计触发次数（统计/调试） */
} my_task_t;

/* 触发动作由上层实现（播放/调光/发 Skill），核心层只回调不关心具体动作。
 * task 为触发的任务，dt 为触发时刻，is_catchup 标明是否为补触发。 */
typedef void (*my_task_cb_t)(const my_task_t *task, const my_datetime_t *dt,
                             bool is_catchup, void *user);

/* ---- 持久化 HAL（存储后端注入，核心层不绑 cJSON/LittleFS） ---- */
typedef struct {
    /* 载入任务表；返回读取到的任务数，<0 表示失败（用 fallback 默认表）。
     * buf/cap 为后端自管的序列化缓冲，核心层不关心其格式。 */
    int  (*load)(my_task_t *tasks, int max_tasks, void *store_ctx);
    /* 保存任务表；返回 MY_OK/MY_ERR_IO。脏标记由调度器维护，仅变更时调用。 */
    my_err_t (*save)(const my_task_t *tasks, int count, void *store_ctx);
    void *ctx;   /* 后端上下文（真机=文件路径句柄，PC=内存结构指针） */
} my_sched_store_t;

typedef struct {
    my_task_t      tasks[MY_TASK_MAX];
    int            task_count;
    my_task_cb_t   on_fire;
    void          *cb_user;
    my_sched_store_t store;
    bool           store_present;   /* 是否配置了持久化后端 */
    bool           dirty;           /* 任务表有未保存变更 */
    bool           started;         /* 已 my_sched_start（用于 catch-up 一次性判定） */
    my_datetime_t  last_dt;         /* 上次 tick 的时间，判分钟跳变 */
    bool           has_last_dt;
} my_sched_t;

/* ---- 生命周期 ---- */
/* 初始化空调度器（无任务）。store 传 NULL 则不持久化（掉电丢设置）。 */
my_err_t my_sched_init(my_sched_t *s, const my_sched_store_t *store,
                       my_task_cb_t on_fire, void *cb_user);

/* 载入默认哄睡任务表（睡前 22:30、晨唤 7:00 等），用于首次启动/恢复出厂。
 * 会覆盖现有任务。默认表全部带 fallback，保证无脑可用。 */
my_err_t my_sched_load_defaults(my_sched_t *s);

/* 从持久化后端载入任务表（开机读盘）。store 未配置时返回 MY_OK（保持空表）。
 * 后端只需还原 cron_expr/kind/enabled/fb/catchup 等原始字段，解析结果由
 * 核心层据此重算（单一事实来源）；last_fire_min/fire_count 属运行期状态，
 * 载入后统一重置。返回 MY_OK / MY_ERR_IO；读取失败或空盘时调用方再回退
 * my_sched_load_defaults。 */
my_err_t my_sched_load(my_sched_t *s);

/* ---- 任务管理 ---- */
/* 新增/更新一个任务。expr 为 cron 表达式；若解析失败，自动用 fb_hour/fb_minute
 * 兜底并把 task->cron.valid 置 false（仍可正常调度）。返回任务索引或负错误码。 */
int my_sched_add_task(my_sched_t *s, my_task_kind_t kind, const char *expr,
                      int fb_hour, int fb_minute, my_catchup_t catchup,
                      int catchup_window_min);

/* 启停任务（改动会置 dirty）。 */
my_err_t my_sched_enable(my_sched_t *s, int index, bool enabled);

/* 改任务的 cron 表达式（运行期重设作息，如用户把睡前从 22:30 改到 23:00）。 */
my_err_t my_sched_set_expr(my_sched_t *s, int index, const char *expr);

const my_task_t *my_sched_get_task(const my_sched_t *s, int index);
int my_sched_task_count(const my_sched_t *s);

/* ---- 运行 ---- */
/* 启动调度：记录基准时间，并对每个任务执行一次 catch-up 回看。
 * 应在设备开机、时间已同步后调用一次。 */
my_err_t my_sched_start(my_sched_t *s, const my_datetime_t *now);

/* 喂入当前时间，检查并触发到期任务。可高频调用（每秒/每帧），
 * 内部按分钟去重。返回本次触发的任务数（>=0）或负错误码。 */
int my_sched_tick(my_sched_t *s, const my_datetime_t *now);

/* 若有未保存变更则调用后端 save 落盘（延迟写：减少 Flash 磨损）。
 * 返回 MY_OK（无需保存或保存成功）/ MY_ERR_IO。 */
my_err_t my_sched_flush(my_sched_t *s);

#ifdef __cplusplus
}
#endif
#endif /* MIANYU_SCHEDULE_H */
