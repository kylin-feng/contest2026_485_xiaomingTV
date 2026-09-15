/* SPDX-License-Identifier: Apache-2.0
 *
 * 安眠科技 · 手表界面（LVGL 9）
 *
 * ===================== 两个页面 =====================
 *
 *  表盘页 —— 设备平时就停在这。大时间是主体，下面一行是「今晚几点自动开始」，
 *            再下面一个按钮「开始哄睡」。最底下一行是昨晚的结果。
 *            这里要回答的问题只有一个：我现在要不要睡，以及昨晚睡得怎么样。
 *
 *  哄睡页 —— 点按钮进来的。中间是呼吸光晕（跟着它吸气、屏息、呼气），
 *            光晕上面一行字说明当前在干什么，底部两行是噪声类型和已用时。
 *            睡着以后屏幕整体暗下去，只留一点微光，不再有可读的文字。
 *
 * ===================== 交互怎么设计的 =====================
 *
 * 能点的东西只有两个：表盘上的「开始哄睡」，哄睡页左上的「返回」。
 * 手表屏小，触摸精度有限，所以每个热区都不小于 68×40，字号最小 16。
 *
 *   · 点「开始哄睡」→ 立刻切到哄睡页（不等后台确认，手感上没有延迟），
 *     同时给主循环发一个开始请求。真实动作由主循环去做。
 *   · 点「返回」→ 回表盘，但哄睡继续跑。这是有意的：设备在哄睡中途被
 *     用户按回表盘，不该把声音和判定一起停掉。表盘上的按钮这时写着
 *     「回到哄睡」，点一下能切回去看。
 *   · 长按「返回」→ 才是真的结束今晚（停声音、复位判定）。
 *
 * ===================== 夜里怎么不刺眼 =====================
 *
 * 这块是 AMOLED，纯黑像素不发光。所以：
 *   · 底色用 000000，界面元素之外一点光都不漏。
 *   · 只用暖色（FFB26B，约 2700K），不用蓝白。蓝光会压褪黑素。
 *   · 睡着之后（MY_UI_PHASE_ASLEEP）光晕亮度直接压到很低，文字不更新，
 *     屏幕进入「几乎全黑、只有一点呼吸感」的状态。
 *   · 夜醒安抚时只亮到两成左右，不弹任何需要读的信息。
 *
 * 编译位置：本文件含 <lvgl/lvgl.h>，只在 openvela 工程内编译。
 * 字体 lv_font_mianyu_16 / _32 是用文泉驿微米黑裁出来的，只含界面用到的字。
 */
#include <lvgl/lvgl.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include "mianyu_ui.h"
#include "mianyu_breathe.h"
#include "watch_ui.h"

/* 裁字体时用的就是这两个名字（见 ui/lv_font_mianyu_*.c） */
extern const lv_font_t lv_font_mianyu_16;
extern const lv_font_t lv_font_mianyu_32;

/* ===================== 配色 ===================== */

#define C_BLACK      lv_color_hex(0x000000)   /* AMOLED 像素级熄灯 */
#define C_WARM       lv_color_hex(0xFFB26B)   /* 暖琥珀，约 2700K */
#define C_WARM_DIM   lv_color_hex(0x6B4A28)   /* 暖色的暗版，描边用 */
#define C_TEXT       lv_color_hex(0xEDEAE6)   /* 正文，不是纯白，减少刺眼 */
#define C_TEXT_DIM   lv_color_hex(0x8A8580)   /* 次要信息 */

/* ===================== 界面对象 ===================== */

typedef struct {
    lv_obj_t *watch;            /* 表盘页（整页容器） */
    lv_obj_t *sleep;            /* 哄睡页（整页容器） */

    /* 表盘页上的东西 */
    lv_obj_t *w_date;
    lv_obj_t *w_time;
    lv_obj_t *w_plan;
    lv_obj_t *w_btn;
    lv_obj_t *w_btn_lbl;
    lv_obj_t *w_last1;
    lv_obj_t *w_last2;

    /* 哄睡页上的东西 */
    lv_obj_t *s_back;
    lv_obj_t *halo_o, *halo_m, *halo_c;
    lv_obj_t *s_phase;
    lv_obj_t *s_sub;
    lv_obj_t *s_meta;
    lv_obj_t *s_elapsed;

    my_breathe_t engine;        /* 呼吸节奏源（纯 C 算法） */
    lv_timer_t  *tick;
    bool         on_sleep_page; /* 当前在哄睡页吗 */
    int          last_phase;    /* 上一帧阶段，用于只在变化时改文字 */
    int          shown_elapsed; /* 上一帧显示的秒数，避免每帧重排 */

    /* ---- 「上次真正写进对象的值」缓存 ----
     * 表盘大部分时间是不变的（时间一分钟才动一次、昨晚结果一天一次），
     * 所以每个标签都留一份上次写进去的文字，值没变就一个 LVGL 调用都不发。
     * 见 set_text_if_changed() 上面的说明。 */
    char last_date[64];
    char c_time[16];
    char c_plan[48];
    char c_btn[24];
    char c_last1[48];
    char c_last2[48];
    char c_phase[24];
    char c_sub[32];
    char c_meta[40];
    char c_elapsed[48];
    int  last_running;          /* 上一帧是否在哄睡（-1 = 还没定过） */
    int  last_halo_opa[3];      /* 三层光晕上次写进去的透明度（-1 = 还没写） */
    lv_coord_t last_core;       /* 上次写进去的内核直径 */
} watch_ui_t;

static watch_ui_t s;
static bool s_inited;

/* ===================== 小工具 ===================== */

static const char *aid_name(int kind)
{
    switch (kind) {
    case 0:  return "棕噪";
    case 1:  return "白噪";
    case 2:  return "粉噪";
    default: return "白噪";
    }
}

static const char *weekday_name(int w)
{
    static const char *n[7] = { "周日", "周一", "周二", "周三", "周四", "周五", "周六" };
    return (w >= 0 && w < 7) ? n[w] : "";
}

/* 秒 → "12分04秒" / "1小时02分" */
static void fmt_dur(char *b, size_t n, int sec)
{
    if (sec < 0) sec = 0;
    if (sec < 3600) {
        snprintf(b, n, "%d分%02d秒", sec / 60, sec % 60);
    } else {
        snprintf(b, n, "%d小时%02d分", sec / 3600, (sec % 3600) / 60);
    }
}

/* 建一个铺满父容器的纯黑容器 */
static lv_obj_t *mk_page(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(o, C_BLACK, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font,
                          lv_color_t color, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_remove_style_all(l);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, txt);
    return l;
}

/* 只在文字真的变了才写进标签。这个函数是整个界面流畅度的关键。
 *
 * 原因：lv_label_set_text() 一进来就无条件 lv_obj_invalidate()（它不比较
 * 新旧文字），而且每次都 free + malloc 一遍文本缓冲。表盘上 6 个标签按
 * 10Hz 重设，等于每秒 60 次无效化 + 60 次堆分配，屏就一直在重画 ——
 * 实测这块屏渲染一帧要 ~5 ms/千像素 的量级，66k 像素的无效化就是 200 ms
 * 一帧、5 fps，光晕看着一顿一顿，还白占 CPU。
 *
 * 所以每个标签配一份缓存（watch_ui_t 里那些 c_*），值没变就一个 LVGL 调用
 * 都不发。表盘静止时无效化面积直接归零。 */
static void set_text_if_changed(lv_obj_t *l, char *cache, size_t n, const char *txt)
{
    if (strcmp(cache, txt) == 0) return;
    snprintf(cache, n, "%s", txt);      /* 缓存自己持有一份，传给 LVGL 的字串安全 */
    lv_label_set_text(l, cache);
}

/* 一层光晕圆。外大内小、内亮外暗，三层叠出渐变感。 */
static lv_obj_t *mk_halo(lv_obj_t *parent, lv_coord_t d, lv_opa_t opa)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, d, d);
    lv_obj_set_style_bg_color(o, C_WARM, 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(o, LV_ALIGN_CENTER, 0, -30);
    return o;
}

/* ===================== 页面切换 ===================== */

static void show_page(bool sleep_page)
{
    if (!s.watch) return;
    if (sleep_page) {
        lv_obj_add_flag(s.watch, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s.sleep, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s.sleep, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s.watch, LV_OBJ_FLAG_HIDDEN);
    }
    s.on_sleep_page = sleep_page;
}

/* ===================== 事件 ===================== */

static bool s_long_press_fired;

static void ev_btn_start(lv_event_t *e)
{
    (void)e;
    /* 这几条事件日志是故意留着的：板上起哄睡之后屏幕反应才是唯一证据，
     * 串口上留一行"谁点的、点了什么"，排查时不用猜。频率极低（一次点击一行）。*/
    syslog(LOG_ERR, "[watch_ui] 点开始哄睡\n");
    my_hal_ui_request_start();   /* 真正动手的是主循环 */
    show_page(true);             /* 界面立刻响应，不等后台 */
}

/* 按下/松开给一个即时反馈：描边加粗 + 一层淡暖底。
 * 表屏小、手指把按钮挡掉一大半，这点变化是"点到了"的唯一即时证据
 * （尤其触摸刚补挂、跟随手指的光标又没开的时候）。 */
static void ev_btn_pressed(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_set_style_border_width(btn, 3, 0);
    lv_obj_set_style_bg_color(btn, C_WARM, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_20, 0);
}

static void ev_btn_released(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
}

static void ev_back_long(lv_event_t *e)
{
    (void)e;
    s_long_press_fired = true;
    syslog(LOG_ERR, "[watch_ui] 长按返回：结束今晚\n");
    my_hal_ui_request_stop();    /* 长按才是结束今晚 */
    show_page(false);
}

static void ev_back_click(lv_event_t *e)
{
    (void)e;
    if (s_long_press_fired) {    /* 长按松手后 LVGL 还会补一个点击 */
        s_long_press_fired = false;
        return;
    }
    syslog(LOG_ERR, "[watch_ui] 返回表盘（哄睡继续）\n");
    show_page(false);            /* 只是回表盘，哄睡照跑 */
}

/* ===================== 表盘页刷新 ===================== */

static void update_watch(const my_ui_state_t *st)
{
    char buf[48];
    int  running;

    /* 日期只在跨天时才改 */
    snprintf(buf, sizeof(buf), "%d月%d日 %s",
             st->month, st->day, weekday_name(st->weekday));
    set_text_if_changed(s.w_date, s.last_date, sizeof(s.last_date), buf);

    /* 时间一分钟才动一次，剩下 5999 帧都不该碰它 */
    snprintf(buf, sizeof(buf), "%02d:%02d", st->hour, st->minute);
    set_text_if_changed(s.w_time, s.c_time, sizeof(s.c_time), buf);

    /* 今晚计划：主动式调度的意思就在这一行 —— 不点它也会开始 */
    if (st->plan_valid) {
        snprintf(buf, sizeof(buf), "今晚%d:%02d自动开始",
                 st->plan_hour, st->plan_min);
    } else {
        snprintf(buf, sizeof(buf), "还没定今晚几点睡");
    }
    set_text_if_changed(s.w_plan, s.c_plan, sizeof(s.c_plan), buf);

    /* 按钮：没在哄睡时是「开始哄睡」，正在哄睡时是「回到哄睡」。
     * 文字和两个颜色都只在状态翻转那一次改 —— 改样式同样是一次无效化，
     * 而且描边横跨 220×62，改一次就够刷半张卡。 */
    running = (st->phase != MY_UI_PHASE_IDLE);
    if (running != s.last_running) {
        s.last_running = running;
        snprintf(s.c_btn, sizeof(s.c_btn), "%s",
                 running ? "回到哄睡" : "开始哄睡");
        lv_label_set_text(s.w_btn_lbl, s.c_btn);
        lv_obj_set_style_border_color(s.w_btn, running ? C_WARM_DIM : C_WARM, 0);
        lv_obj_set_style_text_color(s.w_btn_lbl, running ? C_TEXT_DIM : C_WARM, 0);
    }

    /* 昨晚的结果 */
    if (st->last_valid) {
        int h = st->last_total_min / 60, m = st->last_total_min % 60;
        if (h > 0) {
            snprintf(buf, sizeof(buf), "昨晚睡了%d小时%d分", h, m);
        } else {
            snprintf(buf, sizeof(buf), "昨晚睡了%d分", m);
        }
        set_text_if_changed(s.w_last1, s.c_last1, sizeof(s.c_last1), buf);
        snprintf(buf, sizeof(buf), "入睡%d分 · 夜醒%d次",
                 st->last_onset_sec / 60, st->last_wakes);
        set_text_if_changed(s.w_last2, s.c_last2, sizeof(s.c_last2), buf);
    } else {
        set_text_if_changed(s.w_last1, s.c_last1, sizeof(s.c_last1), "还没有记录");
        set_text_if_changed(s.w_last2, s.c_last2, sizeof(s.c_last2),
                            "今晚睡一晚就有了");
    }
}

/* ===================== 哄睡页刷新 ===================== */

static void update_sleep(const my_ui_state_t *st)
{
    char buf[48];
    int  lvl;
    my_breath_phase_t ph;
    const char *phase_txt = "放松";
    const char *sub_txt   = "跟着光呼吸";

    switch (st->phase) {
    case MY_UI_PHASE_GUIDING:
    case MY_UI_PHASE_DETECTING:
        /* 引导和判定共用呼吸节拍：用户跟着光呼吸，MIC 同时在听 */
        my_breathe_tick(&s.engine, 100);
        lvl = my_breathe_level_pct(&s.engine);
        ph  = my_breathe_get_phase(&s.engine);
        switch (ph) {
        case MY_BREATH_INHALE: phase_txt = "吸气"; break;
        case MY_BREATH_HOLD:   phase_txt = "屏息"; break;
        case MY_BREATH_EXHALE: phase_txt = "呼气"; break;
        default:               phase_txt = "放松"; break;
        }
        sub_txt = (st->phase == MY_UI_PHASE_DETECTING)
                  ? "正在听你的呼吸" : "跟着光呼吸";
        break;

    case MY_UI_PHASE_ASLEEP:
        /* 睡着后不再有可读信息，光晕压到很低，只留一点存在感 */
        lvl = 8;
        phase_txt = "已睡着";
        sub_txt   = "晚安";
        break;

    case MY_UI_PHASE_NIGHT_WAKE:
        lvl = 14;
        phase_txt = "安抚中";
        sub_txt   = "接着睡就好";
        break;

    default:
        lvl = 0;
        phase_txt = "还没开始";
        sub_txt   = "";
        break;
    }

    set_text_if_changed(s.s_phase, s.c_phase, sizeof(s.c_phase), phase_txt);
    set_text_if_changed(s.s_sub, s.c_sub, sizeof(s.c_sub), sub_txt);

    /* 亮度 → 三层透明度。外淡内亮，光晕才有层次。
     *
     * 这里按层做了"透明度量化"：外圈直径 214，它改一次透明度就要重绘
     * 45.8k 像素 —— 而外圈那点明暗变化肉眼几乎看不出。所以外圈只取 8 档、
     * 中圈 16 档，档位没跳就不发给 LVGL；内核最小（86²，视觉焦点），逐档更新。
     * 效果：呼吸每拍真正重绘的只剩内核那 7k 像素，而不是三层加起来的 73k。 */
    {
        static const lv_opa_t quant[3] = { 16, 8, 1 };   /* 外 / 中 / 内 的量化粒度 */
        lv_obj_t *halo[3] = { s.halo_o, s.halo_m, s.halo_c };
        lv_opa_t  opa = (lv_opa_t)(lvl * 255 / 100);
        lv_opa_t  want[3];
        int i;

        want[0] = (lv_opa_t)(opa / 4);
        want[1] = (lv_opa_t)(opa / 2);
        want[2] = opa;

        for (i = 0; i < 3; i++) {
            lv_opa_t q = (lv_opa_t)((want[i] / quant[i]) * quant[i]);
            if ((int)q != s.last_halo_opa[i]) {
                s.last_halo_opa[i] = (int)q;
                lv_obj_set_style_bg_opa(halo[i], q, 0);
            }
        }
    }

    /* 内核随呼吸轻微鼓缩，像肺在动（尺寸只在真的变了才改） */
    lv_coord_t core = 86;
    if (st->phase == MY_UI_PHASE_GUIDING || st->phase == MY_UI_PHASE_DETECTING) {
        core = 86 + (lv_coord_t)((lvl - 50) * 22 / 100);
        if (core < 78) core = 78;
        if (core > 108) core = 108;
    }
    if (core != s.last_core) {
        s.last_core = core;
        lv_obj_set_size(s.halo_c, core, core);
        lv_obj_align(s.halo_c, LV_ALIGN_CENTER, 0, -30);
    }

    /* 底部：噪声类型 + 音量 */
    if (st->phase == MY_UI_PHASE_IDLE) {
        set_text_if_changed(s.s_meta, s.c_meta, sizeof(s.c_meta), "");
        set_text_if_changed(s.s_elapsed, s.c_elapsed, sizeof(s.c_elapsed), "");
    } else {
        snprintf(buf, sizeof(buf), "%s %d%%", aid_name(st->aid_kind), st->volume_pct);
        set_text_if_changed(s.s_meta, s.c_meta, sizeof(s.c_meta), buf);

        /* 秒数只在整秒变化时重排 */
        if (st->elapsed_sec != s.shown_elapsed) {
            char line[64];
            s.shown_elapsed = st->elapsed_sec;
            fmt_dur(buf, sizeof(buf), st->elapsed_sec);
            snprintf(line, sizeof(line), "已用 %s", buf);
            set_text_if_changed(s.s_elapsed, s.c_elapsed,
                                sizeof(s.c_elapsed), line);
        }
    }

    s.last_phase = (int)st->phase;
}

/* ===================== 心跳 ===================== */

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    const my_ui_state_t *st = my_hal_ui_state();
    if (!st) return;

#ifdef MIANYU_UI_BENCH
    /* 台架自检模式（编译期开关，默认关）：不看触摸、不管有没有真的在哄睡，
     * 直接把状态伪装成"引导中"，让哄睡页的呼吸光晕一直动起来。
     * 用途是上板调渲染节奏和配色 —— 光晕是整个界面最重的一块（三层圆、
     * 每拍都在变），必须能单独量它，而不是靠在板子上手点按钮碰运气。 */
    {
        static my_ui_state_t bench;
        if (st->phase == MY_UI_PHASE_IDLE) {
            bench = *st;
            bench.phase        = MY_UI_PHASE_GUIDING;
            bench.aid_kind     = 0;
            bench.volume_pct   = 40;
            bench.elapsed_sec  = 132;
            st = &bench;
        }
    }
#endif

    update_watch(st);
    /* 哄睡页只在可见时刷新，省掉没必要的排版开销 */
    if (s.on_sleep_page) update_sleep(st);
}

/* ===================== 创建 ===================== */

static void build_watch_page(lv_obj_t *scr)
{
    char buf[48];
    const my_ui_state_t *st = my_hal_ui_state();

    s.watch = mk_page(scr);

    lv_obj_t *d = mk_label(s.watch, &lv_font_mianyu_16, C_TEXT_DIM, "");
    lv_obj_align(d, LV_ALIGN_TOP_MID, 0, 44);
    s.w_date = d;

    lv_obj_t *tm = mk_label(s.watch, &lv_font_mianyu_32, C_TEXT, "0:00");
    lv_obj_align(tm, LV_ALIGN_TOP_MID, 0, 82);
    s.w_time = tm;

    lv_obj_t *pl = mk_label(s.watch, &lv_font_mianyu_16, C_WARM_DIM, "");
    lv_obj_align(pl, LV_ALIGN_TOP_MID, 0, 158);
    s.w_plan = pl;

    /* 开始按钮：暖色描边胶囊。触摸热区 220×62，手小也能点中。 */
    lv_obj_t *btn = lv_obj_create(s.watch);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 220, 62);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 206);
    lv_obj_set_style_radius(btn, 31, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, C_WARM, 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, ev_btn_pressed,  LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(btn, ev_btn_released, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(btn, ev_btn_released, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(btn, ev_btn_start, LV_EVENT_CLICKED, NULL);
    s.w_btn = btn;

    lv_obj_t *bl = mk_label(btn, &lv_font_mianyu_16, C_WARM, "开始哄睡");
    lv_obj_center(bl);
    s.w_btn_lbl = bl;

    /* 昨晚的结果 */
    lv_obj_t *l1 = mk_label(s.watch, &lv_font_mianyu_16, C_TEXT_DIM, "");
    snprintf(buf, sizeof(buf), "");
    lv_obj_align(l1, LV_ALIGN_TOP_MID, 0, 322);
    s.w_last1 = l1;

    lv_obj_t *l2 = mk_label(s.watch, &lv_font_mianyu_16, C_TEXT_DIM, "");
    lv_obj_align(l2, LV_ALIGN_TOP_MID, 0, 354);
    s.w_last2 = l2;

    (void)st;
}

static void build_sleep_page(lv_obj_t *scr)
{
    s.sleep = mk_page(scr);

    /* 返回：左上角小胶囊。短按回表盘，长按结束今晚。 */
    lv_obj_t *bk = lv_obj_create(s.sleep);
    lv_obj_remove_style_all(bk);
    lv_obj_set_size(bk, 76, 40);
    lv_obj_align(bk, LV_ALIGN_TOP_LEFT, 18, 18);
    lv_obj_set_style_radius(bk, 20, 0);
    lv_obj_set_style_bg_opa(bk, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bk, 1, 0);
    lv_obj_set_style_border_color(bk, C_WARM_DIM, 0);
    lv_obj_set_style_border_opa(bk, LV_OPA_COVER, 0);
    lv_obj_remove_flag(bk, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bk, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bk, ev_back_click, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(bk, ev_back_long, LV_EVENT_LONG_PRESSED, NULL);
    s.s_back = bk;

    lv_obj_t *bkl = mk_label(bk, &lv_font_mianyu_16, C_WARM_DIM, "返回");
    lv_obj_center(bkl);

    /* 三层光晕，圆心比屏幕中心稍高一点，给底部文字留位置 */
    s.halo_o = mk_halo(s.sleep, 214, LV_OPA_10);
    s.halo_m = mk_halo(s.sleep, 142, LV_OPA_20);
    s.halo_c = mk_halo(s.sleep,  86, LV_OPA_40);

    s.s_phase = mk_label(s.sleep, &lv_font_mianyu_32, C_TEXT, "放松");
    lv_obj_align(s.s_phase, LV_ALIGN_CENTER, 0, -30);

    s.s_sub = mk_label(s.sleep, &lv_font_mianyu_16, C_TEXT_DIM, "");
    lv_obj_align(s.s_sub, LV_ALIGN_CENTER, 0, 16);

    s.s_meta = mk_label(s.sleep, &lv_font_mianyu_16, C_WARM_DIM, "");
    lv_obj_align(s.s_meta, LV_ALIGN_BOTTOM_MID, 0, -76);

    s.s_elapsed = mk_label(s.sleep, &lv_font_mianyu_16, C_TEXT_DIM, "");
    lv_obj_align(s.s_elapsed, LV_ALIGN_BOTTOM_MID, 0, -46);
}

void watch_ui_start(void)
{
    lv_obj_t *scr;

    if (s_inited) return;
    memset(&s, 0, sizeof(s));
    /* -1 = 「还没写过」，让第一帧无条件把值刷进去（0 会被误判成"已经是 0"） */
    s.last_running     = -1;
    s.last_core        = -1;
    s.last_halo_opa[0] = -1;
    s.last_halo_opa[1] = -1;
    s.last_halo_opa[2] = -1;
    s_inited = true;

    scr = lv_screen_active();
    if (scr == NULL) {
        syslog(LOG_ERR, "[watch_ui] 没有活动屏幕，界面放弃\n");
        return;
    }

    lv_obj_set_style_bg_color(scr, C_BLACK, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    my_breathe_init(&s.engine, NULL);

    build_watch_page(scr);
    build_sleep_page(scr);

#ifdef MIANYU_UI_BENCH
    show_page(true);                                   /* 自检：直接摆哄睡页 */
#else
    show_page(false);                                  /* 开机停在表盘 */
#endif
    s.tick = lv_timer_create(tick_cb, 100, NULL);      /* 10Hz 刷新 */

    tick_cb(NULL);
    syslog(LOG_ERR, "[watch_ui] 界面就绪：表盘页 + 哄睡页，10Hz 刷新\n");
}
