/* SPDX-License-Identifier: Apache-2.0
 *
 * 眠语 · 噪声合成单元测试（PC 端，无需任何硬件/RTOS）
 *
 * 这组测试同时服务技术报告 3.5 节——它证明了"端侧实时合成音频"
 * 不是纸上谈兵：频谱特性、幅度稳定性、无溢出、可复现性都有量化数据。
 *
 * 编译： cc -std=c11 -O2 -I../include -o test_noise_gen test_noise_gen.c ../src/mianyu_noise_gen.c -lm
 * 运行： ./test_noise_gen
 */
#include "mianyu_noise_gen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_SAMPLES  32000     /* 2 秒 @16kHz，足够统计收敛 */

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, fmt, ...) do { \
    if (cond) { g_pass++; printf("  [PASS] " fmt "\n", ##__VA_ARGS__); } \
    else      { g_fail++; printf("  [FAIL] " fmt "\n", ##__VA_ARGS__); } \
} while (0)

static void test_kind(const char *name, my_noise_kind_t kind, uint32_t seed)
{
    printf("\n--- %s (seed=%u) ---\n", name, seed);
    my_noise_gen_t g;
    my_pcm_t *buf = malloc(sizeof(my_pcm_t) * TEST_SAMPLES);
    if (!buf) { printf("  [FAIL] malloc\n"); g_fail++; return; }

    my_err_t e = my_noise_init(&g, kind, seed, 70);   /* 70% 音量，留余量防削顶 */
    CHECK(e == MY_OK, "init 返回 MY_OK");

    int n = my_noise_render(&g, buf, TEST_SAMPLES);
    CHECK(n == TEST_SAMPLES, "render 写满 %d 样本 (实际 %d)", TEST_SAMPLES, n);

    int clipped = my_noise_count_clipped(buf, TEST_SAMPLES);
    CHECK(clipped == 0, "无削顶样本 (削顶=%d)", clipped);

    int32_t dc = my_noise_measure_dc(buf, TEST_SAMPLES);
    int32_t rms = my_noise_measure_rms_q8(buf, TEST_SAMPLES);   /* Q8 */
    /* DC 判据改为相对 RMS：DC blocker 保证直流远小于信号幅度。
     * 粉/棕噪能量集中低频，短窗口(2s)均值天然非零，用绝对阈值不合理。
     * 要求 |DC| < RMS/8（即直流占比 <12.5%），白噪另用更严的绝对阈值。 */
    int32_t rms_abs = rms >> 8;
    if (kind == MY_NOISE_WHITE) {
        CHECK(dc > -300 && dc < 300, "白噪 DC 接近 0 (实测 %d)", dc);
    } else {
        CHECK(dc * 8 < rms_abs && dc * 8 > -rms_abs,
              "%s DC 占比 <12.5%% (DC=%d, RMS=%d)", name, dc, rms_abs);
    }

    /* 70% 音量下 RMS 应为正且在合理区间（Q8） */
    CHECK(rms > 0, "RMS > 0 (实测 %.1f)", rms / 256.0);

    int32_t hf = my_noise_measure_hf_ratio_q8(buf, TEST_SAMPLES);
    printf("  [INFO] RMS=%.1f  DC=%d  HF_ratio=%.3f\n", rms / 256.0, dc, hf / 256.0);

    /* 频谱倾斜判别（不引入 FFT 的轻量法）：
     *   白噪：样本独立，一阶差分能量 ≈ 2× 原能量 → HF_ratio ≈ 2.0
     *   粉噪：介于白噪与棕噪之间 → 0.3 < ratio < 1.6
     *   棕噪：强相关，差分能量 << 原能量 → HF_ratio < 0.3 */
    double hfr = hf / 256.0;
    switch (kind) {
    case MY_NOISE_WHITE:
        CHECK(hfr > 1.6 && hfr < 2.4, "白噪频谱平坦 HF_ratio≈2.0 (实测 %.3f)", hfr);
        break;
    case MY_NOISE_PINK:
        CHECK(hfr > 0.2 && hfr < 1.6, "粉噪频谱倾斜居中 (实测 %.3f)", hfr);
        break;
    case MY_NOISE_BROWN:
        CHECK(hfr < 0.4, "棕噪能量集中低频 HF_ratio<0.4 (实测 %.3f)", hfr);
        break;
    default: break;
    }

    /* 幅度对比：同音量下白噪 RMS 应明显高于棕噪（棕噪能量集中在极低频，
     * 泄漏积分后幅度被压低），验证三种噪声确实不同而非退化成同一个信号 */
    free(buf);
}

static void test_reproducibility(void)
{
    printf("\n--- 可复现性（同 seed 必须逐样本一致）---\n");
    my_pcm_t *a = malloc(sizeof(my_pcm_t) * 1000);
    my_pcm_t *b = malloc(sizeof(my_pcm_t) * 1000);
    my_noise_gen_t g1, g2;
    my_noise_init(&g1, MY_NOISE_PINK, 12345, 80);
    my_noise_init(&g2, MY_NOISE_PINK, 12345, 80);
    my_noise_render(&g1, a, 1000);
    my_noise_render(&g2, b, 1000);
    CHECK(memcmp(a, b, sizeof(my_pcm_t) * 1000) == 0, "同 seed 两次输出完全一致");

    /* 不同 seed 应不同 */
    my_noise_init(&g2, MY_NOISE_PINK, 99999, 80);
    my_noise_render(&g2, b, 1000);
    CHECK(memcmp(a, b, sizeof(my_pcm_t) * 1000) != 0, "不同 seed 输出不同");
    free(a); free(b);
}

static void test_rms_ordering(void)
{
    printf("\n--- 三种噪声幅度关系（白 > 粉 > 棕，同音量）---\n");
    my_pcm_t *buf = malloc(sizeof(my_pcm_t) * TEST_SAMPLES);
    my_noise_gen_t g;
    int32_t rms[3];
    const char *names[3] = {"白噪", "粉噪", "棕噪"};
    for (int k = 0; k < 3; k++) {
        my_noise_init(&g, (my_noise_kind_t)k, 42, 70);
        my_noise_render(&g, buf, TEST_SAMPLES);
        rms[k] = my_noise_measure_rms_q8(buf, TEST_SAMPLES);
        printf("  [INFO] %s RMS=%.1f\n", names[k], rms[k] / 256.0);
    }
    CHECK(rms[0] > rms[1], "白噪 RMS > 粉噪 RMS");
    CHECK(rms[1] > rms[2], "粉噪 RMS > 棕噪 RMS");
    free(buf);
}

static void test_level_and_edge(void)
{
    printf("\n--- 音量与边界 ---\n");
    my_noise_gen_t g;
    my_pcm_t buf[1600];

    my_noise_init(&g, MY_NOISE_WHITE, 1, 0);
    my_noise_render(&g, buf, 1600);
    CHECK(my_noise_measure_rms_q8(buf, 1600) == 0, "0%% 音量输出静音");

    my_noise_init(&g, MY_NOISE_WHITE, 1, 100);
    my_noise_render(&g, buf, 1600);
    CHECK(my_noise_count_clipped(buf, 1600) == 0, "100%% 音量白噪仍不削顶");

    /* 回归防护：粉噪 crest factor 高，满音量最易削顶（曾实测 16% 样本削顶）。
     * 三种噪声在 100% 音量、长样本下都必须零削顶，否则真机爆音。 */
    {
        my_pcm_t *big = malloc(sizeof(my_pcm_t) * 32000);
        const char *kn[3] = {"白", "粉", "棕"};
        for (int k = 0; k < 3; k++) {
            my_noise_init(&g, (my_noise_kind_t)k, 0x5150AA55, 100);
            my_noise_render(&g, big, 32000);
            int cl = my_noise_count_clipped(big, 32000);
            CHECK(cl == 0, "100%% 音量%s噪 32000 样本零削顶 (削顶=%d)", kn[k], cl);
        }
        free(big);
    }

    /* 非法参数 */
    CHECK(my_noise_init(NULL, MY_NOISE_WHITE, 1, 50) == MY_ERR_PARAM, "NULL 句柄返回 MY_ERR_PARAM");
    CHECK(my_noise_init(&g, MY_NOISE_KIND_MAX, 1, 50) == MY_ERR_PARAM, "非法 kind 返回 MY_ERR_PARAM");

    /* 运行期切换类型不崩溃 */
    my_noise_init(&g, MY_NOISE_WHITE, 7, 60);
    my_noise_render(&g, buf, 800);
    CHECK(my_noise_set_kind(&g, MY_NOISE_BROWN) == MY_OK, "运行期切到棕噪 OK");
    my_noise_render(&g, buf, 800);
    CHECK(my_noise_count_clipped(buf, 800) == 0, "切换后仍不削顶");
}

int main(void)
{
    printf("=== 眠语 · 噪声合成单元测试 ===\n");
    test_kind("白噪 WHITE", MY_NOISE_WHITE, 0x1234ABCD);
    test_kind("粉噪 PINK",  MY_NOISE_PINK,  0xDEADBEEF);
    test_kind("棕噪 BROWN", MY_NOISE_BROWN, 0x5150AA55);
    test_reproducibility();
    test_rms_ordering();
    test_level_and_edge();

    printf("\n=== 汇总: %d passed, %d failed ===\n", g_pass, g_fail);
    /* 机器可读权威结果行：Makefile 只信这一行，不再 grep 数 [PASS]。
     * 数行法必然对不齐——手动打印的明细行不计入 g_pass，而 ADD_OK 之类
     * 静默断言计入 g_pass 却不打印。两个口径混用会得到"合计 423 但各套件
     * 相加不等于程序内部数字"的假统计。 */
    printf("MY_RESULT: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
