# PPT 源码（可重新生成）

路演 PPT 不是手工排的，是脚本生成的。改完文字跑一遍脚本即可重出 PPTX。

## 目录

| 文件 | 说明 |
|---|---|
| `build.js` | 生成脚本（pptxgenjs，15 页全部在这里定义，含每页演讲者备注） |
| `01-gate.png` … `06-me.png` | PPT 里用的产品运行截图（500×1000，来自 `prototype/m.html` 真实截图） |

生成的 PPTX 输出到上级目录：`../眠屿SleepIsle_决赛路演.pptx`。

## 怎么重新生成

```bash
npm install -g pptxgenjs        # 只需一次
cd docs/决赛路演_20261103/source
node build.js                   # 输出 ../眠屿SleepIsle_决赛路演.pptx
```

脚本里的路径是相对于本文件的（`__dirname`），clone 到任何位置都能直接跑；
图片从 `../../../prototype/pets/` 取，截图在本目录。

## 导出 PDF

```bash
soffice --headless --convert-to pdf --outdir .. ../眠屿SleepIsle_决赛路演.pptx
```

Windows 上 `soffice` 可能在 `C:\Program Files\LibreOffice\program\soffice.exe` 或
`C:\Users\<你>\LibreOffice-26\program\soffice.exe`。

## 改团队页（路演前必做）

三处占位，都在 `build.js` 里搜「占位」：

| 位置 | 代码里的锚点 |
|---|---|
| 封面「参赛团队 / 路演人」 | `参赛团队：（占位）` |
| 团队页三张卡片 | `姓名 A` / `姓名 B` / `姓名 C` |
| 结尾「参赛团队（占位）」 | `参赛团队（占位） · 2026 年 11 月` |

改完重跑脚本 + 导出 PDF，两个文件一起替换。

## 设计约定（改之前先看）

- 配色是产品自身色系：底 `#0D0E1A`、卡片 `#161728`、强调 `#E8875A`（陶橙）、
  正文 `#F2EFE8`、次要 `#9A9EB5`
- 字体统一 `微软雅黑`（Windows/macOS Office 都有，避免评委机器替换字体）
- 每页都有演讲者备注（讲稿），改页面内容时记得同步备注
- 数据口径见 `../README.md`，不要在图里写没来源的数字
