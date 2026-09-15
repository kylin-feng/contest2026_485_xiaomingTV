/* SPDX-License-Identifier: Apache-2.0
 *
 * 眠语 (MianYu) - 会聊天的哄睡设备
 * 公共定义：采样格式、错误码、平台无关基础类型
 *
 * 定位：核心是 Agent 语音对话；本层是它的两个端 ——
 *   附加 1 入睡判定（给 Agent 输入）与附加 2 呼吸光引导（做输出通道）。
 *   推导见 docs/以对话为核心的架构.md。
 *
 * 设计约束（见技术报告 4.5 资源预算）：
 *   - 音频统一 16kHz / 16bit / mono PCM，避开 MP3 编解码风险
 *     （openvela FAQ 明示音频框架在编解码环节易 crash）
 *   - 核心算法层不得包含任何平台/RTOS 头文件，保证可在 PC 上单元测试
 */
#ifndef MIANYU_COMMON_H
#define MIANYU_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 音频格式常量 ---- */
#define MY_SAMPLE_RATE      16000   /* Hz */
#define MY_SAMPLE_BITS      16
#define MY_CHANNELS         1
typedef int16_t my_pcm_t;           /* 单声道 16bit PCM 样本 */

#define MY_PCM_MAX          32767
#define MY_PCM_MIN          (-32768)

/* ---- 通用错误码 ---- */
typedef enum {
    MY_OK               =  0,
    MY_ERR_PARAM        = -1,   /* 非法参数 */
    MY_ERR_NOMEM        = -2,   /* 内存/缓冲不足 */
    MY_ERR_STATE        = -3,   /* 状态机不允许该操作 */
    MY_ERR_IO           = -4,   /* 存储/外设 IO 失败 */
    MY_ERR_UNSUPPORTED  = -5,   /* 当前 HAL 后端不支持 */
    MY_ERR_TIMEOUT      = -6,
} my_err_t;

/* ---- 编译期断言（C11 起有 _Static_assert，否则退化为负数组技巧） ----
 * msg 用 # 字符串化，两个分支都可传裸标识符 */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  define MY_STATIC_ASSERT(cond, msg)  _Static_assert(cond, #msg)
#else
#  define MY_STATIC_ASSERT(cond, msg) \
     typedef char my_sa_##msg[(cond) ? 1 : -1]
#endif

/* ---- 常用数学小工具（避免各处重复 include math.h 的开销判断） ---- */
#ifndef MY_MIN
#  define MY_MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MY_MAX
#  define MY_MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef MY_CLAMP
#  define MY_CLAMP(x, lo, hi) (MY_MIN(MY_MAX((x), (lo)), (hi)))
#endif

/* ---- 数学常量 ----
 * M_PI 不是 C 标准库的一部分：glibc/macOS 默认有，MinGW/嵌入式 libc 默认无。
 * 核心层要真正做到「平台无关」，就在这里统一提供，避免各文件各写一个 guard。 */
#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

#ifdef __cplusplus
}
#endif
#endif /* MIANYU_COMMON_H */
