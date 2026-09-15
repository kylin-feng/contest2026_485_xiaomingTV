/* SPDX-License-Identifier: Apache-2.0
 *
 * bsp_voice_link.c — 板级串口语音链路端点（对齐 PC 侧 voice/board_link.py）
 *
 * 为什么走串口
 * ============
 * 本板**没有 WiFi**（原理图无 RF、无天线），只有 BLE。而板上现成的 Type-C ①号口
 * 就是 CH343 串口（也是烧录口）。1 Mbaud 下 16kHz/16bit/mono 的 PCM 只要
 * 32KB/s，占约 32% 带宽，语音对话又是半双工的（要么在听要么在说），完全够。
 * 语音识别/合成这些重活放 PC（网关），板子只做"耳朵 + 嘴 + 按键 + 灯"。
 *
 * 帧格式（与 board_link.py 逐字节一致，改一边必须改另一边）
 * ---------------------------------------------------------
 *   A5 5A | type | flags | seq | len(u16 小端) | payload(len 字节) | crc16(u16 小端)
 *   crc16 = CRC16-CCITT-FALSE(poly 0x1021, init 0xFFFF, 不反转, 不异或输出)
 *   覆盖范围 = type/flags/seq/len/payload（不含同步头和 crc 自身）
 *
 * 这个格式是给"半双工 + 会丢字节"的链路上用的：
 *   · 2 字节同步头 + 尾部 CRC → 上位机能自己找回帧边界，不假设对齐；
 *   · seq 单调递增 → 上位机能算丢帧率（语音能容忍丢帧，但不能不知道丢了多少）；
 *   · 板子复位、USB 重枚举之后，上位机不需要重连协议，重新同步就行。
 *
 * 为什么控制台上会有文本日志混进二进制流
 * --------------------------------------
 * 本板只有一个能通到 USB 的串口（UART1，即 /dev/console），syslog 也走它。
 * 所以板→PC 方向是"文本日志 + 二进制帧"混流。这不需要额外处理：上位机的
 * FrameParser 本来就在流里找 A5 5A 并用 CRC 验证，日志文本会被当作噪声跳掉。
 * 反过来 PC→板 方向是纯二进制，所以板子的接收端不解析文本。
 *
 * 【已知约束 / 下一步】
 *   本板上 nsh 也挂在 /dev/console 上当交互控制台，它会读走 PC 发下来的字节。
 *   所以"下行 PCM（PC 的 TTS 送去喇叭）"这条路在 nsh 仍在控制台时是不完整的：
 *   上行（麦克风 → PC）不受影响，下行需要先把 nsh 从控制台挪走
 *   （改 defconfig 的 CONFIG_INIT_ENTRYPOINT，或把控制台换到 UART2）。
 *   代码这里两种情形都写好了，nsh 一让开就整条通。
 */

#include <nuttx/config.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/sched.h>

#include "bsp_board.h"

/* ------------------------------------------------------------------------- */
/* 协议常量（必须与 voice/board_link.py 保持一致）                            */
/* ------------------------------------------------------------------------- */

#define VL_SYNC0            (0xA5)
#define VL_SYNC1            (0x5A)

#define VL_T_AUDIO_UP       (0x01)   /* 板->PC  MIC PCM 16k/16bit/mono */
#define VL_T_AUDIO_DOWN     (0x02)   /* PC->板  喇叭 PCM */
#define VL_T_EVENT          (0x03)   /* 板->PC  事件 */
#define VL_T_CMD            (0x04)   /* PC->板  命令 */
#define VL_T_PING           (0x05)   /* 双向保活（板->PC 是 PONG） */
#define VL_T_LOG            (0x06)   /* 板->PC  调试文本 */

#define VL_CMD_SET_AMP      (0x01)   /* payload[1] = 0/1 */
#define VL_CMD_SET_VOL      (0x02)   /* payload[1] = 0..100 */
#define VL_CMD_PLAY_STOP    (0x03)   /* 停播 + 清空下行缓冲 */
#define VL_CMD_SET_MIC      (0x04)   /* payload[1] = 0/1 上行开关 */
#define VL_CMD_TONE         (0x05)   /* payload[1] = 提示音编号 */
#define VL_CMD_SET_DOWNLINK (0x06)   /* payload[1] = 0/1 下行接收开关（省电/让路） */
#define VL_CMD_LOOPBACK     (0x07)   /* payload[1] = 0/1 声学回环（0=功放关作对照） */

/* 板子启动后没有交互控制台（INIT_ENTRYPOINT = mianyu_main，不是 nsh_main），
 * 否则 nsh 会和语音链路抢 /dev/console，把 PC 发下来的 PCM 字节吃掉
 * —— 实测下行成功率只有 1%。但调试时还是想要个 shell，所以留这个口子：
 * 发这个命令，链路就会把控制台交还给 nsh（之后链路只发不收）。 */
#define VL_CMD_SHELL        (0x10)

#define VL_EV_KEY_DOWN      (0x01)
#define VL_EV_KEY_UP        (0x02)
#define VL_EV_BOOT_READY    (0x03)

#define VL_MAX_PAYLOAD      (1024)   /* 与 board_link.py 的 MAX_PAYLOAD 一致 */
#define VL_HEAD_LEN         (5)      /* type(1)+flags(1)+seq(1)+len(2) */

/* 上行一块 = 256 样本 = 16ms @16k。
 * 取 16ms 是折中：更小块会让帧头开销占比上升（每帧 9 字节固定开销），
 * 更大块会推高"用户打断"时 PC 侧还要多收多久才停。 */
#define VL_UP_SAMPLES       (256)
#define VL_UP_BYTES         (VL_UP_SAMPLES * 2)

/* 串口：UART1 是唯一通到 USB 的口（= /dev/console = 烧录口 COM6）。
 * 波特率必须和 CONFIG_UART_BAUD 一致（当前 1000000），
 * PC 侧 board_link.py 默认写的是 1500000，连的时候要显式给 1000000。 */
#define VL_UART_DEV         "/dev/console"
#define VL_RXBUF_SIZE       (4096)

static int               g_fd = -1;
static volatile int      g_mic_up = 1;      /* 上行开关（CMD_SET_MIC 控） */
static volatile int      g_down_on = 1;     /* 下行接收开关（CMD_SET_DOWNLINK 控） */
static uint8_t           g_tx_seq;
static volatile uint32_t g_up_frames;       /* 上行帧数（诊断） */
static volatile uint32_t g_down_frames;     /* 下行帧数（诊断） */
static uint32_t          g_crc_err;         /* CRC 错计数 */
static uint32_t          g_resync;          /* 重新同步次数 */
static uint32_t          g_tx_bytes;        /* 实际写出去的字节数 */
static uint32_t          g_tx_short;        /* 没写完整的帧数（丢帧证据） */
static uint32_t          g_down_drop;       /* 下行因缓冲满丢弃的样本数 */

/* 64 点 Q15 正弦表（本地提示音合成用；不共用 bsp_audio_test.c 里那份，
 * 避免两个文件互相依赖） */
static const int16_t g_vl_sin64[64] =
{
       0,   3212,   6393,   9512,  12539,  15446,  18204,  20787,
   23170,  25329,  27245,  28898,  30273,  31356,  32137,  32609,
   32767,  32609,  32137,  31356,  30273,  28898,  27245,  25329,
   23170,  20787,  18204,  15446,  12539,   9512,   6393,   3212,
       0,  -3212,  -6393,  -9512, -12539, -15446, -18204, -20787,
  -23170, -25329, -27245, -28898, -30273, -31356, -32137, -32609,
  -32767, -32609, -32137, -31356, -30273, -28898, -27245, -25329,
  -23170, -20787, -18204, -15446, -12539,  -9512,  -6393,  -3212,
};

/* ------------------------------------------------------------------------- */
/* 板载音频接口（实现在 bsp_audio_test.c）                                    */
/* ------------------------------------------------------------------------- */

extern int  bsp_audio_hw_init(void);
extern int  bsp_audio_ready(void);
extern void bsp_audio_dump(const char *tag);
extern int  bsp_audio_play_pcm(const int16_t *pcm, int count);
extern void bsp_audio_play_clear(void);
extern int  bsp_audio_play_pending(void);
extern void bsp_audio_set_amp(int on);
extern void bsp_audio_set_volume_pct(int pct);
extern int  bsp_audio_mic_read(int16_t *dst, int max);
extern void bsp_audio_mic_stats(uint32_t *ht, uint32_t *tc);
extern int  bsp_audio_loopback(int pa_on, int nwin,
                               uint32_t *off_1k, uint32_t *on_1k,
                               uint32_t *off_ref, uint32_t *on_ref,
                               uint32_t *mic_peak, uint32_t *mic_rms);

/* ------------------------------------------------------------------------- */

static void vllog(const char *fmt, ...)
{
  char msg[160];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  syslog(LOG_INFO, "[vlink] %s\n", msg);
}

/* ------------------------------------------------------------------------- */
/* CRC16-CCITT-FALSE                                                          */
/* ------------------------------------------------------------------------- */

/* 用查表法：2048 个样本一帧、16ms 一帧，如果按位算 CRC 每帧要多花上千个周期，
 * 在中断/高优先级任务里不划算。256 项的 nibble 表就够（一字节两次查表）。 */
static const uint16_t g_crc_nibble[16] =
{
  0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
  0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF,
};

static uint16_t vl_crc16(const uint8_t *d, int n)
{
  uint16_t crc = 0xFFFF;
  int      i;

  for (i = 0; i < n; i++)
    {
      crc = (uint16_t)((crc << 4) ^ g_crc_nibble[((crc >> 12) ^ (d[i] >> 4)) & 0x0F]);
      crc = (uint16_t)((crc << 4) ^ g_crc_nibble[((crc >> 12) ^ (d[i] & 0x0F)) & 0x0F]);
    }

  return crc;
}

/* ------------------------------------------------------------------------- */
/* 发送                                                                       */
/* ------------------------------------------------------------------------- */

/* 组帧到调用方给的缓冲，返回整帧长度。
 *
 * 拆出来单独一个函数，是为了让自检能拿"组出来的字节"和 board_link.py 的
 * build_frame() 输出逐字节对比 —— 协议对不对不能靠"看起来像"，
 * 得有一份从 Python 侧带过来的黄金向量。
 * 用 seq 参数而不是直接取 g_tx_seq，让自检能用固定 seq 做定值对比。 */
static int vl_build(uint8_t *frame, uint8_t type, uint8_t flags, uint8_t seq,
                    const uint8_t *payload, int len)
{
  uint16_t crc;
  int      total;

  if (len > VL_MAX_PAYLOAD)
    {
      len = VL_MAX_PAYLOAD;
    }

  frame[0] = VL_SYNC0;
  frame[1] = VL_SYNC1;
  frame[2] = type;
  frame[3] = flags;
  frame[4] = seq;
  frame[5] = (uint8_t)(len & 0xFF);
  frame[6] = (uint8_t)((len >> 8) & 0xFF);

  if (len > 0 && payload != NULL)
    {
      memcpy(&frame[7], payload, (size_t)len);
    }

  crc   = vl_crc16(&frame[2], VL_HEAD_LEN + len);
  total = 2 + VL_HEAD_LEN + len;

  frame[total++] = (uint8_t)(crc & 0xFF);
  frame[total++] = (uint8_t)((crc >> 8) & 0xFF);

  return total;
}

/* 把整帧写出去。
 *
 * 【为什么必须"写不完就等"】UART 发送缓冲只有 1024 字节（CONFIG_UART_BUFSZ），
 * 一发上行帧是 512 字节 payload + 9 字节头尾 = 521 字节。缓冲偶尔只剩几十字节
 * 可用，此时非阻塞 write() 返回 -EAGAIN。如果就这么放弃，**整帧会被静默丢掉**
 * —— 而且丢得很"干净"：PC 侧一个 CRC 错都看不到（因为一个字节都没发出去），
 * 只表现为"声音莫名其妙少了约五分之一"。实测丢帧率 ~18%，就是这么来的。
 * 所以这里改成：EAGAIN/EINTR 就歇 1ms 重试，直到写完（上限 60ms，
 * 真超了说明串口出问题了，记一笔让上层知道）。 */
static int vl_write_all(const uint8_t *p, int n)
{
  int off     = 0;
  int spins   = 0;

  while (off < n)
    {
      int w = (int)write(g_fd, &p[off], (size_t)(n - off));

      if (w > 0)
        {
          off   += w;
          spins  = 0;
          continue;
        }

      if (w < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
        {
          if (++spins > 60)
            {
              break;               /* 60ms 还没排空：串口大概率死了 */
            }

          usleep(1000);
          continue;
        }

      break;                       /* 真错误，不重试 */
    }

  if (off < n)
    {
      g_tx_short++;
    }

  g_tx_bytes += (uint32_t)off;
  return off;
}

/* 组帧 + 写出。串口 xmit 缓冲是 1024 字节（CONFIG_UART_BUFSZ），
 * 所以上行单帧控制在 512 字节 payload 以内。 */
static void vl_send(uint8_t type, const uint8_t *payload, int len, uint8_t flags)
{
  uint8_t frame[2 + VL_HEAD_LEN + VL_MAX_PAYLOAD + 2];
  int     total;

  if (g_fd < 0)
    {
      return;
    }

  total = vl_build(frame, type, flags, g_tx_seq++, payload, len);
  vl_write_all(frame, total);
}

static void vl_send_log(const char *text)
{
  vl_send(VL_T_LOG, (const uint8_t *)text, (int)strlen(text), 0);
}

/* ------------------------------------------------------------------------- */
/* 本地提示音（合成后塞进放音环形缓冲，不阻塞链路）                            */
/* ------------------------------------------------------------------------- */

static void vl_play_tone(uint32_t freq, uint32_t ms)
{
  /* 一次最多合成 8 秒，防手滑传进来一个大数把栈/环撑爆 */
  int16_t  buf[256];
  uint32_t step = (uint32_t)(((uint64_t)freq << 16) / 16000u);
  uint32_t ph   = 0;
  uint32_t left = (ms * 16000u) / 1000u;
  int      i;

  if (left > 8u * 16000u)
    {
      left = 8u * 16000u;
    }

  while (left > 0)
    {
      int chunk = (left > 256u) ? 256 : (int)left;

      for (i = 0; i < chunk; i++)
        {
          buf[i] = (int16_t)(((int32_t)g_vl_sin64[(ph >> 10) & 63] * 8192) >> 15);
          ph += step;
        }

      bsp_audio_play_pcm(buf, chunk);
      left -= (uint32_t)chunk;
    }
}

/* ------------------------------------------------------------------------- */
/* 接收：增量帧解析                                                           */
/* ------------------------------------------------------------------------- */

static uint8_t  g_rx[VL_RXBUF_SIZE];
static int      g_rx_len;

/* 自检用：置 1 时 vl_on_frame 只记录、不真的去操作硬件/回包，
 * 这样自检可以拿真实解析器跑真实字节流，而不会真去开关功放。 */
static int      g_test_mode;
static int      g_test_frames;
static uint8_t  g_test_type[8];
static int      g_test_len[8];

/* ------------------------------------------------------------------------- */
/* 串口控制台归属                                                             */
/* ------------------------------------------------------------------------- */

/* 产品模式下板子**没有**交互控制台：init 入口是 mianyu_main()（不是 nsh_main）。
 *
 * 为什么必须这样：板上只有一个能通到 USB 的串口（UART1 = /dev/console），
 * 而 nsh 默认会在这个口上阻塞地 read() 等命令。PC 发下来的下行 PCM 字节
 * 会被 nsh 抢走 —— 实测过：100 个 PING 只回了 1 个，下行成功率 1%，
 * 而且 nsh 还会把它对二进制垃圾的回应打进上行流里，把上行也搅乱
 * （CRC 错 41 次 / 重同步 275 次）。
 *
 * 但调试的时候还是得有个能敲命令的 shell，所以留这个运行时开关：
 * PC 发 VL_CMD_SHELL，链路就把控制台交还给 nsh。
 * 交还之后下行 PCM 就不能用了（同一个口，只能有一个读者），这是有意取舍。 */
static volatile int g_shell_mode;

static void vl_shell_thread(void)
{
  extern int nsh_consolemain(int argc, char *argv[]);
  char *argv[2];

  argv[0] = (char *)"nsh";
  argv[1] = NULL;
  (void)nsh_consolemain(1, argv);
}

static void vl_hand_over_to_shell(void)
{
  int pid;

  if (g_shell_mode)
    {
      vllog("已经是 shell 模式了（复位板子才能回到纯语音模式）");
      return;
    }

  pid = task_create("nsh_vlink", SCHED_PRIORITY_DEFAULT, 4096,
                    (main_t)vl_shell_thread, NULL);
  if (pid < 0)
    {
      vllog("交还控制台失败: %d", errno);
      return;
    }

  g_shell_mode = 1;
  vllog("控制台已交还 nsh（pid=%d）。现在可以敲命令；"
        "上行音频仍然在发，但下行 PCM 会被 nsh 吃掉。复位板子回到纯语音模式。",
        pid);
}

/* 处理一条完整的帧。payload 已通过 CRC。 */
static void vl_on_frame(uint8_t type, uint8_t flags, uint8_t seq,
                        const uint8_t *pl, int len)
{
  (void)flags;

  if (g_test_mode)
    {
      if (g_test_frames < 8)
        {
          g_test_type[g_test_frames] = type;
          g_test_len[g_test_frames]  = len;
        }

      g_test_frames++;
      return;
    }

  (void)seq;

  switch (type)
    {
      case VL_T_AUDIO_DOWN:
        {
          /* 下行 PCM：直接推给放音环形缓冲。
           * 按样本对齐（16bit），奇数字节说明帧被截过，丢掉尾巴。 */
          int samples = (len / 2) * 2;

          if (samples > 0 && g_down_on)
            {
              int wrote = bsp_audio_play_pcm((const int16_t *)pl, samples / 2);

              g_down_frames++;

              if (wrote < samples / 2)
                {
                  /* 下发得比喇叭放得快，环形缓冲满了。丢掉是最稳的处理 ——
                   * 在这里等会把上行也拖住，语音就僵了。计数留证据，
                   * 攒够量再报一次，免得刷屏。 */
                  g_down_drop += (uint32_t)(samples / 2 - wrote);
                }
            }
        }
        break;

      case VL_T_CMD:
        {
          uint8_t cmd = (len > 0) ? pl[0] : 0;
          uint8_t arg = (len > 1) ? pl[1] : 0;

          switch (cmd)
            {
              case VL_CMD_SET_AMP:
                bsp_audio_set_amp(arg ? 1 : 0);
                vllog("CMD 功放 -> %s", arg ? "开" : "关");
                break;

              case VL_CMD_SET_VOL:
                bsp_audio_set_volume_pct((int)arg);
                vllog("CMD 音量 -> %d%%", (int)arg);
                break;

              case VL_CMD_PLAY_STOP:
                /* 用户打断 AI：立刻静音并丢掉还没放的 PCM */
                bsp_audio_play_clear();
                vllog("CMD 打断：已清空下行缓冲");
                break;

              case VL_CMD_SET_MIC:
                g_mic_up = arg ? 1 : 0;
                vllog("CMD 上行麦克风 -> %s", arg ? "开" : "关");
                break;

              case VL_CMD_TONE:
                {
                  /* 提示音本地合成，PC 不用把音频发下来。
                   * 编号 -> (频率, 时长)：1=开机 2=收到 3=打断 4=错误 */
                  static const uint32_t tone_freq[4] = { 880, 1320, 660, 330 };
                  static const uint32_t tone_ms[4]   = { 180,  90, 120, 300 };
                  uint8_t idx = (arg >= 1 && arg <= 4) ? (uint8_t)(arg - 1) : 0;

                  vl_play_tone(tone_freq[idx], tone_ms[idx]);
                  vllog("CMD 提示音 #%u", (unsigned)arg);
                }
                break;

              case VL_CMD_SET_DOWNLINK:
                g_down_on = arg ? 1 : 0;
                vllog("CMD 下行接收 -> %s", arg ? "开" : "关");
                break;

              case VL_CMD_LOOPBACK:
                {
                  /* 板上声学回环自检：本地放 1kHz，量麦克风频谱（多窗累加）。
                   * arg=1 功放开（真声学 + 串扰），arg=0 功放关（只有串扰）。
                   * 两遍一比就能判"喇叭到底有没有发声"。
                   * 注意：这一步要独占音频约 2s，期间链路不收发，
                   * 上位机会看到一段上行空洞 —— 这是预期的。 */
                  uint32_t o1k = 0, n1k = 0, oref = 0, nref = 0;
                  uint32_t pk = 0, rms = 0;
                  int      rc;
                  int      nw = 12;      /* 12 窗 x 64ms ≈ 0.8s/相位 */

                  vllog("CMD 声学回环：开始（功放%s）...", arg ? "开" : "关");
                  rc = bsp_audio_loopback(arg ? 1 : 0, nw, &o1k, &n1k,
                                          &oref, &nref, &pk, &rms);
                  if (rc != 0)
                    {
                      vllog("CMD 声学回环：音频通路没起来，跳过");
                    }
                  else
                    {
                      /* 打均值（累加值 / 窗数），PC 侧直接读这几个数 */
                      vllog("CMD 声学回环 功放%s 窗=%d：静音 1kHz=%u 邻近=%u | "
                            "放音 1kHz=%u 邻近=%u | 麦克风 峰值=%u RMS=%u",
                            arg ? "开" : "关", nw,
                            (unsigned)(o1k / (uint32_t)nw),
                            (unsigned)(oref / (uint32_t)nw),
                            (unsigned)(n1k / (uint32_t)nw),
                            (unsigned)(nref / (uint32_t)nw),
                            (unsigned)pk, (unsigned)rms);
                    }
                }
                break;

              case VL_CMD_SHELL:
                vl_hand_over_to_shell();
                break;

              default:
                vllog("CMD 未知命令 0x%02x（忽略）", (unsigned)cmd);
                break;
            }
        }
        break;

      case VL_T_PING:
        /* 收到 PING 就回 PONG（同类型空帧），上位机据此测往返和存活 */
        vl_send(VL_T_PING, NULL, 0, 0);
        break;

      case VL_T_AUDIO_UP:
      case VL_T_EVENT:
      case VL_T_LOG:
      default:
        /* PC 不该发这些给板子，静默忽略（不刷日志，免得被垃圾流打爆） */
        break;
    }
}

/* 喂进接收缓冲并尽可能多地取出帧。
 *
 * 边界恢复策略与 board_link.py 的 FrameParser 对称：
 *   找不到同步头   -> 只留最后 1 字节（可能是 A5 的一半）
 *   长度不合理     -> 认为这个同步头是假的，跳过 1 字节重找
 *   CRC 不对       -> 同上，只丢 1 字节，不做整帧丢弃（避免连带丢掉后面的好帧）
 * 之所以都"只退 1 字节"，是因为二进制流里 A5 5A 完全可能出现在 payload 里，
 * 整帧丢弃会把真帧也一起扔掉。 */
static void vl_feed(const uint8_t *data, int n)
{
  int i;

  if (n <= 0)
    {
      return;
    }

  if (g_rx_len + n > VL_RXBUF_SIZE)
    {
      /* 缓冲要满了：说明一直没对上帧边界，丢掉旧的一半重新来 */
      g_rx_len = 0;
      g_resync++;
    }

  memcpy(&g_rx[g_rx_len], data, (size_t)n);
  g_rx_len += n;

  i = 0;
  while (i < g_rx_len)
    {
      int j;

      /* 1) 找同步头 */
      for (j = i; j + 1 < g_rx_len; j++)
        {
          if (g_rx[j] == VL_SYNC0 && g_rx[j + 1] == VL_SYNC1)
            {
              break;
            }
        }

      if (j + 1 >= g_rx_len)
        {
          /* 没找到：保留最后一个字节（可能是半截同步头） */
          i = (g_rx_len > 0) ? g_rx_len - 1 : g_rx_len;
          break;
        }

      if (j > i)
        {
          g_resync++;
        }

      /* 2) 头够不够 */
      if (j + 2 + VL_HEAD_LEN + 2 > g_rx_len)
        {
          i = j;
          break;                 /* 帧还没收全，等更多字节 */
        }

      {
        const uint8_t *h = &g_rx[j + 2];
        uint16_t len   = (uint16_t)(h[3] | ((uint16_t)h[4] << 8));
        uint16_t want;
        uint16_t got;
        int      total;

        if (len > VL_MAX_PAYLOAD)
          {
            i = j + 1;           /* 长度不合理 -> 假的同步头 */
            g_resync++;
            continue;
          }

        total = 2 + VL_HEAD_LEN + (int)len + 2;
        if (j + total > g_rx_len)
          {
            i = j;
            break;               /* 帧还没收全 */
          }

        got  = (uint16_t)(g_rx[j + 2 + VL_HEAD_LEN + len] |
                          ((uint16_t)g_rx[j + 2 + VL_HEAD_LEN + len + 1] << 8));
        want = vl_crc16(h, VL_HEAD_LEN + (int)len);

        if (got != want)
          {
            i = j + 1;
            g_crc_err++;
            continue;
          }

        vl_on_frame(h[0], h[1], h[2], &h[VL_HEAD_LEN], (int)len);
        i = j + total;
      }
    }

  /* 把没用掉的尾巴搬到缓冲区开头 */
  if (i > 0)
    {
      int rest = g_rx_len - i;

      if (rest > 0)
        {
          memmove(g_rx, &g_rx[i], (size_t)rest);
        }

      g_rx_len = rest;
    }
}

/* ------------------------------------------------------------------------- */
/* 协议自检                                                                   */
/* ------------------------------------------------------------------------- */

/* 黄金向量全部由 PC 侧 voice/board_link.py 现场算出来贴在源码里。
 * 以后谁改了协议（同步头/字段顺序/CRC 初值或多项式），这里会先炸，
 * 而不是等到"板子连上去收不到帧"才发现。 */
static const uint8_t g_vl_selftest_stream[] =
{
  /* 前置噪声：上位机刚打开串口时抓到的半截日志 */
  0x00, 0xFF, 0x13,

  /* T_EVENT, seq=0, payload=[0x03]  ->  build_frame(T_EVENT, b"\x03", seq=0) */
  0xA5, 0x5A, 0x03, 0x00, 0x00, 0x01, 0x00, 0x03, 0xA3, 0xC7,

  /* T_CMD, seq=0x11, payload=[SET_VOL=0x02, 50] -> build_frame(T_CMD, b"\x02\x32", seq=0x11) */
  0xA5, 0x5A, 0x04, 0x00, 0x11, 0x02, 0x00, 0x02, 0x32, 0x18, 0x03,

  /* 故意插一个反向同步头 + 单字节，验证"只退 1 字节"的边界恢复 */
  0x5A, 0xA5, 0x00,

  /* T_AUDIO_UP, seq=0x42, payload=[AA 55 11 22] */
  0xA5, 0x5A, 0x01, 0x00, 0x42, 0x04, 0x00, 0xAA, 0x55, 0x11, 0x22, 0x7E, 0x4B,
};

static int vl_selftest(void)
{
  int fail = 0;

  /* ---- 1) CRC16-CCITT-FALSE 定值 ---- */
  {
    static const struct
    {
      const char *data;
      int         n;
      uint16_t    want;
    } vec[4] =
    {
      { "123456789",              9, 0x29B1 },   /* 标准 check 值 */
      { "",                       0, 0xFFFF },   /* 空输入 = init */
      { "A",                      1, 0xB915 },
      { "\x00\x01\x02\x03",       4, 0xE5F1 },
    };

    int i;

    for (i = 0; i < 4; i++)
      {
        uint16_t got = vl_crc16((const uint8_t *)vec[i].data, vec[i].n);

        if (got != vec[i].want)
          {
            fail++;
            vllog("自检失败：CRC[%d] 得到 %04x，期望 %04x（与 board_link.py 不一致！）",
                  i, (unsigned)got, (unsigned)vec[i].want);
          }
      }

    vllog("自检 CRC16-CCITT-FALSE：4 组向量 %s", fail ? "不通过" : "全部吻合");
  }

  /* ---- 2) 组帧字节序（拿 vl_build 去对 board_link.py 的黄金帧） ---- */
  {
    static const uint8_t evp[1] = { 0x03 };
    uint8_t frame[64];
    int     n = vl_build(frame, VL_T_EVENT, 0, 0, evp, 1);

    if (n != 10 || memcmp(frame, &g_vl_selftest_stream[3], 10) != 0)
      {
        fail++;
        vllog("自检失败：T_EVENT 帧字节不一致（得到 %d 字节）", n);
      }
    else
      {
        vllog("自检组帧：T_EVENT -> A5 5A 03 00 00 01 00 03 A3 C7 与 PC 侧一致");
      }
  }

  /* ---- 3) 解析器跑真实字节流（含噪声 + 反向同步头） ---- */
  {
    int i;
    int want_type[3] = { VL_T_EVENT, VL_T_CMD, VL_T_AUDIO_UP };
    int want_len[3]  = { 1, 2, 4 };

    /* 3a) 一次性喂 */
    g_test_mode   = 1;
    g_test_frames = 0;
    g_rx_len      = 0;
    vl_feed(g_vl_selftest_stream, (int)sizeof(g_vl_selftest_stream));

    if (g_test_frames != 3)
      {
        fail++;
        vllog("自检失败：整块喂入解析到 %d 帧，期望 3", g_test_frames);
      }
    else
      {
        for (i = 0; i < 3; i++)
          {
            if (g_test_type[i] != want_type[i] || g_test_len[i] != want_len[i])
              {
                fail++;
                vllog("自检失败：第 %d 帧 type=%02x len=%d，期望 type=%02x len=%d",
                      i, (unsigned)g_test_type[i], g_test_len[i],
                      (unsigned)want_type[i], want_len[i]);
              }
          }
      }

    /* 3b) 逐字节喂 —— 串口就是一次给一个字节的，这条必须过 */
    g_test_frames = 0;
    g_rx_len      = 0;
    for (i = 0; i < (int)sizeof(g_vl_selftest_stream); i++)
      {
        vl_feed(&g_vl_selftest_stream[i], 1);
      }

    if (g_test_frames != 3)
      {
        fail++;
        vllog("自检失败：逐字节喂入解析到 %d 帧，期望 3", g_test_frames);
      }

    /* 3c) CRC 坏掉时不许崩、也不许把后面的好帧连坐丢掉 */
    {
      uint8_t bad[32];
      int     badn;

      memcpy(bad, &g_vl_selftest_stream[3], 10);
      bad[7] ^= 0xFF;                      /* 打坏 payload，CRC 必然不匹配 */

      g_test_frames = 0;
      g_rx_len      = 0;
      vl_feed(bad, 10);
      badn = g_test_frames;

      /* 后面紧跟一个好帧，应该还能被认出来 */
      vl_feed(&g_vl_selftest_stream[3], 10);

      if (badn != 0 || g_test_frames != 1)
        {
          fail++;
          vllog("自检失败：坏帧处理异常（坏帧被认成 %d 个，其后好帧只认出 %d 个）",
                badn, g_test_frames);
        }
      else
        {
          vllog("自检容错：坏 CRC 帧被丢掉，后随好帧仍能解析");
        }
    }

    g_test_mode = 0;
    g_rx_len    = 0;

    if (!fail)
      {
        vllog("自检解析器：整块喂 3/3 帧、逐字节喂 3/3 帧、坏帧容错 —— 全部通过");
      }
  }

  if (fail)
    {
      vllog("!!! 协议自检有 %d 项不通过：链路不可信，先别拿它跑语音 !!!", fail);
      return -1;
    }

  vllog("协议自检全部通过（与 voice/board_link.py 逐字节一致）");
  return 0;
}

/* ------------------------------------------------------------------------- */
/* 打开串口                                                                   */
/* ------------------------------------------------------------------------- */

static int vl_uart_open(void)
{
  int fd;

  /* O_NONBLOCK 让 read() 立刻返回，链路循环不被串口挂住 —— 上行还有 16ms
   * 的节拍要守，不能被"等 PC 说话"阻塞。 */
  fd = open(VL_UART_DEV, O_RDWR | O_NONBLOCK);
  if (fd < 0)
    {
      vllog("打开 %s 失败: %d（上行/下行都不可用）", VL_UART_DEV, errno);
      return -1;
    }

  return fd;
}

/* ------------------------------------------------------------------------- */
/* 链路任务                                                                   */
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* 上行泵：从采集环形缓冲取样本，凑满一帧就发，能发多少发多少                    */
/* ------------------------------------------------------------------------- */

/* 返回本次发出去的帧数。
 *
 * 关键点是 fill 由调用方跨循环保持（见 vl_thread 的注释）：
 * 采集侧按 1024 样本一跳推进，两次跳之间取不到东西。只有把半帧攒下来，
 * 才能做到"产出多少就发出去多少"，而不是每次凑不满就把样本扔掉。
 *
 * 用 for(;;) 一次把当前能取到的全部发完，是因为系统 tick 是 10ms，
 * 一个 tick 里可能已经攒了不止 16ms 的音频。 */
static int vl_uplink_pump(int16_t *buf, int *fill, int enable)
{
  int sent = 0;

  if (!enable)
    {
      *fill = 0;
      return 0;
    }

  for (;;)
    {
      /* 先把累积缓冲填到满一帧 */
      while (*fill < VL_UP_SAMPLES)
        {
          int n = bsp_audio_mic_read(&buf[*fill], VL_UP_SAMPLES - *fill);

          if (n <= 0)
            {
              break;
            }

          *fill += n;
        }

      if (*fill < VL_UP_SAMPLES)
        {
          break;                 /* 还没攒够，等下一个 tick */
        }

      vl_send(VL_T_AUDIO_UP, (const uint8_t *)buf, VL_UP_SAMPLES * 2, 0);
      g_up_frames++;
      sent++;
      *fill = 0;
    }

  return sent;
}

static void vl_thread(void)
{
  /* 上行累积缓冲必须是**跨循环保持**的。为什么要累积而不是"读一次就发"：
   * 采集侧是按 DMA 半传输推进的，每 1024 个样本才跳一次计数，两次跳之间
   * "可用样本数"是 0。如果每次循环读多少发多少、不足一帧就丢掉，
   * 那么每 64ms 里只有前几次能凑够一帧，后面的样本全被扔掉 —— 声音听起来
   * 就是"每隔一会儿断一下"。累积起来就不会丢。 */
  static int16_t up[VL_UP_SAMPLES];
  static int     up_fill;

  uint8_t        rx[512];
  uint32_t       last_stat = 0;
  uint32_t       last_up_ht = 0, last_up_tc = 0;
  int            i;

  /* 先自检协议，再等音频。协议不对的话后面全是白跑
   * （甚至更糟：看起来在跑，其实两边 CRC 算法不同，一个字节都收不下）。 */
  if (vl_selftest() != 0)
    {
      vllog("协议自检失败：为避免收到一堆 CRC 错的垃圾，链路不启动");
      return;
    }

  /* 等音频通路初始化完（bsp_audio_test.c 的线程先跑）。
   * 不等的话第一帧上行会拿到空数据，PC 侧看着像"板子没声音"。 */
  for (i = 0; i < 300 && !bsp_audio_ready(); i++)
    {
      usleep(10 * 1000);
    }

  if (!bsp_audio_ready())
    {
      /* 音频没起来也要把链路立起来：至少保活/命令还能用，方便上层诊断 */
      vllog("警告：音频通路未就绪，先只跑链路");
    }

  g_fd = vl_uart_open();
  if (g_fd < 0)
    {
      return;
    }

  vllog("语音链路已建立：%s，上行 %d 样本/帧，协议 A5 5A + CRC16-CCITT",
        VL_UART_DEV, VL_UP_SAMPLES);

  /* 开机事件 + 一句日志，让上位机一眼确认"板子活了、版本对了" */
  {
    uint8_t ev = VL_EV_BOOT_READY;

    vl_send(VL_T_EVENT, &ev, 1, 0);
    vl_send_log("board ready: SF32LB52-DevKit-LCD, 16kHz/16bit/mono, DAC+ADC running");
    vl_send_log("无 nsh 控制台（init=mianyu_main）。要 shell 请发 CMD 0x10。");
  }

  for (;;)
    {
      /* ---- 1) 收 PC 下来的东西 ----
       * shell 模式下不读：字节要让给 nsh，谁读谁有，抢着读两边都残。 */
      if (!g_shell_mode)
        {
          for (;;)
            {
              ssize_t n = read(g_fd, rx, sizeof(rx));

              if (n > 0)
                {
                  vl_feed(rx, (int)n);
                }
              else
                {
                  break;         /* 没数据（非阻塞）或出错，回去看上行 */
                }
            }
        }

      /* ---- 2) 上行：把这一个 tick 里攒下的整帧全部发出去 ---- */
      vl_uplink_pump(up, &up_fill, g_mic_up);

      /* ---- 3) 周期性状态（500ms 一次）：采到的量 + 链路健康度 ---- */
      {
        uint32_t now = (uint32_t)clock_systime_ticks();
        uint32_t ht  = 0, tc = 0;

        if ((now - last_stat) >= (uint32_t)(500 / (USEC_PER_TICK / 1000)))
          {
            last_stat = now;
            bsp_audio_mic_stats(&ht, &tc);

            /* 采集侧到底有没有在跑：HT/TC 不涨就说明 ADC 侧断了。
             * tx_short 是"没写完整的帧"数 —— 它一直涨就说明串口在丢。 */
            vllog("上行 %u 帧 下行 %u 帧 | 麦克风 HT=%u TC=%u(Δ%u) | 待放=%d 样本"
                  " | TX %u 字节 丢帧 %u | CRC错=%u 重同步=%u | cfg=%s%s",
                  (unsigned)g_up_frames, (unsigned)g_down_frames,
                  (unsigned)ht, (unsigned)tc,
                  (unsigned)(ht + tc - last_up_ht - last_up_tc),
                  bsp_audio_play_pending(),
                  (unsigned)g_tx_bytes, (unsigned)g_tx_short,
                  (unsigned)g_crc_err, (unsigned)g_resync,
                  g_mic_up ? "MIC-UP" : "MIC-OFF",
                  g_shell_mode ? " SHELL" : "");

            last_up_ht = ht;
            last_up_tc = tc;
          }
      }

      /* tick 是 10ms，睡 2ms 实际就会睡到下一个 tick，所以这里写 1ms
       * 表达意图即可（别指望亚 tick 的精度，见 bsp_audio_test.c 的说明）。 */
      usleep(1000);
    }
}

int bsp_voice_link_start(void)
{
  int pid;

  /* 优先级给高一点：上行有 16ms 的硬节拍，被低优先级任务挤到就会丢样本
   * （丢样本 = PC 侧听到"沙沙"的断续）。 */
  pid = task_create("voice_link", 110, 4096, (main_t)vl_thread, NULL);
  if (pid < 0)
    {
      syslog(LOG_ERR, "ERROR: voice_link task_create failed: %d\n", errno);
      return -1;
    }

  syslog(LOG_INFO, "[vlink] voice_link 任务已启动 (pid=%d)\n", pid);
  return 0;
}

/* ------------------------------------------------------------------------- */
/* 产品模式入口                                                               */
/* ------------------------------------------------------------------------- */

/* 历史背景（保留备查）：本固件的 CONFIG_INIT_ENTRYPOINT 从 nsh_main 改成了
 * mianyu_main。为什么非改不可：板上只有一个能通到 USB 的串口
 * （UART1 = /dev/console），nsh 会在这个口上阻塞 read() 抢 PC 发下来的下行
 * PCM。实测：nsh 还在的时候 100 个 PING 只回 1 个（下行成功率 1%），而且
 * nsh 对二进制垃圾的回应会灌进上行流，把上行也搅乱（CRC 错 41 / 重同步 275）。
 * 一个串口只能有一个读者 —— 让语音链路独占，产品才是可用的。
 *
 * 需要 shell 调试时：PC 发 CMD 0x10，链路会把控制台交还给 nsh（见
 * vl_hand_over_to_shell）。这个取舍是有意的：交还之后下行就不能用了。
 *
 * ---------------------------------------------------------------------------
 * 注意：这里曾经定义 `int mianyu_main(...)` 来"占住 init 线程"。那是错的：
 * 它与哄睡 app（app/mianyu/mianyu_app_main.c）的 `mianyu_main` **同名**，
 * 链接器在解析 init 入口时先撞上板级这个目标文件，于是 app 的
 * mianyu_app_main.o 整个没被拉进固件 —— 编译日志一切正常、固件里却没有
 * 哄睡主循环，是典型的"静默符号抢占"。
 *
 * 现在板级函数已改名为 vl_product_hold()，仅作兜底：当 app 因配置原因
 * 没被编进固件时，由它占住 init 线程，保证系统不会因为没有 init 入口而
 * 跑飞。app 正常编入时，真正执行的是 app 的 mianyu_main（哄睡主循环）。
 *
 * 语音链路的启动**不依赖**这个符号：它由 board_late_initialize()
 * 里的 bsp_voice_link_start() 独立拉起（见 sifli_ap.c）。
 *
 * **绝对不能 return** —— init 退了系统行为未定义。 */
int vl_product_hold(int argc, char *argv[])
{
  (void)argc;
  (void)argv;

  syslog(LOG_INFO,
         "[vlink] vl_product_hold: 兜底占住 init（说明哄睡 app 未编入固件）\n");

  for (;;)
    {
      sleep(3600);
    }

  return 0;
}
