/* SPDX-License-Identifier: Apache-2.0
 *
 * 眠语 · HAL 后端：openvela / NuttX 真机（SF32LB52-DevKit-LCD）
 *
 * 这是上真机时链接的 backend，实现 hal/mianyu_hal.h 的同一套 my_hal_* 符号。
 * 与 hal/sim 的区别只是「平台能力从哪来」：
 *   时间   = NuttX RTC（gettimeofday）
 *   存储   = LittleFS 文件（二进制版本化序列化，依赖零、掉电可恢复）
 *   音频出 = 音频框架 DMA 环形缓冲（TODO 接入，见下）
 *   麦克风 = 音频框架输入流（TODO 接入，见下）
 *   灯光   = LVGL 呼吸光晕（app/ui/breathe_lvgl.c）
 *   IMU    = 无板载 IMU（DevKit-LCD），恒 false
 *
 * 编译位置：本文件在 openvela 工程内编译（见 app/mianyu/CMakeLists.txt），
 * 不在 PC 的 `make` 里（PC 用 hal/sim）。移植时按 app/PORT_TO_OPENVELA.md 走。
 *
 * 【诚实边界】带 `[TODO 接 SDK]` 的段落是硬件接入点：具体 API（音频框架的
 * 流句柄、MIC 采集接口、LVGL 线程模型）以实际 vendor_sifli SDK 为准，本骨架
 * 只给出正确的数据流位置与调用契约，上真机时逐点替换，核心层一字不改。
 */
#include "mianyu_hal.h"
#include "mianyu_common.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

/* ===================== 时间（NuttX RTC） ===================== */

my_err_t my_hal_time_now(my_datetime_t *dt)
{
    if (!dt) return MY_ERR_PARAM;
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) return MY_ERR_IO;
    struct tm tmv;
    if (localtime_r(&tv.tv_sec, &tmv) == NULL) return MY_ERR_IO;
    dt->year   = tmv.tm_year + 1900;
    dt->month  = tmv.tm_mon + 1;
    dt->day    = tmv.tm_mday;
    dt->hour   = tmv.tm_hour;
    dt->minute = tmv.tm_min;
    dt->second = tmv.tm_sec;
    return my_datetime_normalize(dt);   /* 补 wday */
}

/* ===================== 存储：LittleFS 二进制序列化 =====================
 *
 * 选二进制而非 cJSON 的理由：任务表/睡眠档案都是【定长紧凑结构】，
 * fwrite/fread 一次读写，无解析歧义、无转义/半条 JSON 的健壮性问题，
 * 也不引入 cJSON 依赖（构建更简单）。文件头带 magic + 版本，坏盘可自愈。
 * 若产品后续需要「人类可读/跨版本迁移」，再换 cJSON——接口不变。 */

#define VELA_STORE_DIR   "/data/mianyu"
#define VELA_SCHED_FILE  VELA_STORE_DIR "/sched.bin"
#define VELA_MEM_FILE    VELA_STORE_DIR "/sleep_mem.bin"
#define VELA_MAGIC       0x4D59554Bu   /* "MYUK" */
#define VELA_VER         1u

typedef struct {
    uint32_t magic;
    uint32_t version;
    int32_t  count;
} file_hdr_t;

static my_err_t write_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return MY_ERR_IO;
    size_t n = fwrite(data, 1, len, f);
    fclose(f);
    return n == len ? MY_OK : MY_ERR_IO;
}

static int read_file(const char *path, void *data, size_t max_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;                     /* 文件不存在 = 空盘 */
    size_t n = fread(data, 1, max_len, f);
    fclose(f);
    return (int)n;
}

/* 校验文件头，合法返回记录数（>0），空盘/坏盘/版本不符返回 0。
 * 调用方用 0 判定「回退默认表/空档案」，绝不因一条坏盘崩溃。 */
static int check_hdr(const void *buf, int got, uint32_t *count_out)
{
    if (got < (int)sizeof(file_hdr_t)) return 0;
    file_hdr_t hdr;
    memcpy(&hdr, buf, sizeof(hdr));
    if (hdr.magic != VELA_MAGIC || hdr.version != VELA_VER) return 0;
    if (hdr.count <= 0) return 0;
    *count_out = hdr.count;
    return 1;
}

/* ---- 调度任务表后端 ---- */

static int vela_sched_load(my_task_t *tasks, int max_tasks, void *ctx)
{
    (void)ctx;
    static uint8_t buf[sizeof(file_hdr_t) + sizeof(my_task_t) * MY_TASK_MAX];
    int got = read_file(VELA_SCHED_FILE, buf, sizeof(buf));
    uint32_t count = 0;
    if (!check_hdr(buf, got, &count)) return 0;
    if (count > (uint32_t)max_tasks) return 0;
    size_t need = sizeof(file_hdr_t) + sizeof(my_task_t) * count;
    if (got < (int)need) return 0;         /* 文件被截断（掉电）→ 丢弃 */
    memcpy(tasks, buf + sizeof(file_hdr_t), sizeof(my_task_t) * count);
    return (int)count;
}

static my_err_t vela_sched_save(const my_task_t *tasks, int count, void *ctx)
{
    (void)ctx;
    if (count < 0 || count > MY_TASK_MAX) return MY_ERR_PARAM;
    static uint8_t buf[sizeof(file_hdr_t) + sizeof(my_task_t) * MY_TASK_MAX];
    file_hdr_t hdr = { VELA_MAGIC, VELA_VER, (uint32_t)count };
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), tasks, sizeof(my_task_t) * (size_t)count);
    return write_file(VELA_SCHED_FILE, buf, sizeof(hdr) + sizeof(my_task_t) * (size_t)count);
}

/* ---- 睡眠档案后端 ---- */

static int vela_mem_load(my_sleep_record_t *records, int max_records,
                         int *write_idx, void *ctx)
{
    (void)ctx;
    if (write_idx) *write_idx = 0;
    static uint8_t buf[sizeof(file_hdr_t) + sizeof(my_sleep_record_t) * MY_MEM_MAX_DAYS];
    int got = read_file(VELA_MEM_FILE, buf, sizeof(buf));
    uint32_t count = 0;
    if (!check_hdr(buf, got, &count)) return 0;
    if (count > (uint32_t)max_records) return 0;
    size_t need = sizeof(file_hdr_t) + sizeof(my_sleep_record_t) * count;
    if (got < (int)need) return 0;
    memcpy(records, buf + sizeof(file_hdr_t), sizeof(my_sleep_record_t) * count);
    if (write_idx) *write_idx = (int)(count % MY_MEM_MAX_DAYS);   /* 环形写指针回推 */
    return (int)count;
}

static my_err_t vela_mem_save(const my_sleep_record_t *records, int count,
                              int write_idx, void *ctx)
{
    (void)ctx; (void)write_idx;
    if (count < 0 || count > MY_MEM_MAX_DAYS) return MY_ERR_PARAM;
    static uint8_t buf[sizeof(file_hdr_t) + sizeof(my_sleep_record_t) * MY_MEM_MAX_DAYS];
    file_hdr_t hdr = { VELA_MAGIC, VELA_VER, (uint32_t)count };
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), records, sizeof(my_sleep_record_t) * (size_t)count);
    return write_file(VELA_MEM_FILE, buf, sizeof(hdr) + sizeof(my_sleep_record_t) * (size_t)count);
}

static my_sched_store_t s_sched_store = { vela_sched_load, vela_sched_save, NULL };
static my_mem_store_t   s_mem_store   = { vela_mem_save, vela_mem_load, NULL };

const my_sched_store_t *my_hal_sched_store(void) { return &s_sched_store; }
const my_mem_store_t   *my_hal_mem_store(void)   { return &s_mem_store; }

/* ===================== 生命周期 ===================== */

my_err_t my_hal_init(void)
{
    /* [TODO 接 SDK] 挂载 LittleFS：NuttX 的 mount() / boardctl，确保
     * VELA_STORE_DIR 可写；初始化音频框架与 RTC 同步（NTP/网络授时）。 */
    return MY_OK;
}

void my_hal_deinit(void) { /* [TODO 接 SDK] 释放音频流、关闭 LVGL */ }

void my_hal_sleep_ms(int ms)
{
    /* 真机：让出 CPU，真实等 ms（usleep 即可满足非实时节拍） */
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ===================== 音频输出 ===================== */

int my_hal_audio_play(const my_pcm_t *buf, int count)
{
    /* [TODO 接 SDK] 把 buf[0..count) 写入音频框架的播放 DMA 环形缓冲。
     * 非阻塞：缓冲满则丢弃或等待，绝不阻塞主循环。参考 openvela 音频
     * 框架（media_server / audio 驱动）的 PCM 写入接口。 */
    (void)buf; (void)count;
    return MY_ERR_UNSUPPORTED;
}

void my_hal_audio_stop(void)
{
    /* [TODO 接 SDK] 清空播放缓冲、停流（入睡淡出归零后调用）。 */
}

/* ===================== 麦克风输入 ===================== */

int my_hal_mic_read(my_pcm_t *buf, int max_count)
{
    /* [TODO 接 SDK] 从音频框架的 MIC 输入环形缓冲拷贝已采到的 PCM。
     * 16kHz/16bit/mono；无新数据返回 0。 */
    (void)buf; (void)max_count;
    return 0;
}

/* ===================== 体动（IMU） ===================== */

bool my_hal_imu_available(void)
{
    /* DevKit-LCD 无板载 IMU：入睡判定走 MIC 呼吸节律单一主判据。
     * （若换黄山派，此函数返回 true 并实现 imu_read 读 LSM6DS3TR-C。） */
    return false;
}

my_err_t my_hal_imu_read(float *ax, float *ay, float *az)
{
    (void)ax; (void)ay; (void)az;
    return MY_ERR_UNSUPPORTED;
}

/* ===================== 呼吸引导灯 ===================== */

void my_hal_light_set(int level_pct)
{
    /* [TODO 接 SDK] 把亮度映射到 LVGL 呼吸光晕。breathe_lvgl.c 已有
     * 三层同心圆的 opacity 更新逻辑，这里把 level_pct 传给它的 set 接口
     * （注意 LVGL 非线程安全，跨线程用 lv_async_call）。 */
    (void)level_pct;
}
