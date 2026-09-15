/* SPDX-License-Identifier: Apache-2.0
 *
 * 眠语 · 睡眠记忆持久化（长期作息档案）
 *
 * ====================== 这一层是"主动"的记忆基座 ======================
 *
 * 哄睡智能体要能主动开口，前提是【记得用户的过去】：
 *   • "你这周有 4 天都过了 1 点才睡，今晚要不要早点开始？"（趋势）
 *   • "你上次听棕噪睡得最快（23 分钟），今晚还用棕噪？"（偏好学习）
 *   • "昨夜你 3 点醒过一次，是不是空调太冷？"（关怀）
 * 这些都要一个可长期累积、断电不丢、能快速聚合查询的睡眠档案。
 *
 * ====================== 分层与可移植性（沿用 schedule 模块的验证模式） ======================
 *
 *   ① 数据结构 + 环形缓冲 + 聚合算法：平台无关纯 C，PC 上完整单元测试。
 *      180 天历史用【定长环形数组】，O(1) 覆盖最旧、O(n) 聚合，n<=180 可控。
 *   ② 序列化 / 落盘：走 HAL 接口注入（my_mem_store_t），核心层【不 #include cJSON、
 *      不碰 LittleFS】。真机后端 = cJSON 拼 JSON + LittleFS 写文件；
 *      PC 测试后端 = 内存 buffer。这样这一层在任何比赛/平台都能原样复用。
 *
 * ====================== 三个关键工程约束 ======================
 *
 *   A. Flash 磨损：NOR Flash 擦写寿命有限（LittleFS 虽 wear-leveling，
 *      也不该每秒写）。用【脏标记 + 延迟写】：只在"整晚记录落定"这类
 *      低频事件才 flush，运行期改内存不碰盘。
 *   B. 断电/写坏自愈：JSON 可能被掉电截断成半条。load 时校验字段范围，
 *      坏记录丢弃、好记录保留，绝不因一条脏数据丢掉整份档案；
 *      彻底无法解析则回落到空档案（不崩溃）。
 *   C. 存储预算：180 条记录，每条定长紧凑结构，序列化后目标 < 32KB，
 *      落进 LittleFS 一个 4KB sector 的数倍以内，不挤占 16MB Flash。
 */
#ifndef MIANYU_SLEEP_MEMORY_H
#define MIANYU_SLEEP_MEMORY_H

#include "mianyu_common.h"
#include "mianyu_sleep_detect.h"   /* 复用 my_sleep_state_t 等 */

#ifdef __cplusplus
extern "C" {
#endif

#define MY_MEM_MAX_DAYS   180       /* 保留最近 180 晚 */

/* 当晚使用的哄睡手段（用于偏好学习） */
typedef enum {
    MY_AID_NONE = 0,
    MY_AID_WHITE,
    MY_AID_PINK,
    MY_AID_BROWN,
    MY_AID_VOICE,          /* 语音陪伴/故事 */
    MY_AID_MIXED,
    MY_AID_KIND_MAX
} my_aid_t;

/* 一晚的睡眠记录（定长紧凑，便于环形缓冲与序列化） */
typedef struct {
    int32_t  date_key;          /* YYYYMMDD 整数，如 20260904；0=空槽 */
    int32_t  bedtime_min;       /* 计划入睡时刻，当日分钟数 0..1439 */
    int32_t  sleep_onset_sec;   /* 从上床到判定入睡的秒数（sleep_latency）*/
    int32_t  total_sleep_min;   /* 总睡眠时长（分钟） */
    uint8_t  night_wake_count;  /* 夜醒次数 */
    uint8_t  aid_used;          /* my_aid_t，当晚主用手段 */
    uint8_t  quality;           /* 主观/推算睡眠质量 0..100 */
    uint8_t  flags;             /* bit0=有效记录 bit1=用户手动确认 bit2=跨日 */
} my_sleep_record_t;

MY_STATIC_ASSERT(sizeof(my_sleep_record_t) <= 32, sleep_record_compact);

/* 聚合视图（供智能体主动查询，不存盘，按需算） */
typedef struct {
    int      valid_days;         /* 有效记录数 */
    int32_t  avg_onset_sec;      /* 平均入睡潜伏期（秒） */
    int32_t  avg_total_min;      /* 平均睡眠时长（分钟） */
    int32_t  avg_wake_count;     /* 平均夜醒次数（×100 定点，避免浮点） */
    int32_t  avg_quality;        /* 平均质量 0..100 */
    int      best_aid;           /* 入睡最快的手段（my_aid_t），-1=无数据 */
    int32_t  best_aid_onset_sec; /* 该手段的平均入睡潜伏期 */
    int32_t  late_nights_7d;     /* 近 7 晚中"晚睡"次数（超过 bedtime 阈值） */
    int      trend_onset_sec;    /* 近期 vs 更早的入睡潜伏期变化（正=变差） */
} my_sleep_summary_t;

/* ---- 持久化 HAL（后端注入，核心层不绑 cJSON/LittleFS） ----
 * 契约：save 把档案序列化成字节流交给后端；load 反向。
 * 后端负责具体存储（真机=cJSON+LittleFS 文件；测试=内存 buffer）。
 * buf 的所有权在后端，核心层只读写不持有。 */
typedef struct {
    /* 把 records[0..count) 序列化写入持久介质。返回 MY_OK/MY_ERR_IO。 */
    my_err_t (*save)(const my_sleep_record_t *records, int count,
                     int write_idx, void *store_ctx);
    /* 从持久介质反序列化。回填 records/count/write_idx。
     * 返回读到的记录数(>=0)；介质为空返回 0；损坏返回负错误码。 */
    int (*load)(my_sleep_record_t *records, int max_records,
                int *write_idx, void *store_ctx);
    void *ctx;
} my_mem_store_t;

typedef struct {
    my_sleep_record_t rec[MY_MEM_MAX_DAYS];   /* 环形缓冲 */
    int               count;                  /* 有效记录数 0..MAX */
    int               write_idx;              /* 下一个写入位置（环头） */
    my_mem_store_t    store;
    bool              store_present;
    bool              dirty;                  /* 有未落盘变更 */
    int               save_fail_count;        /* 连续保存失败次数（诊断用） */
} my_sleep_memory_t;

MY_STATIC_ASSERT(sizeof(my_sleep_memory_t) < 6144, sleep_mem_fits_6k);

/* ---- 生命周期 ---- */
my_err_t my_mem_init(my_sleep_memory_t *m, const my_mem_store_t *store);
/* 从后端载入档案；store 为空或介质为空则得到空档案。坏记录被跳过。 */
my_err_t my_mem_load(my_sleep_memory_t *m);
/* 落盘（仅 dirty 时真写，延迟写减少 Flash 磨损）。返回 MY_OK/MY_ERR_IO。 */
my_err_t my_mem_flush(my_sleep_memory_t *m);

/* ---- 记录写入 ---- */
/* 追加一晚记录。若 date_key 已存在则【更新】该条（同晚多次写入合并），
 * 否则按环形覆盖最旧。自动置 flags bit0（有效）与 dirty。返回槽位或负错误码。 */
int my_mem_add(my_sleep_memory_t *m, const my_sleep_record_t *rec);

/* 按 date_key 查记录，返回指针或 NULL。 */
const my_sleep_record_t *my_mem_find(const my_sleep_memory_t *m, int32_t date_key);

/* 取最近 n 晚（按日期降序填入 out，返回实际填入数）。n<=0 或 out=NULL 返回 0。 */
int my_mem_recent(const my_sleep_memory_t *m, my_sleep_record_t *out, int n);

/* ---- 聚合查询（智能体主动开口的数据源） ---- */
/* 汇总最近 days 晚（days<=0 表示全部）。结果写入 sum。返回 MY_OK/MY_ERR_PARAM。 */
my_err_t my_mem_summarize(const my_sleep_memory_t *m, int days, my_sleep_summary_t *sum);

/* 记录数 */
int my_mem_count(const my_sleep_memory_t *m);

/* ---- "晚睡"判定的公开语义（Skill 层与调度层共用，避免各处各判一套） ----
 *
 * bedtime_min 是【当日分钟数 0..1439】，跨午夜上床落在 0 附近，
 * 因此绝不能用 `bedtime_min > 23:30` 直接比大小——那会把凌晨 00:10(=10)
 * 判成"没晚睡"，越熬夜越漏检。本函数以 18:00 为睡眠夜原点做环形折算后
 * 再判 [23:30, 次日 06:00) 区间；06:00 之后上床属通宵/白天补觉，
 * 语义上不是"睡得晚"，不计入（否则关怀话术会对补觉的人说"你又熬夜了"）。
 * 返回 true = 属于晚睡。bedtime_min < 0（未知）返回 false。 */
bool my_mem_is_late_night(int32_t bedtime_min);

#ifdef __cplusplus
}
#endif
#endif /* MIANYU_SLEEP_MEMORY_H */
