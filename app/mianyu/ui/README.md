# 界面层（openvela / LVGL 9）

这一层只在 openvela 工程里编译（`#include <lvgl/lvgl.h>`），PC 侧不参与 ——
PC 的 `make app` 走的是 `hal/sim`，那条路上界面接口全是空实现，见
`hal/sim/mianyu_hal_sim.c` 里"手表界面"那一段。

| 文件 | 里面是什么 |
|---|---|
| `watch_ui.c` / `.h` | **手表界面**：表盘页 + 哄睡页，两页切换、触摸交互、状态刷新 |
| `breathe_lvgl.c` / `.h` | 最早那版只有一个呼吸光晕的渲染壳。现在由 `watch_ui.c` 接管，**不参与编译**，留作移植过程存档 |
| `lv_font_mianyu_16.c` / `_32.c` | 界面用的中文字体。文泉驿微米黑裁的，只含界面上出现过的字 |
| `breathe_lvgl.h` | 呼吸光晕渲染壳的头文件（外驱亮度接口）。同样不参与编译 |

编译清单见 `app/mianyu/CMakeLists.txt`（那边只挂 `watch_ui.c` + 两个字体文件）。

---

## 为什么不用 LVGL 自带的字体

LVGL 内置的 `lv_font_montserrat_*` **不含 CJK 字形**。界面上的
「吸气 / 屏息 / 呼气 / 开始哄睡 / 昨晚睡了 7 小时 21 分」全是中文，
用 montserrat 只会得到一串方块。

所以从文泉驿微米黑（`wqy-microhei.ttc`，GPL v2 + 字体例外）抽字形，
按界面实际用到的字集合裁剪。**只裁用到的字**，所以体积很小：
16px 约 92KB，32px 约 269KB（源码文本，编进固件的是压缩后的点阵）。

## 怎么重新生成

前提：装了 `lv_font_conv`（LVGL 官方的字体转换工具）。

```bash
npm i -g lv_font_conv
```

两个字号都要出，参数完全对称，只有 `--size` 不同：

```bash
wget -O /tmp/wqy-microhei-0.ttf \
  https://github.com/StellarCN/scp_zh/raw/master/fonts/wqy-microhei.ttc

SYMS="昨晚睡了小时分入次夜醒今自动开始点哄返回吸气屏息跟着光棕噪白粉播放音量引导中判定已安抚月日周一三四五六早好正在听简报眠用·秒你的天"

for SZ in 16 32; do
  lv_font_conv \
    --font /tmp/wqy-microhei-0.ttf \
    --size $SZ \
    --bpp 4 \
    --format lvgl \
    --no-compress \
    --range 0x20-0x7f \
    --symbols "$SYMS" \
    --lv-include lvgl/lvgl.h \
    -o app/ui/lv_font_mianyu_${SZ}.c
done
```

生成的文件第 4 行就是这次用的完整参数，**改完参数会体现在文件头里**，
可以直接拿它跟上面的命令比对，确认没跑偏。

### 加字的时候

`--symbols` 里 **少一个字，界面上就是方块**，而且不会报错 —— 这是最容易踩的坑。
在 `watch_ui.c` 里新写了中文文案，就得：

1. 把新字补进 `SYMS`（重复的字不用管）；
2. 重跑上面那段生成脚本；
3. 重新编译，看屏上没有方块。

`--range 0x20-0x7f` 是 ASCII，时间「22:30」、百分比那些数字和标点都在这段里，
不用单独列。

### 字号怎么定的

| 字号 | 用在哪 |
|---|---|
| 32px | 表盘上的大时间、哄睡页的阶段文字（「吸气」「屏息」）—— 这两处是躺着也要能看清的 |
| 16px | 日期、计划、昨晚简报、噪声类型、已用时 —— 都是次要信息 |

最小 16 是有意的下限：再小在 390×450 的屏上就要凑近了看，而这块屏是在床头、
半睡半醒的状态下扫一眼的。

---

## 界面改完怎么验证

串口能验到的：对象建起来了、渲染循环帧率正常、触摸挂上了。
**屏上实际亮成什么样，串口验不了，得人工看板。**

详细的验证方法、实测数字、以及已知的板级诊断（`[co5300][readback] NO MATCH`
那条是 QSPI 读回假警报的概率很大）都写在
[`docs/手表界面_真机验证_20260915.md`](../../docs/手表界面_真机验证_20260915.md)。
