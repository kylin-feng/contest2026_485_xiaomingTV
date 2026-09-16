/* SPDX-License-Identifier: Apache-2.0
 *
 * 安眠科技 · 睡眠记忆持久化实现
 * 设计动机与三个工程约束见 mianyu_sleep_memory.h。
 *
 * 环形缓冲不变量：
 *   count <= MY_MEM_MAX_DAYS；rec[0..count) 为有效记录，date_key 互不相同
 *   （add/load 均按 date_key 去重）。顺序不保证，find 靠 date_key 线性查。
 *   write_idx 仅在缓冲满后用于覆盖最旧槽（这里用最旧 date_key 定位，见 add）。
 *
 * 【嵌入式栈安全】本文件所有函数【不分配大块栈数组】。
 *   初版曾在 load/recent/summarize 各开 my_sleep_record_t[180]（≈5.7KB），
 *   且 summarize→recent 嵌套达 11.5KB，在 M33/NuttX（任务栈常 2~8KB）上
 *   必然栈溢出。现改为：
 *     • load：直接填进 m->rec，原地压缩去重（O(1) 额外栈）
 *     • recent：上界扫描选择排序，直接写调用方 out（O(1) 额外栈）
 *     • summarize：先算"第 k 大 date_key"阈值，再单遍聚合（O(1) 额外栈）
 *   代价是几处 O(count²)（count<=180 → <=3.24 万次整数比较，微秒级），
 *   在低频的记忆读写路径上完全可接受，换来零大栈占用。
 */
#include "mianyu_sleep_memory.h"
#include <string.h>

/* ---- 单条记录合法性校验（load/add 时逐条过，坏数据拒收不崩） ----
 * 只校验字段范围，不校验业务合理性（quality 高但 total 短可能是真实情况）。 */
static bool record_sane(const my_sleep_record_t *r)
{
    if (r->date_key <= 0) return false;               /* 0/负 = 空槽 */
    if (r->date_key < 19700101 || r->date_key > 29991231) return false;
    int mm = (r->date_key / 100) % 100;
    int dd = r->date_key % 100;
    if (mm < 1 || mm > 12) return false;
    if (dd < 1 || dd > 31) return false;
    if (r->bedtime_min < -1 || r->bedtime_min > 1439) return false;
    if (r->sleep_onset_sec < 0 || r->sleep_onset_sec > 86400) return false;
    if (r->total_sleep_min < 0 || r->total_sleep_min > 1440) return false;
    if (r->aid_used >= MY_AID_KIND_MAX) return false;
    if (r->quality > 100) return false;
    return true;
}

/* 按 date_key 线性查内部槽位，返回索引或 -1 */
static int find_slot(const my_sleep_memory_t *m, int32_t date_key)
{
    for (int i = 0; i < m->count; i++) {
        if (m->rec[i].date_key == date_key) return i;
    }
    return -1;
}

/* 第 k 大（k 从 1 起）的 date_key。O(count*k) 时间、O(1) 栈。
 * date_key 互异，故"上界扫描取最大"可稳定选出第 k 大。
 * 返回 0 表示不足 k 条。summarize 用它划出"最近 want 晚"的窗口阈值，
 * 避免把整份档案拷到栈上再排序。 */
static int32_t nth_largest_date_key(const my_sleep_memory_t *m, int k)
{
    if (k <= 0) return 0;
    int32_t upper = INT32_MAX;   /* 严格小于 upper 的才算候选 */
    int32_t result = 0;
    for (int i = 0; i < k; i++) {
        int best = -1;
        for (int j = 0; j < m->count; j++) {
            if (m->rec[j].date_key >= upper) continue;
            if (best < 0 || m->rec[j].date_key > m->rec[best].date_key) best = j;
        }
        if (best < 0) break;          /* 不足 k 条 */
        result = m->rec[best].date_key;
        upper = result;
    }
    return result;
}

my_err_t my_mem_init(my_sleep_memory_t *m, const my_mem_store_t *store)
{
    if (!m) return MY_ERR_PARAM;
    memset(m, 0, sizeof(*m));
    m->write_idx = 0;
    if (store && store->save && store->load) {
        m->store = *store;
        m->store_present = true;
    }
    return MY_OK;
}

my_err_t my_mem_load(my_sleep_memory_t *m)
{
    if (!m) return MY_ERR_PARAM;
    if (!m->store_present) return MY_OK;   /* 无后端 = 空档案，不算错 */

    int wi = 0;
    /* 直接填进 m->rec，省掉初版的 my_sleep_record_t[180] 栈缓冲 */
    int n = m->store.load(m->rec, MY_MEM_MAX_DAYS, &wi, m->store.ctx);
    if (n < 0) {
        /* 介质损坏/读失败：回落到空档案，不崩溃（约束 B） */
        m->count = 0;
        m->write_idx = 0;
        return MY_ERR_IO;
    }
    if (n > MY_MEM_MAX_DAYS) n = MY_MEM_MAX_DAYS;

    /* 原地压缩：逐条校验，坏记录丢弃、同 date_key 去重（保留后者）。
     * kept 始终 <= i，故 m->rec[kept] = m->rec[i] 不会覆盖未读数据。 */
    int kept = 0;
    for (int i = 0; i < n; i++) {
        if (!record_sane(&m->rec[i])) continue;
        int dup = -1;
        for (int k = 0; k < kept; k++) {
            if (m->rec[k].date_key == m->rec[i].date_key) { dup = k; break; }
        }
        if (dup >= 0) { m->rec[dup] = m->rec[i]; continue; }
        if (kept != i) m->rec[kept] = m->rec[i];
        kept++;
    }
    m->count = kept;
    m->write_idx = (kept > 0 && wi >= 0 && wi < kept) ? wi : 0;
    m->dirty = false;
    return MY_OK;
}

my_err_t my_mem_flush(my_sleep_memory_t *m)
{
    if (!m) return MY_ERR_PARAM;
    if (!m->store_present || !m->dirty) return MY_OK;   /* 延迟写（约束 A） */
    my_err_t e = m->store.save(m->rec, m->count, m->write_idx, m->store.ctx);
    if (e == MY_OK) {
        m->dirty = false;
        m->save_fail_count = 0;
    } else {
        m->save_fail_count++;   /* dirty 保持，可重试 */
    }
    return e;
}

/* 满缓冲时定位"最旧"槽（date_key 最小者）以覆盖。O(count)。 */
static int oldest_slot(const my_sleep_memory_t *m)
{
    int best = 0;
    for (int i = 1; i < m->count; i++) {
        if (m->rec[i].date_key < m->rec[best].date_key) best = i;
    }
    return best;
}

/* ---- 晚睡判定：必须先把"当日分钟数"折算到"睡眠夜时间轴" ----
 *
 * 【这里原先是个真 bug】初版直接写 `if (bedtime_min > 1410) late++;`
 * （1410 = 23:30）。但 bedtime_min 是【当日分钟数 0..1439】，跨午夜上床的人
 * 落在 0 附近：
 *     凌晨 00:10 上床 → bedtime_min = 10  → 10 > 1410 ? 否 → 判为"没晚睡"
 *     凌晨 03:00 上床 → bedtime_min = 180 → 同样判为"没晚睡"
 *     晚上 23:40 上床 → bedtime_min = 1420 → 判为"晚睡"
 * 结果是【越熬夜越检测不出来】——通宵党 bedtime_min 最小，全被漏掉，
 * 而"晚睡关怀"恰恰是主动式哄睡最该开口的场景。这个 bug 在单元测试里
 * 被"非法值被 record_sane 静默拒收"掩盖了一次（late 恒为 0，看着像
 * 窗口过滤问题），是重写断言后才暴露的。
 *
 * 修法：以 18:00 为睡眠夜的 0 点做环形折算，让 18:00→次日 17:59 单调递增。
 *     night_min = (bedtime_min - 1080 + 1440) % 1440
 *     22:30 → 270    23:30 → 330    00:10 → 420    03:00 → 540
 * 这样跨午夜自然接在 23:xx 之后，单调可比。
 *
 * 再配上界：只认 [23:30, 次日 06:00) 为"晚睡"。06:00 之后上床的是通宵/
 * 白天补觉（night_min >= 720），语义上不属于"睡得晚"，混进计数会把
 * "熬夜"和"倒班补觉"揉成一团，关怀话术就会说错（对补觉的人说"你又熬夜了"
 * 是很蠢的体验）。 */
#define MY_NIGHT_EPOCH_MIN   1080   /* 18:00，睡眠夜时间轴原点 */
#define MY_LATE_FROM_NIGHT    330   /* 折算后 >= 330 即 23:30 起算晚睡 */
#define MY_LATE_TO_NIGHT      720   /* 折算后 <  720 即次日 06:00 前 */

static int32_t night_offset_min(int32_t bedtime_min)
{
    /* 返回 0..1439；入参已在 record_sane 里保证落在 [-1,1439] */
    return (bedtime_min - MY_NIGHT_EPOCH_MIN + 2 * 1440) % 1440;
}

static bool is_late_night(int32_t bedtime_min)
{
    if (bedtime_min < 0) return false;      /* -1 = 未知，不计入 */
    int32_t nm = night_offset_min(bedtime_min);
    return nm >= MY_LATE_FROM_NIGHT && nm < MY_LATE_TO_NIGHT;
}

bool my_mem_is_late_night(int32_t bedtime_min)
{
    return is_late_night(bedtime_min);
}

int my_mem_add(my_sleep_memory_t *m, const my_sleep_record_t *rec)
{
    if (!m || !rec) return MY_ERR_PARAM;
    if (!record_sane(rec)) return MY_ERR_PARAM;   /* 拒写非法记录 */

    /* 同晚去重：已存在则更新该条（多次写入合并） */
    int slot = find_slot(m, rec->date_key);
    if (slot >= 0) {
        m->rec[slot] = *rec;
        m->rec[slot].flags |= 0x01;   /* 置有效位 */
        m->dirty = true;
        return slot;
    }

    int idx;
    if (m->count < MY_MEM_MAX_DAYS) {
        idx = m->count++;                       /* 未满：追加 */
    } else {
        idx = oldest_slot(m);                   /* 已满：覆盖最旧（按 date_key） */
        m->write_idx = idx;
    }
    m->rec[idx] = *rec;
    m->rec[idx].flags |= 0x01;
    m->dirty = true;
    return idx;
}

const my_sleep_record_t *my_mem_find(const my_sleep_memory_t *m, int32_t date_key)
{
    if (!m) return NULL;
    int slot = find_slot(m, date_key);
    return slot >= 0 ? &m->rec[slot] : NULL;
}

int my_mem_recent(const my_sleep_memory_t *m, my_sleep_record_t *out, int n)
{
    if (!m || !out || n <= 0 || m->count == 0) return 0;
    int want = n < m->count ? n : m->count;

    /* 上界扫描选择：第 i 轮取"date_key < upper 中的最大者"，天然降序。
     * 直接写调用方 out，不在栈上拷贝整份档案（省 5.7KB 栈）。
     * date_key 互异，无需 tie-break。 */
    int32_t upper = INT32_MAX;
    int filled = 0;
    for (int i = 0; i < want; i++) {
        int best = -1;
        for (int j = 0; j < m->count; j++) {
            if (m->rec[j].date_key >= upper) continue;
            if (best < 0 || m->rec[j].date_key > m->rec[best].date_key) best = j;
        }
        if (best < 0) break;              /* 理论上不会：want<=count 且键互异 */
        out[i] = m->rec[best];
        upper = m->rec[best].date_key;
        filled++;
    }
    return filled;
}

my_err_t my_mem_summarize(const my_sleep_memory_t *m, int days, my_sleep_summary_t *sum)
{
    if (!m || !sum) return MY_ERR_PARAM;
    memset(sum, 0, sizeof(*sum));
    sum->best_aid = -1;
    if (m->count == 0) return MY_OK;

    int want = (days <= 0 || days > m->count) ? m->count : days;

    /* 窗口阈值：最近 want 晚 = date_key >= thresh_all。
     * 半程阈值：近半 = date_key >= thresh_half（用于趋势对比）。
     * 用阈值单遍聚合，避免把档案拷到栈上（省 5.7KB 栈）。 */
    int32_t thresh_all  = nth_largest_date_key(m, want);
    int32_t thresh_half = (want >= 4) ? nth_largest_date_key(m, want / 2) : 0;

    /* 各手段入睡潜伏期累计（偏好学习 best_aid） */
    int64_t aid_sum[MY_AID_KIND_MAX] = {0};
    int     aid_cnt[MY_AID_KIND_MAX] = {0};

    int64_t onset_sum = 0, total_sum = 0, wake_sum = 0, qual_sum = 0;
    int64_t near_sum = 0, far_sum = 0;
    int valid = 0, near_cnt = 0, far_cnt = 0;

    for (int i = 0; i < m->count; i++) {
        const my_sleep_record_t *r = &m->rec[i];
        if (r->date_key < thresh_all) continue;   /* 在窗口外（更旧） */
        valid++;
        onset_sum += r->sleep_onset_sec;
        total_sum += r->total_sleep_min;
        wake_sum  += r->night_wake_count;
        qual_sum  += r->quality;
        if (r->aid_used < MY_AID_KIND_MAX && r->sleep_onset_sec > 0) {
            aid_sum[r->aid_used] += r->sleep_onset_sec;
            aid_cnt[r->aid_used]++;
        }
        /* 趋势分桶：近半 vs 远半 */
        if (thresh_half > 0) {
            if (r->date_key >= thresh_half) { near_sum += r->sleep_onset_sec; near_cnt++; }
            else                            { far_sum  += r->sleep_onset_sec; far_cnt++;  }
        }
    }

    if (valid == 0) return MY_OK;   /* best_aid 已置 -1 */

    sum->valid_days     = valid;
    sum->avg_onset_sec  = (int32_t)(onset_sum / valid);
    sum->avg_total_min  = (int32_t)(total_sum / valid);
    sum->avg_wake_count = (int32_t)(wake_sum * 100 / valid);   /* ×100 定点 */
    sum->avg_quality    = (int32_t)(qual_sum / valid);

    /* best_aid：有使用记录且平均入睡潜伏期最低者（跳过 AID_NONE） */
    int best = -1;
    int64_t best_onset = 0;
    for (int a = 1; a < MY_AID_KIND_MAX; a++) {
        if (aid_cnt[a] == 0) continue;
        int64_t avg = aid_sum[a] / aid_cnt[a];
        if (best < 0 || avg < best_onset) { best = a; best_onset = avg; }
    }
    sum->best_aid = best;
    sum->best_aid_onset_sec = (int32_t)best_onset;

    /* late_nights_7d：近 7 晚中"晚睡"次数（跨午夜折算，见 is_late_night）。
     * 只取 7 条到小栈缓冲（7×20B=140B，安全；不拷整份档案）。
     * recent 输出按 date_key 降序，故前 min(n7, want) 条正好是【窗口内】最近 7 晚。 */
    my_sleep_record_t small7[7];
    int n7 = my_mem_recent(m, small7, 7);
    int lim7 = n7 < want ? n7 : want;
    int late = 0;
    for (int i = 0; i < lim7; i++) {
        if (is_late_night(small7[i].bedtime_min)) late++;
    }
    sum->late_nights_7d = late;

    /* trend：近半 vs 远半的平均入睡潜伏期差（正=变慢=变差）。 */
    if (near_cnt > 0 && far_cnt > 0) {
        sum->trend_onset_sec = (int32_t)(near_sum / near_cnt - far_sum / far_cnt);
    }
    return MY_OK;
}

int my_mem_count(const my_sleep_memory_t *m)
{
    return m ? m->count : 0;
}
