/* SPDX-License-Identifier: Apache-2.0
 *
 * 安眠科技 · HAL 后端：openvela / NuttX 真机（SF32LB52-DevKit-LCD）
 *
 * 这是上真机时链接的 backend，实现 hal/mianyu_hal.h 的同一套 my_hal_* 符号。
 * 与 hal/sim 的区别只是「平台能力从哪来」：
 *   时间   = NuttX RTC（gettimeofday）
 *   存储   = LittleFS 文件（二进制版本化序列化，依赖零、掉电可恢复）
 *   音频出 = 板级 BSP 的放音环形缓冲（bsp_audio_play_pcm，已接真机验证）
 *   麦克风 = 板级 BSP 的采集环形缓冲（bsp_audio_mic_read，已接真机验证）
 *   灯光   = LVGL 呼吸光晕（app/ui/breathe_lvgl.c）
 *   IMU    = 无板载 IMU（DevKit-LCD），恒 false
 *
 * 编译位置：本文件在 openvela 工程内编译（见 app/mianyu/CMakeLists.txt），
 * 不在 PC 的 `make` 里（PC 用 hal/sim）。移植时按 app/PORT_TO_OPENVELA.md 走。
 *
 * 【诚实边界】音频三件套（init/play/mic_read）已接到板级 BSP 并上真机跑通；
 * 仍带 `[TODO 接 SDK]` 的是存储挂载与 LVGL 灯光两条支路。 */
#include <stdint.h>

#include "mianyu_hal.h"
#include "mianyu_common.h"
#include "mianyu_ui.h"

/* 屏幕这一支只在配了 LVGL 的板子上编译（DevKit-LCD 有 CO5300 AMOLED +
 * FT6146 触摸，见 CONFIG_LV_USE_NUTTX_LCD / CONFIG_LV_USE_NUTTX_TOUCHSCREEN）。
 * 没配 LVGL 时，界面相关代码整段不参与编译，HAL 仍可单独用于纯音频场景。 */
#if defined(CONFIG_GRAPHICS_LVGL) && defined(CONFIG_LV_USE_NUTTX_LCD)
#  define MIANYU_HAVE_LVGL 1
#  include <nuttx/config.h>
#  include <nuttx/sched.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <sched.h>
#  include <syslog.h>
#  include <unistd.h>

#  include <lvgl/lvgl.h>
#  include "watch_ui.h"

/* LVGL 线程入口。openvela 这边没有别人替我们初始化 LVGL —— 没有 nsh 控制台、
 * 也没接 lvx 的 demo 框架，所以 lv_init + lv_nuttx_init 必须自己做，否则
 * lv_display_get_default() 一直是空，界面根本建不出来（屏幕停在黑/白）。
 * 前进声明放这里是必须的：my_hal_init() 在文件靠前的位置就要创建这个线程。 */
static void mianyu_lvgl_thread(int argc, FAR char *argv[]);

#else
#  define MIANYU_HAVE_LVGL 0
#endif

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

/* ===================== 板载音频通路（vendor 侧） =====================
 *
 * 下面三个符号实现在 openvela 工程的板级 BSP 里：
 *   vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd/src/bsp_audio_test.c
 * 它把 AUDCODEC 的 DAC 通路（A.33 -> NS4150B U0104 -> 喇叭 J0101，使能脚 PA10）
 * 和 MIC 通路（M0100/WMM7037 -> MIC_BIAS -> 模拟 ADC -> DMA）都初始化好，
 * 并对外只暴露这三个能力。核心层因此完全不碰寄存器。
 *
 * 链路参数：16kHz / 16bit / mono，与 my_pcm_t 一致，不需要重采样。 */
extern int  bsp_audio_ready(void);
extern int  bsp_audio_play_pcm(const int16_t *pcm, int count);
extern void bsp_audio_play_clear(void);
extern int  bsp_audio_mic_read(int16_t *dst, int max);
extern void bsp_audio_set_amp(int on);
extern void bsp_audio_set_volume_pct(int pct);

/* ===================== 生命周期 ===================== */

my_err_t my_hal_init(void)
{
    /* 音频通路是异步初始化的（板级 BSP 起了一个自己的线程，PLL/REFGEN/加电
     * 顺序要等，见 bsp_audio_test.c 头部）。这里等它就绪再放行主循环，最多等 3s：
     * 早于它就调 play_pcm/mic_read 会写进还没起来的 DMA。 */
    int i;
    for (i = 0; i < 300 && !bsp_audio_ready(); i++)
    {
        my_hal_sleep_ms(10);
    }
    if (!bsp_audio_ready())
    {
        return MY_ERR_IO;      /* 通路没起来：主循环会降级成"只有算法"跑 */
    }

    /* 功放始终使能（睡眠场景没有省电诉求，反而是唤醒时要能立刻出声）；
     * 放音通路音量给满，响度由 my_fade_apply() 那层的增益曲线控制 ——
     * 这样"淡入淡出"是软件算的，不会被两次量化夹掉动态。 */
    bsp_audio_set_amp(1);
    bsp_audio_set_volume_pct(100);

#if MIANYU_HAVE_LVGL
    /* 界面：起一条常驻线程去初始化 LVGL 并建界面。不在这里等它 ——
     * 屏的初始化比音频还慢（co5300 要发一大串初始化命令），而且界面晚
     * 一两百毫秒出现对手感没有任何影响，不该拖住主循环。 */
    {
        int pid = task_create("mianyu_lvgl", SCHED_PRIORITY_DEFAULT, 49152,
                              mianyu_lvgl_thread, NULL);
        if (pid < 0) {
            syslog(LOG_ERR, "[mianyu] LVGL 线程创建失败: %d\n", errno);
        } else {
            syslog(LOG_ERR, "[mianyu] LVGL 线程已起 pid=%d\n", pid);
        }
    }
#endif

    /* [TODO 接 SDK] 挂载 LittleFS：NuttX 的 mount() / boardctl，确保
     * VELA_STORE_DIR 可写；RTC 同步（NTP/网络授时）。 */
    return MY_OK;
}

void my_hal_deinit(void)
{
    /* 停播并清空缓冲，避免退出时喇叭留一截尾音 */
    bsp_audio_play_clear();
    /* [TODO 接 SDK] 关闭 LVGL */
}

void my_hal_sleep_ms(int ms)
{
    /* 真机：让出 CPU，真实等 ms（usleep 即可满足非实时节拍） */
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ===================== 音频输出 ===================== */

int my_hal_audio_play(const my_pcm_t *buf, int count)
{
    /* 直接写板级放音环形缓冲：非阻塞，缓冲不够就只写进去能写的部分，
     * 返回值就是实际写进去的样本数（契约与 hal/mianyu_hal.h 一致）。
     * 底层绝不在这里等 —— 主循环被卡住会连带把 MIC 采集也堵住。 */
    if (!buf || count <= 0) return 0;
    return bsp_audio_play_pcm((const int16_t *)buf, count);
}

void my_hal_audio_stop(void)
{
    /* 清空播放缓冲（入睡淡出归零后调用，避免残留尾音） */
    bsp_audio_play_clear();
}

/* ===================== 麦克风输入 ===================== */

int my_hal_mic_read(my_pcm_t *buf, int max_count)
{
    /* 从板级采集环形缓冲取"已经完整采到"的样本，无新数据返回 0。
     * 底层按 DMA 的 HT/TC 计数算已产量，绝不读正在写的那一块。 */
    if (!buf || max_count <= 0) return 0;
    return bsp_audio_mic_read((int16_t *)buf, max_count);
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

/* 界面用的快照 + 两个请求标志。放在 light_set 前面，因为 light_set 要把
 * 引擎算出来的亮度记进快照，界面那边读的就是这份。 */
static my_ui_state_t s_ui;
static bool          s_ui_start_req;
static bool          s_ui_stop_req;

void my_hal_light_set(int level_pct)
{
    /* 呼吸光晕现在由 ui/watch_ui.c 按快照里的 light_pct 直接渲染
     * （它本来就在 LVGL 线程里跑，不需要再绕一层异步调用）。
     * 这里只记一下，方便串口排查时能看到引擎算出来的亮度。 */
    s_ui.light_pct = level_pct < 0 ? 0 : (level_pct > 100 ? 100 : level_pct);
}

/* ===================== 手表界面 =====================
 *
 * 两个线程的分工，说清楚免得后面有人乱改：
 *
 *   主循环线程 ── my_hal_ui_update(快照) ──▶ 写进 s_ui
 *   LVGL 线程 ── 自己的 100ms 定时器 ──▶ my_hal_ui_state() 读 s_ui 刷界面
 *
 * 两边只共享这么一个纯 int 的结构体，不做任何对象层面的跨线程调用。
 * LVGL 自己也是单线程的：lv_init/lv_nuttx_init、建界面对象、lv_timer_handler
 * 全部发生在 mianyu_lvgl_thread 这一条线程里，别的线程只写 s_ui。
 */

#if MIANYU_HAVE_LVGL
/* LVGL 常驻线程：初始化显示与触摸 → 建界面 → 一直驱动 lv_timer_handler。
 *
 * 为什么不把 lv_timer_handler 塞进主循环：主循环是 100ms 一拍的睡眠算法节拍，
 * 而 LVGL 要按它自己算出来的空闲时间跑（返回值 idle 就是它希望下次被调用的
 * 毫秒数）。塞进主循环会让动画和触摸响应都被算法节拍绑住。单独一条线程最省事，
 * 也天然满足了「界面对象只在 LVGL 线程里被碰」这条约束。 */
/* 等一个设备节点出现。板级的 LCD 起得比我们这条线程慢得多（co5300 要发一大串
 * 初始化命令、面板自检、再 lcddev_register 重试，实测好几秒），开机直接 open
 * 只会拿到 ENOENT。 */
static bool my_wait_dev(const char *path, int max_ms)
{
    int waited = 0;
    while (waited < max_ms) {
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) { close(fd); return true; }
        usleep(100 * 1000);
        waited += 100;
    }
    return false;
}

/* 只探一下节点在不在，不等。运行期用它轮询后到的触摸设备。 */
static bool my_dev_exists(const char *path)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return false;
    close(fd);
    return true;
}

static void mianyu_lvgl_thread(int argc, FAR char *argv[])
{
    lv_nuttx_dsc_t    dsc;
    lv_nuttx_result_t res;
    lv_indev_t       *indev = NULL;
    uint32_t          last_probe = 0;

    (void)argc; (void)argv;

    if (!my_wait_dev("/dev/lcd0", 20000)) {
        syslog(LOG_ERR, "[mianyu] 等 /dev/lcd0 超时，界面不启动\n");
        return;
    }

    lv_init();

    lv_nuttx_dsc_init(&dsc);
    dsc.fb_path    = "/dev/lcd0";    /* co5300 经 lcddev_register(0) 注册 */
    /* lv_nuttx_dsc_init() 默认就把 input_path 指到 /dev/input0，但开机这一刻
     * 它一定还不存在（见下面主循环里的补挂），留着只会让 lv_nuttx_init 打一行
     * errno=2。清掉，触摸交给后面的探测负责。 */
    dsc.input_path = NULL;
    lv_nuttx_init(&dsc, &res);

    if (res.disp == NULL) {
        syslog(LOG_ERR, "[mianyu] LVGL display 创建失败\n");
        return;
    }

    watch_ui_start();                /* 表盘页 + 哄睡页，开 100ms 刷新定时器 */

    /* ---- 换成局部刷新缓冲 ----
     * lv_nuttx 默认给的是【整屏缓冲 + LV_DISPLAY_RENDER_MODE_FULL】（它按
     * CONFIG_LV_NUTTX_LCD_BUFFER_COUNT>0 走全屏那条分支）：每帧都渲染一整屏
     * 390*450*2 = 351 KB、再整屏刷到 AMOLED。实测 196 ms 一帧、5 fps，呼吸
     * 光晕肉眼看就是一顿一顿的，而且这条线程一直在吃满 CPU，还挤主循环。
     * 我们这套界面每帧真正变的只有一小块（光晕的透明度/尺寸 + 几个字），
     * 一帧只渲染、只刷脏区那几条带就够了。
     * 缓冲放静态数组（不进堆，链接期就能看到占用）；48 行是 LVGL 要求的
     * "不小于屏幕 1/10"往上取整的结果。
     * 代价：lcd_init 里那 351 KB 整屏缓冲就此闲置 —— 这是驱动端口分配的，
     * 我们拿不到句柄，只能让它留着。8 MB PSRAM 上这点代价可以接受。 */
    {
        static lv_color_t s_lcd_partial_buf[390 * 48];
        lv_display_set_buffers(res.disp, s_lcd_partial_buf, NULL,
                               sizeof(s_lcd_partial_buf),
                               LV_DISPLAY_RENDER_MODE_PARTIAL);
    }

    for (;;) {
        uint32_t idle;

        /* ---- 触摸补挂 ----
         * 触摸是【后到】的：板级那条 lcd 线程要先把 co5300 一长串初始化命令
         * 发完、面板自检跑完，才轮到 ft6146 去 touch_register("/dev/input0")，
         * 而 LVGL 一拿到 /dev/lcd0 就已经起来了。所以触摸不能在初始化时
         * 一次性判定（以前写成"等 5 秒，等不到就永远没有触摸"，实测正好差
         * 那么一点点），必须运行期盯着，节点一出现就补挂上去。
         *
         * 计时必须用 lv_tick_get() 这种真实毫秒。一开始拿 lv_timer_handler()
         * 的返回值累加，那是错的：它给的是"建议下次被调用的间隔"，而这块屏
         * 渲染一帧要一百多毫秒，两者差两个数量级 —— 实测把"每 0.5 秒探一次"
         * 拖成了 110 秒才探一次，触摸节点早就存在了却一直没挂上。 */
        if (indev == NULL) {
            uint32_t t0 = lv_tick_get();
            if ((uint32_t)(t0 - last_probe) >= 500) {
                last_probe = t0;
                if (my_dev_exists("/dev/input0")) {
                    indev = lv_nuttx_touchscreen_create("/dev/input0");
                    if (indev != NULL) {
                        /* lv_indev_create() 自己会绑默认屏，这里显式再绑一遍，
                         * 免得以后多屏时踩坑。 */
                        lv_indev_set_display(indev, res.disp);
                        syslog(LOG_ERR, "[mianyu] 触摸已补挂 indev=%p\n",
                               (void *)indev);
                    }
                }
            }
        }

        idle = lv_timer_handler();

        usleep((idle ? idle : 1) * 1000);   /* 至少睡 1ms，别空转烧 CPU */
    }
}
#endif

void my_hal_ui_update(const my_ui_state_t *st)
{
    /* 只写快照。界面对象全在 LVGL 线程那边，这里碰都不碰 —— 两边共享的
     * 就是这个纯 int 的结构体，界面按 100ms 自己读，不需要锁也不会竞态。 */
    if (st) s_ui = *st;
}

const my_ui_state_t *my_hal_ui_state(void)
{
    return &s_ui;
}

void my_hal_ui_request_start(void)
{
    s_ui_start_req = true;
}

bool my_hal_ui_take_start_request(void)
{
    bool r = s_ui_start_req;
    s_ui_start_req = false;
    return r;
}

void my_hal_ui_request_stop(void)
{
    s_ui_stop_req = true;
}

bool my_hal_ui_take_stop_request(void)
{
    bool r = s_ui_stop_req;
    s_ui_stop_req = false;
    return r;
}
