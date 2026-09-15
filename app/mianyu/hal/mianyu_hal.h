/* SPDX-License-Identifier: Apache-2.0
 *
 * 眠语 · 平台适配层（HAL）接口定义
 *
 * ====================== 为什么需要这一层 ======================
 *
 * 核心层（include/ + src/）是【平台无关纯 C】，只依赖 <math.h>/<string.h>/<stdint.h>，
 * 不含任何 openvela / NuttX / SDK 头文件。但"哄睡"这件事离不开真实硬件：
 *   要读时间（RTC/网络授时）、要出声音（DAC/Class-D PA）、要采呼吸（MEMS MIC）、
 *   要点灯（AMOLED）、要存档案（LittleFS）。
 *
 * 这一层把【平台能力】抽象成一组薄接口。核心层与 app 主循环只认这套接口，
 * 不认具体硬件；换平台（模拟器 sim / 真机 sf32lb52）只换 backend，一行核心代码不动。
 *
 * ====================== 两个 backend 怎么选 ======================
 *
 *   hal/sim/mianyu_hal_sim.c       —— PC 模拟器后端（无硬件，纯内存 + 合成信号）。
 *       用于 `make app` 在 PC 上把「真机主循环」完整跑一遍，供评审 clone 后自证。
 *   hal/sf32lb52/mianyu_hal_vela.c —— openvela / NuttX 真机后端（RTC + 音频框架 +
 *       MEMS MIC + LittleFS + cJSON + LVGL）。移植进 openvela 工程时编译它。
 *
 * 由构建系统决定链接哪一个 backend（Makefile 里 `make app` 链 sim；openvela 的
 * CMakeLists.txt 链 vela）。两者实现同一套 my_hal_* 符号。
 *
 * ====================== 音频数据流（与核心层对接） ======================
 *
 *   my_noise_render() 合成噪声 PCM ──▶ my_fade_apply() 施加音量增益
 *        ──▶ my_hal_audio_play() 推到 DAC（真机）/ 丢弃（模拟器）
 *
 *   麦克风反向：my_hal_mic_read() 拉取已采到的 PCM ──▶ my_sleep_feed_mic() 做入睡判定
 *   呼吸引导灯：my_breathe_level_pct() 算亮度 ──▶ my_hal_light_set() 上屏
 *
 * 存储：my_sched_store_t / my_mem_store_t 由 backend 提供（真机=cJSON+LittleFS，
 *       模拟器=内存 buffer），核心层通过函数指针注入，不 #include cJSON/LittleFS。
 */
#ifndef MIANYU_HAL_H
#define MIANYU_HAL_H

#include "mianyu_common.h"
#include "mianyu_schedule.h"
#include "mianyu_sleep_memory.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== 生命周期 ===================== */

/* 平台初始化（开外设/挂载文件系统/初始化 RTC 等）。返回 MY_OK/MY_ERR_*。 */
my_err_t my_hal_init(void);

/* 平台反初始化（停机前释放资源）。 */
void my_hal_deinit(void);

/* 阻塞睡眠 ms 毫秒（主循环节拍用；真机可让出 CPU，模拟器 usleep）。 */
void my_hal_sleep_ms(int ms);

/* ===================== 时间 ===================== */

/* 读取当前本地时间到 dt（真机=RTC/网络授时；模拟器=虚拟时钟）。 */
my_err_t my_hal_time_now(my_datetime_t *dt);

/* ===================== 音频输出 ===================== */

/* 播放一段 PCM（16k/16bit/mono）。实现须【非阻塞或短阻塞】：真机写进 DMA
 * 环形缓冲即返回；模拟器丢弃。返回处理的样本数，<0 为错误。 */
int my_hal_audio_play(const my_pcm_t *buf, int count);

/* 停止播放并清空缓冲（入睡淡出归零后调用，避免残留尾音）。 */
void my_hal_audio_stop(void);

/* ===================== 麦克风输入 ===================== */

/* 从 MIC 拉取已采集的 PCM（非阻塞），写入 buf[0..max_count)，返回实际样本数。
 * 无新数据返回 0；未启用/不可用返回负错误码。真机从音频框架的输入环形缓冲拷贝。 */
int my_hal_mic_read(my_pcm_t *buf, int max_count);

/* ===================== 体动（IMU，可选） ===================== */

/* 平台是否有可用 IMU（DevKit-LCD 无板载 IMU，恒 false；黄山派有）。 */
bool my_hal_imu_available(void);

/* 读一帧三轴加速度（单位 g）。仅 imu_available 为 true 时调用。 */
my_err_t my_hal_imu_read(float *ax, float *ay, float *az);

/* ===================== 呼吸引导灯 ===================== */

/* 把亮度百分比 0..100 上屏（真机=LVGL 呼吸光晕；模拟器=日志/丢弃）。 */
void my_hal_light_set(int level_pct);

/* ============= 语音链路：判定上行 / 光引导下行 =============
 *
 * 位置感：这两个接口是"核心（Agent 对话）"与"两个附加"之间的接线柱。
 *
 *   附加 1（输入）：入睡判定 ── my_hal_sleep_report() ──▶ PC 侧 Agent
 *      判定的产物不是给人看的报告，是给 Agent 的一个信号：
 *      "他现在是清醒/困倦/睡着了"，Agent 据此决定要不要换策略。
 *
 *   附加 2（输出）：PC 侧 Agent ── my_hal_remote_cmd_take() ──▶ 光/呼吸节拍
 *      Agent 说了算的是"光怎么带呼吸"，板子只负责把参数落地。
 *
 * 两侧都做成【非阻塞 + 整型参数】：真机上是跨线程（主循环 ↔ 串口线程），
 * 模拟器上是空实现，只有 int 在两边走，不共享对象。
 */

/* 下行指令的种类。参数统一放 a/b/c/d 四个 int，够表达、又能安全跨线程传。 */
typedef enum {
    MY_RCMD_NONE = 0,       /* 没取到（仅内部用） */
    MY_RCMD_SET_BREATHE,    /* 改呼吸节拍：a=吸ms b=屏ms c=呼ms d=峰值% */
    MY_RCMD_SET_HALO,        /* 只改光晕亮度：a=亮度% */
    MY_RCMD_LIGHT_ONOFF,     /* 开/关灯：a=0/1 */
} my_rcmd_kind_t;

typedef struct {
    my_rcmd_kind_t kind;
    int            a, b, c, d;
} my_remote_cmd_t;

/* 把入睡判定结果上报给 PC 侧 Agent。
 * state: 0=清醒 1=困倦 2=已入睡；conf_pct: 置信度 0..100；resp_bpm: 呼吸率(次/分)。 */
void my_hal_sleep_report(int state, int conf_pct, int resp_bpm);

/* 取一条 PC 侧 Agent 发来的下行指令（非阻塞）。取到返回 true，无则 false。 */
bool my_hal_remote_cmd_take(my_remote_cmd_t *out);

/* ===================== 存储后端（注入给核心层） ===================== */

/* 调度任务表后端：真机=cJSON+LittleFS，模拟器=内存。调用方用 my_sched_init 注入。 */
const my_sched_store_t *my_hal_sched_store(void);

/* 睡眠档案后端：真机=cJSON+LittleFS，模拟器=内存。调用方用 my_mem_init 注入。 */
const my_mem_store_t *my_hal_mem_store(void);

#ifdef __cplusplus
}
#endif
#endif /* MIANYU_HAL_H */
