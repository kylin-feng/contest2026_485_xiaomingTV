/* 眠屿 SleepIsle S1 · 创新创业决赛路演 · 15 页
 * 深夜主题（产品自身配色）：BG 深夜 / CARD 靛卡 / TERRA 陶橙强调
 */
const pptxgen = require("pptxgenjs");

const W = 13.33, H = 7.5, M = 0.6;
const BG = "0D0E1A", CARD = "161728", CARD2 = "1F2138";
const TERRA = "E8875A", TERRA_TINT = "31221B";
const VIOLET = "8C7FE8", VIOLET_DIM = "5A51A8";
const TEXT = "F2EFE8", MUTED = "9A9EB5", MUTED2 = "7E829A", LINE = "2A2C44";
const F = "微软雅黑";

const path = require("path");
const here = __dirname;                                   // docs/决赛路演_20261103/source
const SHOTS = here;                                       // 截图就在本目录
const PETS = path.join(here, "..", "..", "..", "prototype", "pets");
const OUT = path.join(here, "..", "眠屿SleepIsle_决赛路演.pptx");

const p = new pptxgen();
p.layout = "LAYOUT_WIDE";
p.author = "眠屿 SleepIsle";
p.title = "眠屿 SleepIsle S1 · 决赛路演";

/* ---------- 通用元件 ---------- */
function pageChrome(s, idx, kicker) {
  s.background = { color: BG };
  if (kicker) s.addText(kicker, { x: M, y: 0.38, w: 8, h: 0.32, fontFace: F, fontSize: 12.5, bold: true, color: TERRA, charSpacing: 3, margin: 0 });
  s.addText(String(idx).padStart(2, "0") + " / 15", { x: W - 1.6, y: 7.02, w: 1.0, h: 0.3, fontFace: F, fontSize: 12, color: MUTED2, align: "right", margin: 0 });
}
function title(s, t, sub) {
  s.addText(t, { x: M, y: 0.68, w: W - 2 * M, h: 0.78, fontFace: F, fontSize: 30, bold: true, color: TEXT, margin: 0 });
  if (sub) s.addText(sub, { x: M, y: 1.52, w: W - 2 * M, h: 0.4, fontFace: F, fontSize: 15, color: MUTED, margin: 0 });
}
function hline(s, x, y, w) {
  s.addShape(p.shapes.LINE, { x, y, w, h: 0, line: { color: LINE, width: 0.75 } });
}
/* 行式列表：加粗导语 + 说明，细线分隔 */
function rows(s, x, y, w, items, pitch, headSize, bodySize) {
  items.forEach((it, i) => {
    const yy = y + i * pitch;
    s.addText(it[0], { x, y: yy, w, h: 0.4, fontFace: F, fontSize: headSize, bold: true, color: TEXT, margin: 0 });
    s.addText(it[1], { x, y: yy + 0.38, w, h: pitch - 0.55, fontFace: F, fontSize: bodySize, color: MUTED, margin: 0, lineSpacingMultiple: 1.12 });
    if (i < items.length - 1) hline(s, x, yy + pitch - 0.12, w);
  });
}

/* ============ 01 封面 ============ */
(() => {
  const s = p.addSlide();
  s.background = { color: BG };
  s.addImage({ path: PETS + "/planet.png", x: 7.5, y: 0, w: 5.83, h: 7.5, sizing: { type: "cover", w: 5.83, h: 7.5 } });
  s.addText("2026 · 创新创业大赛 · 决赛路演", { x: M, y: 1.55, w: 6.4, h: 0.35, fontFace: F, fontSize: 13.5, bold: true, color: TERRA, charSpacing: 3, margin: 0 });
  s.addText("眠屿 SleepIsle", { x: M, y: 2.0, w: 6.6, h: 1.05, fontFace: F, fontSize: 54, bold: true, color: TEXT, margin: 0 });
  s.addText("S1", { x: M, y: 3.02, w: 2, h: 0.5, fontFace: F, fontSize: 26, bold: true, color: TERRA, charSpacing: 4, margin: 0 });
  s.addText("住在一台复古掌机里的 AI 哄睡电子宠物", { x: M, y: 3.78, w: 6.5, h: 0.5, fontFace: F, fontSize: 22, color: TEXT, margin: 0 });
  s.addText("会聊天 · 会讲故事 · 会督促睡觉 · 整夜值守音景", { x: M, y: 4.32, w: 6.5, h: 0.4, fontFace: F, fontSize: 16, color: MUTED, margin: 0 });
  s.addText("睡觉能赚金币，金币能养它——先有关系，再有好眠。", { x: M, y: 4.74, w: 6.5, h: 0.4, fontFace: F, fontSize: 16, color: MUTED, margin: 0 });
  s.addText([
    { text: "参赛团队：（占位）", options: { breakLine: true } },
    { text: "路演人：（占位） · 2026 年 11 月", options: {} },
  ], { x: M, y: 5.95, w: 4.6, h: 0.7, fontFace: F, fontSize: 13.5, color: MUTED2, margin: 0, lineSpacingMultiple: 1.3 });
  s.addNotes("开场 30 秒：各位评委好，我们是眠屿 SleepIsle。请先记住一句话——睡觉能赚金币，金币能养它。它是一款住在一台复古掌机里的 AI 哄睡电子宠物：会聊天、会讲故事、会督促你睡觉，整夜值守音景。今天我要讲的是：我们如何把『被验证过的睡眠硬件生意』和『没人做出来的情感陪伴』合成为一个产品。");
})();

/* ============ 02 痛点 ============ */
(() => {
  const s = p.addSlide();
  pageChrome(s, 2, "01 / 痛点");
  title(s, "睡眠经济很热，但产品只会“看”，不会“陪”");
  s.addText("3 亿+", { x: M, y: 2.0, w: 5.0, h: 1.15, fontFace: F, fontSize: 72, bold: true, color: TERRA, margin: 0 });
  s.addText("中国存在睡眠障碍的人群规模", { x: M, y: 3.2, w: 5.0, h: 0.35, fontFace: F, fontSize: 15, color: MUTED, margin: 0 });
  s.addText("睡不着的人很多，\n但没有一台设备在夜里真正陪着他们。", { x: M, y: 4.05, w: 5.0, h: 1.0, fontFace: F, fontSize: 19, color: TEXT, margin: 0, lineSpacingMultiple: 1.35 });
  s.addText("来源：中国睡眠研究会公开数据", { x: M, y: 6.55, w: 5.0, h: 0.3, fontFace: F, fontSize: 12, color: MUTED2, margin: 0 });
  rows(s, 6.2, 1.95, 6.5, [
    ["监测类只报告，不干预", "戒指、手环把你的睡眠打成一个分数，告诉你“昨晚睡得不好”，然后——什么也不做。"],
    ["干预类各自为政，不懂状态", "白噪音机、夜灯、音箱互不相通，不知道你此刻是清醒、浅睡，还是刚被雷声惊醒。"],
    ["哄睡首先是情绪问题", "高压都市人躺下后思维反刍、手机越刷越精神；孩子要“有人陪着”才肯睡。现有产品没有温度。"],
  ], 1.62, 18, 15);
  s.addNotes("痛点用三个“不”讲：监测类不干预——Oura 和手环只给分数；干预类不联动——白噪音机和夜灯互不相通；而哄睡本质是情绪问题——大人是思维反刍，孩子是要人陪。三亿人的市场里，没有一台设备在夜里真正『陪着』用户。");
})();

/* ============ 03 市场与生态位 ============ */
(() => {
  const s = p.addSlide();
  pageChrome(s, 3, "02 / 市场与生态位");
  title(s, "赚钱公式已被验证，米家生态缺一个“睡眠编排中枢”");
  s.addText("≈ 2 亿美元", { x: M, y: 1.85, w: 5.2, h: 0.75, fontFace: F, fontSize: 40, bold: true, color: TERRA, margin: 0 });
  s.addText("Hatch Restore · 2025 年营收，其中内容订阅约 300 万美元/月", { x: M, y: 2.6, w: 5.2, h: 0.55, fontFace: F, fontSize: 14, color: MUTED, margin: 0, lineSpacingMultiple: 1.15 });
  s.addText("≈ 12 亿美元", { x: M, y: 3.35, w: 5.2, h: 0.75, fontFace: F, fontSize: 40, bold: true, color: VIOLET, margin: 0 });
  s.addText("Oura 智能戒指 · 截至 2026 年 6 月的近 12 个月营收，同比 +74%，已递交 IPO；会员一年留存 85%", { x: M, y: 4.1, w: 5.2, h: 0.6, fontFace: F, fontSize: 13.5, color: MUTED, margin: 0, lineSpacingMultiple: 1.15 });
  s.addText("C 端唯一被验证的路径：硬件走量做入口，订阅做利润主体。", { x: M, y: 4.95, w: 5.2, h: 0.8, fontFace: F, fontSize: 17, bold: true, color: TEXT, margin: 0, lineSpacingMultiple: 1.3 });
  s.addText("来源：Inc.（Hatch，2026-05）；Sacra / Yahoo Finance（Oura）", { x: M, y: 6.55, w: 5.4, h: 0.3, fontFace: F, fontSize: 12, color: MUTED2, margin: 0 });
  // 右：生态位地图（自上而下五层）
  const LX = 6.35, LW = 6.35;
  s.addText("米家睡眠生态 · 生态位地图", { x: LX, y: 1.78, w: LW, h: 0.32, fontFace: F, fontSize: 13, color: MUTED, charSpacing: 2, margin: 0 });
  const layers = [
    ["编排层 · 睡眠大脑", "完全空白 → 眠屿 S1 的定位与护城河", true],
    ["干预层 · 灯/声/温/帘", "米家泛设备均为通用件，无睡眠场景专职产品", false],
    ["穿戴层 · 戒指", "Oura / 今日宜休，数据可作为可选输入", false],
    ["感知层 · 床头雷达", "Sleepal（449 美元，医疗级精度单品）", false],
    ["床上层 · 床垫/枕头", "趣睡 8H（米家系上市公司）", false],
  ];
  layers.forEach((l, i) => {
    const y = 2.18 + i * 0.92;
    s.addShape(p.shapes.ROUNDED_RECTANGLE, { x: LX, y, w: LW, h: 0.8, rectRadius: 0.09, fill: { color: l[2] ? TERRA : CARD }, line: { color: l[2] ? TERRA : LINE, width: l[2] ? 1 : 0.75 } });
    s.addText(l[0], { x: LX + 0.28, y: y + 0.1, w: 2.35, h: 0.6, fontFace: F, fontSize: 14.5, bold: true, color: l[2] ? "14100C" : TEXT, valign: "middle", margin: 0 });
    s.addText(l[1], { x: LX + 2.78, y: y + 0.1, w: LW - 3.0, h: 0.6, fontFace: F, fontSize: 12.5, color: l[2] ? "2A1B12" : MUTED, valign: "middle", margin: 0, lineSpacingMultiple: 1.1 });
  });
  s.addNotes("市场两层逻辑：第一，赛道已被验证——Hatch 一年两亿美金，Inc. 今年五月还在报道青少年弃掉手机去买它 170 美元的 Restore；Oura 近十二个月营收十二亿美元、同比涨 74%，已经递交 IPO，会员一年留存 85%——这个留存率就是订阅模式的底气。第二，看米家生态的分层地图：床上有 8H、感知有 Sleepal、穿戴有 Oura，干预层是通用件，而『编排层』完全空白——没有人做那个在正确时间做正确动作的大脑。这个空白生态位就是眠屿。");
})();

/* ============ 04 解决方案 ============ */
(() => {
  const s = p.addSlide();
  pageChrome(s, 4, "03 / 解决方案");
  title(s, "眠屿 S1：不是白噪音音箱，而是整夜值守的哄睡 Agent");
  s.addText("感知你的呼吸与卧室环境，把光、声、温度、窗帘编排成一夜好眠——而且，它是一只会陪你的宠物。", { x: M, y: 1.52, w: 12.1, h: 0.42, fontFace: F, fontSize: 16, color: MUTED, margin: 0 });
  const cols = [
    ["01", "情感陪伴", "五只宝可梦风电子宠物：聊天、讲故事、督促睡觉、夜醒时耳语回应。\n先有关系，再有干预。"],
    ["02", "睡眠编排", "Agent 每 3 秒一轮感知→决策→执行：五层音景实时合成、日落日出灯效、\n米家全屋联动。"],
    ["03", "被动学习", "睡前 25 秒“要不要复习一下”：弱词本与古诗轻声磨耳朵。\n家长买单的理由，也是订阅内容的载体。"],
  ];
  cols.forEach((c, i) => {
    const x = M + i * 4.18;
    s.addText(c[0], { x, y: 2.15, w: 1.2, h: 0.65, fontFace: F, fontSize: 34, bold: true, color: TERRA, margin: 0 });
    s.addText(c[1], { x, y: 2.85, w: 3.85, h: 0.45, fontFace: F, fontSize: 21, bold: true, color: TEXT, margin: 0 });
    s.addText(c[2], { x, y: 3.38, w: 3.85, h: 1.7, fontFace: F, fontSize: 15, color: MUTED, margin: 0, lineSpacingMultiple: 1.25 });
  });
  // 底部：闭环 chevrons
  const steps = ["感知 SENSE", "记忆 MEMORY", "决策 THINK", "执行 ACT"];
  steps.forEach((t, i) => {
    s.addShape(p.shapes.CHEVRON, { x: M + i * 3.05, y: 5.55, w: 3.15, h: 0.78, fill: { color: i === 3 ? TERRA : CARD2 }, line: { color: i === 3 ? TERRA : LINE, width: 0.75 } });
    s.addText(t, { x: M + i * 3.05 + 0.3, y: 5.55, w: 2.75, h: 0.78, fontFace: F, fontSize: 16, bold: true, color: i === 3 ? "14100C" : TEXT, align: "center", valign: "middle", margin: 0 });
  });
  s.addText("每 3 秒一轮闭环；执行动作改变环境后，传感器下一轮回读、自动校准（闭环自检）。", { x: M, y: 6.5, w: 12.1, h: 0.35, fontFace: F, fontSize: 13.5, color: MUTED2, margin: 0 });
  s.addNotes("解决方案一句话：不是白噪音音箱，而是整夜值守的哄睡 Agent。三个支柱：情感陪伴——用户为了宠物留下来；睡眠编排——Agent 真的在干活；被动学习——家长愿意付费的理由。底部是它的运行方式：感知、记忆、决策、执行，每 3 秒一轮，执行完传感器回读校准。这不是定时器，是闭环。");
})();

/* ============ 05 产品进展 ============ */
(() => {
  const s = p.addSlide();
  pageChrome(s, 5, "04 / 产品进展");
  title(s, "软件原型 v1.8 已全功能跑通：真语音 · 真音频 · 真养成");
  s.addShape(p.shapes.ROUNDED_RECTANGLE, { x: M - 0.08, y: 1.62, w: 2.86, h: 5.32, rectRadius: 0.12, fill: { color: CARD }, line: { color: LINE, width: 0.75 } });
  s.addImage({ path: SHOTS + "/02-console.png", x: M, y: 1.7, w: 2.66, h: 5.16 });
  const stats = [["18", "个版本迭代 v0.1 → v1.8"], ["5", "只宠物 · 各有音高与机壳变色"], ["6", "个实时合成音景"], ["10", "枚成就徽章"], ["6", "个学习模块 · 主动 + 被动"], ["0", "依赖 · 单文件可嵌任何壳"]];
  stats.forEach((t, i) => {
    const x = 4.05 + (i % 3) * 3.0, y = 1.78 + Math.floor(i / 3) * 1.42;
    s.addText(t[0], { x, y, w: 1.15, h: 0.85, fontFace: F, fontSize: 44, bold: true, color: i === 0 ? TERRA : TEXT, margin: 0 });
    s.addText(t[1], { x: x + 1.18, y: y + 0.16, w: 1.75, h: 0.7, fontFace: F, fontSize: 13.5, color: MUTED, margin: 0, lineSpacingMultiple: 1.12 });
  });
  hline(s, 4.05, 4.62, 8.75);
  const feats = [
    "Web Audio 五层实时合成 + 卷积混响——不是录音循环，永不重复",
    "语音连续对话闭环：听→想→说→继续听；LLM 优先，内置温柔应答兜底",
    "宠物养成闭环：三围 + XP/LV + 金币商城；按时睡觉 +30 币、按时起床 +30 币",
  ];
  feats.forEach((t, i) => {
    s.addText(t, { x: 4.05, y: 4.8 + i * 0.42, w: 8.75, h: 0.4, fontFace: F, fontSize: 14.5, color: TEXT, margin: 0 });
  });
  const petRow = ["puff", "pika", "char", "gengar", "xiaomian"];
  petRow.forEach((f, i) => {
    s.addImage({ path: PETS + "/" + f + ".png", x: 4.05 + i * 1.0, y: 6.12, w: 0.78, h: 0.78 });
  });
  s.addText("胖丁 · 皮卡丘 · 小火龙 · 耿鬼 · 小眠", { x: 9.3, y: 6.32, w: 3.5, h: 0.4, fontFace: F, fontSize: 13.5, color: MUTED, valign: "middle", margin: 0 });
  s.addNotes("这不是概念图，是已经跑起来的产品。左侧是真实运行截图：Game Boy 形态掌机、胖丁、Agent 值守动态。18 个版本迭代出来：五只宠物、六音景、十成就、六学习模块，零依赖单文件。技术上三个“真”：真 Web Audio 合成、真语音对话闭环、真养成经济——按时睡觉赚金币，金币养宠物。所有数据本地 localStorage，随时可塞进 WebView 壳。");
})();

/* ============ 06 四屏演示 ============ */
(() => {
  const s = p.addSlide();
  pageChrome(s, 6, "05 / 产品演示");
  title(s, "从首开到商城：“睡觉赚金币，金币养宠物”的完整闭环");
  const shots = [
    ["01-gate.png", "首开校验", "称呼与身份校验 → 穿越故事六幕 → 选宠仪式"],
    ["03-scape.png", "音景库", "6 个实时合成音景 · 15/30/45 分钟定时关闭"],
    ["04-study.png", "学习乐园", "主动 6 模块 + 被动磨耳朵 · 过点课本被“没收”"],
    ["05-shop.png", "金币商城", "按时睡 +30 · 按时起 +30 · 6 件商品即买即用"],
  ];
  const tw = 2.16, gap = 0.52, x0 = (W - (tw * 4 + gap * 3)) / 2;
  shots.forEach((t, i) => {
    const x = x0 + i * (tw + gap);
    s.addShape(p.shapes.ROUNDED_RECTANGLE, { x: x - 0.06, y: 1.72, w: tw + 0.12, h: 4.4, rectRadius: 0.1, fill: { color: CARD }, line: { color: LINE, width: 0.75 } });
    s.addImage({ path: SHOTS + "/" + t[0], x, y: 1.8, w: tw, h: 4.24 });
    s.addText(t[1], { x: x - 0.1, y: 6.22, w: tw + 0.2, h: 0.35, fontFace: F, fontSize: 16, bold: true, color: TEXT, align: "center", margin: 0 });
    s.addText(t[2], { x: x - 0.15, y: 6.58, w: tw + 0.3, h: 0.62, fontFace: F, fontSize: 12.5, color: MUTED, align: "center", margin: 0, lineSpacingMultiple: 1.15 });
  });
  s.addNotes("四屏走一遍用户旅程：首开不是表单，是一段“穿越回眠屿星”的故事，孩子会主动想走完；音景库六个场景全部实时合成，可定时关闭；学习乐园白天是主动学习，过了睡觉点课本会被宠物“没收”；最后商城把行为闭环补上——按时睡、按时起都有金币，金币买晚安茶喂宠物。行为改变，靠的是感情，不是自律。");
})();

/* ============ 07 Agent ============ */
(() => {
  const s = p.addSlide();
  pageChrome(s, 7, "06 / 核心技术 · Agent");
  title(s, "每 3 秒一轮闭环，六条核心规则已在模拟器全链路验证");
  // 左：2×2 闭环
  const bx = [[M, 1.95, "感知 SENSE", "雷达呼吸/体动 · 麦克风噪音与鼾声 · 温湿光"], [3.35, 1.95, "记忆 MEMORY", "个人睡眠基线与偏好 · 每晚更新"], [3.35, 3.85, "决策 THINK", "V1 规则引擎 60+ 条 → V2 端侧小模型 → V3 云端大模型"], [M, 3.85, "执行 ACT", "五层音景 · 灯光 PWM · TTS 耳语 · 米家编排"]];
  bx.forEach(b => {
    s.addShape(p.shapes.ROUNDED_RECTANGLE, { x: b[0], y: b[1], w: 2.55, h: 1.5, rectRadius: 0.1, fill: { color: CARD }, line: { color: LINE, width: 0.75 } });
    s.addText(b[2], { x: b[0] + 0.18, y: b[1] + 0.12, w: 2.2, h: 0.35, fontFace: F, fontSize: 15.5, bold: true, color: TERRA, margin: 0 });
    s.addText(b[3], { x: b[0] + 0.18, y: b[1] + 0.5, w: 2.2, h: 0.9, fontFace: F, fontSize: 12, color: MUTED, margin: 0, lineSpacingMultiple: 1.15 });
  });
  s.addShape(p.shapes.LINE, { x: 3.15, y: 2.7, w: 0.2, h: 0, line: { color: TERRA, width: 1.75, endArrowType: "triangle" } });
  s.addShape(p.shapes.LINE, { x: 4.6, y: 3.45, w: 0, h: 0.4, line: { color: TERRA, width: 1.75, endArrowType: "triangle" } });
  s.addShape(p.shapes.LINE, { x: 3.15, y: 4.62, w: 0.2, h: 0, flipH: true, line: { color: TERRA, width: 1.75, endArrowType: "triangle" } });
  s.addShape(p.shapes.LINE, { x: 1.87, y: 3.45, w: 0, h: 0.4, flipV: true, line: { color: TERRA, width: 1.75, endArrowType: "triangle" } });
  s.addText("闭环自检：执行后传感器回读 → 自动校准", { x: M, y: 5.55, w: 5.4, h: 0.35, fontFace: F, fontSize: 13, color: MUTED2, margin: 0 });
  s.addText("验证记录：2026-10-09 模拟器完成“晚安→注入噪音/鼾声/翻身→深睡→日出”全链路自动化测试，规则触发与执行全部符合预期。", { x: M, y: 6.15, w: 5.7, h: 0.75, fontFace: F, fontSize: 12.5, color: MUTED2, margin: 0, lineSpacingMultiple: 1.2 });
  // 右：规则
  s.addText("V1 核心决策规则（节选）", { x: 7.0, y: 1.82, w: 5.7, h: 0.32, fontFace: F, fontSize: 13, bold: true, color: MUTED, charSpacing: 2, margin: 0 });
  const rules = [
    ["R1", "环境噪音 > 45dB", "遮蔽层 +12%（上限 50%），环境安静后 5 分钟缓收"],
    ["R2", "检测到鼾声", "遮蔽层 +5% 掩蔽，不打扰睡者本人"],
    ["R3", "检测到体动（翻身）", "场景层闪避 3 秒——声音不抢占注意力"],
    ["R4", "睡眠阶段切换", "深睡期场景层降至 25%；REM 期保留 40% 掩蔽"],
    ["R5", "呼吸 <10 次/分且入睡", "启动 20 分钟整晚渐弱计划 → 静默纯监测"],
  ];
  rules.forEach((r, i) => {
    const y = 2.22 + i * 0.94;
    s.addText(r[0], { x: 7.0, y, w: 0.55, h: 0.4, fontFace: F, fontSize: 17, bold: true, color: TERRA, margin: 0 });
    s.addText(r[1], { x: 7.62, y: y + 0.02, w: 5.1, h: 0.35, fontFace: F, fontSize: 14.5, bold: true, color: TEXT, margin: 0 });
    s.addText(r[2], { x: 7.62, y: y + 0.37, w: 5.1, h: 0.42, fontFace: F, fontSize: 12.5, color: MUTED, margin: 0 });
    if (i < 4) hline(s, 7.0, y + 0.86, 5.72);
  });
  s.addNotes("核心讲两件事。第一，架构：感知、记忆、决策、执行，每 3 秒一轮，执行完传感器回读——自己播放的音景如果引起体动增多，会自动降音量，这是闭环自检。第二，右侧五条规则是真实跑过的：噪音来了推遮蔽层、打鼾轻掩蔽、翻身时声音闪避、深睡期换挡、入睡后二十分钟渐弱到静默。10 月 9 日在模拟器做了全链路自动化验证，全部符合预期。");
})();

/* ============ 08 音景引擎 + 状态机 ============ */
(() => {
  const s = p.addSlide();
  pageChrome(s, 8, "07 / 核心技术 · 音景引擎");
  title(s, "不是录音循环：五层混音台实时程序合成，随睡眠阶段换挡");
  const layers = [
    ["底噪 base", "棕噪音（低通 350Hz）", "整夜铺底 · 深睡期唯一保留层", 1.0, "4A4480"],
    ["场景 tex", "雨 / 海浪 / 篝火 / 夜风", "入睡期主音景 · 随阶段换挡", 0.72, "8C7FE8"],
    ["遮蔽 mask", "棕噪音（低通 600Hz）", "突发噪音掩蔽 · 由 R1/R2 驱动", 0.5, TERRA],
    ["点缀 detail", "随机雨滴 / 柴火噼啪", "永不重复 · 真实感与“活感”", 0.34, "565A8C"],
    ["晨鸟 birds", "程序合成鸟鸣", "日出唤醒", 0.2, "565A8C"],
  ];
  layers.forEach((l, i) => {
    const y = 1.95 + i * 0.94;
    s.addText(l[0], { x: M, y, w: 1.75, h: 0.35, fontFace: F, fontSize: 15.5, bold: true, color: TEXT, margin: 0 });
    s.addText(l[1] + " · " + l[2], { x: M, y: y + 0.36, w: 5.9, h: 0.32, fontFace: F, fontSize: 12.5, color: MUTED, margin: 0 });
    s.addShape(p.shapes.ROUNDED_RECTANGLE, { x: M, y: y + 0.72, w: 5.9 * l[3], h: 0.1, rectRadius: 0.05, fill: { color: l[4] } });
  });
  s.addText("首发音景：雨夜木屋 · 海边小屋 · 林间篝火 · 纯底噪 · 夏夜虫鸣 · 老式风扇｜Pro 订阅每周新增配方", { x: M, y: 6.72, w: 7.2, h: 0.35, fontFace: F, fontSize: 12.5, color: MUTED2, margin: 0 });
  // 右：状态机
  s.addText("一夜状态机", { x: 8.0, y: 1.82, w: 4.6, h: 0.32, fontFace: F, fontSize: 13, bold: true, color: MUTED, charSpacing: 2, margin: 0 });
  const states = ["待机 IDLE", "日落陪伴 SUNSET", "入睡护航 WIND_DOWN", "深睡守护 DEEP_GUARD ⇄ REM 掩蔽", "日出唤醒 SUNRISE", "晨间报告 REPORT"];
  states.forEach((t, i) => {
    const y = 2.24 + i * 0.76;
    const hot = i === 3;
    s.addShape(p.shapes.ROUNDED_RECTANGLE, { x: 8.0, y, w: 4.65, h: 0.56, rectRadius: 0.09, fill: { color: hot ? TERRA_TINT : CARD }, line: { color: hot ? TERRA : LINE, width: hot ? 1 : 0.75 } });
    s.addText(t, { x: 8.2, y, w: 4.3, h: 0.56, fontFace: F, fontSize: 14, bold: hot, color: hot ? TERRA : TEXT, valign: "middle", margin: 0 });
    if (i < 5) s.addShape(p.shapes.LINE, { x: 10.3, y: y + 0.56, w: 0, h: 0.2, line: { color: MUTED2, width: 1, endArrowType: "triangle" } });
  });
  s.addText("异常回路：任意睡眠态可切入“夜醒陪护”，10 分钟无响应自动回落", { x: 8.0, y: 6.66, w: 4.7, h: 0.3, fontFace: F, fontSize: 12, color: MUTED2, margin: 0 });
  s.addNotes("为什么不用录音循环？因为哄睡需要“活”的声音。我们把音景拆成五层混音台：底噪整夜铺底、场景层随阶段换挡、遮蔽层由噪音规则驱动、点缀层随机永不重复、晨鸟层负责唤醒——全部程序实时合成。右边是一夜状态机：从日落陪伴到晨间报告，夜里任何时刻可切入夜醒陪护。新音景只需要加一个配方，这就是订阅内容的供给成本优势。");
})();

/* ============ 09 护城河 ============ */
(() => {
  const s = p.addSlide();
  pageChrome(s, 9, "08 / 差异化护城河");
  title(s, "最难复制的两件事：自学习的记忆层，与“数据不出卧室”");
  s.addText("记忆层 · 每晚自学习", { x: M, y: 1.85, w: 5.9, h: 0.45, fontFace: F, fontSize: 21, bold: true, color: TEXT, margin: 0 });
  rows(s, M, 2.5, 5.9, [
    ["“雨声 vs 海浪”", "哪种让呼吸率下降更快，次日自动采用——每晚 A/B 微调。"],
    ["“周四入睡最难”", "周期模式识别 → 自动把周四的日落提前 15 分钟。"],
    ["“海浪让你平均快 6 分钟入睡”", "每周一条个性化结论；Pro 页只强调一个数字：已为你生成 N 条专属建议。"],
  ], 1.18, 16, 13.5);
  s.addText("14 晚", { x: M, y: 6.05, w: 2.3, h: 0.8, fontFace: F, fontSize: 46, bold: true, color: TERRA, margin: 0 });
  s.addText("记忆层形成稳定推荐的目标收敛时间", { x: 2.95, y: 6.28, w: 3.6, h: 0.45, fontFace: F, fontSize: 13.5, color: MUTED, valign: "middle", margin: 0 });
  s.addShape(p.shapes.LINE, { x: 6.75, y: 1.95, w: 0, h: 4.9, line: { color: LINE, width: 0.75 } });
  s.addText("数据不出卧室", { x: 7.35, y: 2.0, w: 5.3, h: 0.75, fontFace: F, fontSize: 38, bold: true, color: TEXT, margin: 0 });
  s.addText("印在包装盒上的隐私承诺", { x: 7.35, y: 2.8, w: 5.3, h: 0.35, fontFace: F, fontSize: 14, color: TERRA, margin: 0 });
  const priv = [
    "无摄像头——物理上不存在，而非“可以关闭”",
    "麦克风物理静音拨杆——机械断电，不依赖软件承诺",
    "雷达只输出呼吸率 / 体动特征值，原始点云端侧即时销毁",
    "睡眠分期与 Agent 决策全部端侧完成；云端仅匿名周报，可一键关闭",
  ];
  priv.forEach((t, i) => {
    const y = 3.45 + i * 0.78;
    s.addShape(p.shapes.RECTANGLE, { x: 7.35, y: y + 0.075, w: 0.14, h: 0.14, fill: { color: TERRA } });
    s.addText(t, { x: 7.65, y, w: 5.0, h: 0.68, fontFace: F, fontSize: 14.5, color: MUTED, margin: 0, valign: "top", lineSpacingMultiple: 1.15 });
  });
  s.addNotes("竞品最难复制两件事。第一是记忆层：它每晚做 A/B——雨声和海浪哪个让你呼吸率降得更快，第二天自动用更好的那个；它记得你周四最难入睡，就自动提前日落。十四晚形成稳定推荐，这些睡眠记忆数据是订阅转化的锚点，也是护城河。第二是隐私：卧室设备的隐私是生死线，我们把它做成营销资产——无摄像头是物理不存在，麦克风是机械断电拨杆，雷达点云端侧销毁，“数据不出卧室”直接印在包装盒上。");
})();

/* ============ 10 硬件与编排 ============ */
(() => {
  const s = p.addSlide();
  pageChrome(s, 10, "09 / 硬件形态与生态");
  title(s, "床头一轮“小月亮”：openvela 驱动的全屋睡眠编排");
  // 左：小月亮图形 + 规格
  s.addShape(p.shapes.OVAL, { x: 1.15, y: 1.85, w: 2.5, h: 2.5, fill: { color: "F3E9D2" }, line: { color: "F3E9D2", width: 0 } });
  s.addShape(p.shapes.RECTANGLE, { x: 0.95, y: 3.1, w: 2.9, h: 1.3, fill: { color: BG }, line: { color: BG, width: 0 } });
  s.addShape(p.shapes.ROUNDED_RECTANGLE, { x: 1.0, y: 3.06, w: 2.8, h: 0.34, rectRadius: 0.12, fill: { color: "3B3D55" }, line: { color: "3B3D55", width: 0 } });
  s.addShape(p.shapes.OVAL, { x: 1.55, y: 2.35, w: 0.7, h: 0.7, fill: { color: TERRA, transparency: 62 } });
  const specs = [
    "⌀120 × 86 mm · 约 480 g · USB-C 供电 · 织物网罩“去科技感”",
    "60GHz 毫米波雷达（呼吸/体动/在离床）· 4 麦远场阵列 · 温湿光",
    "RGBW 环形柔光灯 1800–4000K · 1.75 英寸全频扬声器（夜间限幅 65dB）",
    "BOM ≈ 220 元 · 端侧 NPU 运行睡眠分期 · 系统 openvela（小米 Vela 开源版）",
    "三个不做：不做屏幕 · 不做摄像头 · 不做床垫/枕头/戒指单品",
  ];
  specs.forEach((t, i) => {
    const y = 3.75 + i * 0.64;
    s.addShape(p.shapes.RECTANGLE, { x: M, y: y + 0.09, w: 0.13, h: 0.13, fill: { color: VIOLET } });
    s.addText(t, { x: 0.88, y, w: 4.35, h: 0.6, fontFace: F, fontSize: 12.5, color: MUTED, margin: 0, lineSpacingMultiple: 1.1 });
  });
  // 右：一夜编排时间轴
  const TX0 = 6.35, TX1 = 12.3, TY = 3.3;
  s.addText("一夜编排时间轴 · 本机动作 + 米家联动", { x: 5.55, y: 1.82, w: 7.1, h: 0.32, fontFace: F, fontSize: 13, bold: true, color: MUTED, charSpacing: 2, margin: 0 });
  s.addShape(p.shapes.LINE, { x: TX0, y: TY, w: TX1 - TX0, h: 0, line: { color: LINE, width: 1.5 } });
  const nodes = [
    ["21:30", "日落陪伴", "暖光 8 分钟渐暗 · 音景淡入", "关大灯 · 合窗帘"],
    ["入睡期", "入睡护航", "音景节奏随呼吸放缓", "——"],
    ["入睡后", "深睡守护", "渐弱至静默 · 仅留薄底噪", "空调 24.5℃"],
    ["03:00", "夜醒哨兵", "耳语回应 + 25% 音景陪护", "地灯 5%"],
    ["06:40", "日出唤醒", "浅睡窗口光渐亮 + 鸟鸣", "窗帘开 10%"],
  ];
  nodes.forEach((n, i) => {
    const cx = TX0 + (i * (TX1 - TX0)) / 4;
    s.addShape(p.shapes.OVAL, { x: cx - 0.09, y: TY - 0.09, w: 0.18, h: 0.18, fill: { color: i === 4 ? TERRA : VIOLET } });
    s.addText(n[0], { x: cx - 0.71, y: TY - 0.62, w: 1.42, h: 0.35, fontFace: F, fontSize: 15, bold: true, color: i === 4 ? TERRA : TEXT, align: "center", margin: 0 });
    s.addText(n[1], { x: cx - 0.71, y: TY + 0.22, w: 1.42, h: 0.32, fontFace: F, fontSize: 14, bold: true, color: TEXT, align: "center", margin: 0 });
    s.addText(n[2], { x: cx - 0.73, y: TY + 0.56, w: 1.46, h: 0.95, fontFace: F, fontSize: 12, color: MUTED, align: "center", margin: 0, lineSpacingMultiple: 1.12 });
    s.addText(n[3], { x: cx - 0.73, y: TY + 1.6, w: 1.46, h: 0.75, fontFace: F, fontSize: 12, color: VIOLET, align: "center", margin: 0, lineSpacingMultiple: 1.12 });
  });
  s.addText("编排对象：米家灯光 / 窗帘 / 空调 / 加湿器 / 8H 床垫 / 手环数据 · 网络中断时全部核心功能本地可用", { x: 5.55, y: 6.35, w: 7.1, h: 0.55, fontFace: F, fontSize: 12.5, color: MUTED2, margin: 0, lineSpacingMultiple: 1.2 });
  s.addNotes("硬件形态是一轮放在床头的小月亮：直径 12 公分、480 克，织物网罩走“去科技感”家居风。里面是 60GHz 毫米波雷达加四麦阵列——无接触测呼吸，无隐私负担。BOM 大约 220 元，跑 openvela。真正值钱的是右边这条时间轴：九点半日落陪伴，联动关灯合窗帘；入睡后空调降到 24.5 度；半夜雷声来了遮蔽层顶上；清晨在浅睡窗口用光和鸟鸣把你叫醒——它指挥的是用户家里已有的米家设备，这是 Hatch 做不到的。");
})();

/* ============ 11 商业模式 ============ */
(() => {
  const s = p.addSlide();
  pageChrome(s, 11, "10 / 商业模式");
  title(s, "硬件走量做入口，订阅做利润——Hatch 验证过的公式 A");
  const items = [
    ["599–799 元", "硬件零售价 · BOM ≈ 220 元，硬件毛利为正（入口）", TERRA],
    ["15 元 / 月", "Pro 订阅：每周新音景配方 · 被动学习内容库 · 个性化建议（利润主体）", TEXT],
    ["B 端二期", "养老院 / 酒店 / 医院按床位付费（Sleepace 路径，兼作风险预案）", TEXT],
  ];
  items.forEach((t, i) => {
    const y = 1.9 + i * 1.42;
    s.addText(t[0], { x: M, y, w: 3.1, h: 0.7, fontFace: F, fontSize: 33, bold: true, color: t[2], margin: 0 });
    s.addText(t[1], { x: M, y: y + 0.72, w: 5.9, h: 0.6, fontFace: F, fontSize: 13.5, color: MUTED, margin: 0, lineSpacingMultiple: 1.15 });
    if (i < 2) hline(s, M, y + 1.32, 5.9);
  });
  s.addText("风险预案：若订阅留存不及预期（Dodow 无订阅的小而美结局），硬件毛利仍可支撑基本盘，同时转向 B 端养老方案。", { x: M, y: 6.35, w: 5.9, h: 0.65, fontFace: F, fontSize: 12.5, color: MUTED2, margin: 0, lineSpacingMultiple: 1.2 });
  // 右：单台收入构成（示意）
  s.addText("单台 24 个月收入构成（示意估算）", { x: 7.1, y: 1.85, w: 5.6, h: 0.35, fontFace: F, fontSize: 14, bold: true, color: TEXT, margin: 0 });
  s.addChart(p.charts.BAR, [{
    name: "收入（元）",
    labels: ["硬件毛利（一次性）", "订阅 12 个月", "订阅 24 个月"],
    values: [379, 180, 360],
  }], {
    x: 7.1, y: 2.3, w: 5.6, h: 3.9, barDir: "col",
    chartColors: [VIOLET_DIM, "B99B8C", TERRA], varyColors: true,
    chartArea: { fill: { color: BG } }, plotArea: { fill: { color: BG } },
    catAxisLabelColor: MUTED, catAxisLabelFontSize: 12, catAxisLabelFontFace: F,
    valAxisLabelColor: MUTED2, valAxisLabelFontSize: 11, valAxisLabelFontFace: F,
    valAxisMaxVal: 450, valGridLine: { color: LINE, size: 0.5 }, catGridLine: { style: "none" },
    showValue: true, dataLabelPosition: "outEnd", dataLabelColor: TEXT, dataLabelFontSize: 13, dataLabelFontFace: F,
    showLegend: false, barGapWidthPct: 60,
  });
  s.addText("示意估算：ASP 599 元 − BOM 220 元 ≈ 379 元；订阅 15 元/月。实际以量产成本与留存为准。", { x: 7.1, y: 6.35, w: 5.6, h: 0.6, fontFace: F, fontSize: 12, color: MUTED2, margin: 0, lineSpacingMultiple: 1.2 });
  s.addNotes("商业模式照抄被验证过的公式 A：硬件 599 到 799，BOM 220，硬件本身毛利为正，是入口不是利润；利润来自每月 15 块的订阅——每周新音景、被动学习内容库、个性化建议。这个订阅不是空想：Oura 的会员一年留存 85%，Hatch 的内容订阅每月三百万美金。右边这张图是示意：一台机器硬件毛利约 380 元，用户如果订阅两年，订阅收入 360 元追平硬件毛利，LTV 翻倍。万一订阅留存不行，还有 B 端养老这条 Sleepace 验证过的路。");
})();

/* ============ 12 竞品 ============ */
(() => {
  const s = p.addSlide();
  pageChrome(s, 12, "11 / 竞争格局");
  title(s, "错位竞争：别人做单品，我们做会陪伴的编排中枢", "对 Hatch：它只管自己这一台，我们指挥全屋；对 Sleepal：它做医疗级单品，我们做普惠中枢。");
  const hd = { fontFace: F, fontSize: 13, bold: true, color: TEXT, fill: { color: CARD2 }, align: "center", valign: "middle" };
  const c1 = { fontFace: F, fontSize: 12.5, bold: true, color: MUTED, valign: "middle", align: "center" };
  const cc = { fontFace: F, fontSize: 12.5, color: TEXT, fill: { color: TERRA_TINT }, valign: "middle", align: "center" };
  const cn = { fontFace: F, fontSize: 12.5, color: MUTED, valign: "middle", align: "center" };
  const tbl = [
    [{ text: "维度", options: hd }, { text: "眠屿 S1", options: { ...hd, color: TERRA } }, { text: "Hatch Restore", options: hd }, { text: "Sleepal AI Lamp", options: hd }, { text: "Dodow", options: hd }],
    [{ text: "价格", options: c1 }, { text: "599–799 元", options: cc }, { text: "约 170 美元", options: cn }, { text: "449 美元", options: cn }, { text: "约 60 美元", options: cn }],
    [{ text: "核心能力", options: c1 }, { text: "Agent 编排全屋 + 本机干预 + 电子宠物陪伴", options: cc }, { text: "灯光 + 音频内容", options: cn }, { text: "毫米波睡眠监测（单机）", options: cn }, { text: "呼吸灯（单功能）", options: cn }],
    [{ text: "感知能力", options: c1 }, { text: "雷达 + 麦克风 + 环境传感", options: cc }, { text: "无", options: cn }, { text: "雷达（医疗级 κ=0.695）", options: cn }, { text: "无", options: cn }],
    [{ text: "生态联动", options: c1 }, { text: "米家全屋编排", options: cc }, { text: "无", options: cn }, { text: "无（隐私优先不联网）", options: cn }, { text: "无", options: cn }],
    [{ text: "个性化", options: c1 }, { text: "记忆层每晚自学习", options: cc }, { text: "内容订阅", options: cn }, { text: "固定算法", options: cn }, { text: "无", options: cn }],
    [{ text: "情感陪伴", options: c1 }, { text: "五只宠物 · 语音对话 · 故事", options: cc }, { text: "无", options: cn }, { text: "无", options: cn }, { text: "无", options: cn }],
  ];
  s.addTable(tbl, { x: M, y: 2.3, w: 12.13, colW: [1.5, 3.1, 2.4, 2.85, 2.28], rowH: [0.5, 0.6, 0.6, 0.6, 0.6, 0.6, 0.6], border: { type: "solid", pt: 0.5, color: LINE }, margin: 0.06 });
  s.addText("来源：各公司官网与公开报道；Sleepal 数据见 36氪专访（睡眠分期 κ=0.695）", { x: M, y: 6.72, w: 12, h: 0.3, fontFace: F, fontSize: 12, color: MUTED2, margin: 0 });
  s.addNotes("竞争格局一句话：没有人同时占住三格——感知、全屋编排、情感陪伴。Hatch 没有感知也不联动；Sleepal 感知做到医疗级但 449 美元、刻意不联网；Dodow 只是一个呼吸灯。最后一行“情感陪伴”全场只有我们有：五只宠物、语音对话、睡前故事。大厂如果要跟进，我们的应对是速度——先发优势在编排协议和用户睡眠记忆数据，做到“被收购也值”的生态位。");
})();

/* ============ 13 路线图与指标 ============ */
(() => {
  const s = p.addSlide();
  pageChrome(s, 13, "12 / 路线图与指标");
  title(s, "软件先验证，硬件再迁移：四步走到商业化");
  s.addShape(p.shapes.LINE, { x: M + 0.3, y: 2.25, w: 11.2, h: 0, line: { color: LINE, width: 1.5 } });
  const ph = [
    ["1", "软件验证 · 现在", "v0.1 → v1.8 共 18 次迭代已完成；真机试用一周，收集留存与弃用点。", "原型已完成"],
    ["2", "硬件迁移", "openvela 开发板：屏幕、按键、麦克风、扬声器均为现成映射。", "下一站"],
    ["3", "感知升级", "接 60GHz 毫米波雷达或米家手环数据，模拟分期单点替换为真数据。", ""],
    ["4", "商业化", "Hatch 路线：硬件走量 + 内容订阅；B 端养老二期。", ""],
  ];
  ph.forEach((t, i) => {
    const x = M + i * 3.0;
    s.addShape(p.shapes.OVAL, { x: x + 0.16, y: 2.1, w: 0.32, h: 0.32, fill: { color: i === 0 ? TERRA : VIOLET } });
    s.addText(t[0], { x: x + 0.16, y: 2.1, w: 0.32, h: 0.32, fontFace: F, fontSize: 13, bold: true, color: i === 0 ? "14100C" : TEXT, align: "center", valign: "middle", margin: 0 });
    s.addText(t[1], { x, y: 2.62, w: 2.75, h: 0.4, fontFace: F, fontSize: 17.5, bold: true, color: TEXT, margin: 0 });
    s.addText(t[2], { x, y: 3.08, w: 2.72, h: 1.35, fontFace: F, fontSize: 13, color: MUTED, margin: 0, lineSpacingMultiple: 1.22 });
    if (t[3]) {
      s.addShape(p.shapes.ROUNDED_RECTANGLE, { x, y: 4.42, w: 1.5, h: 0.42, rectRadius: 0.21, fill: { color: i === 0 ? TERRA : CARD2 } });
      s.addText(t[3], { x, y: 4.42, w: 1.5, h: 0.42, fontFace: F, fontSize: 12.5, bold: true, color: i === 0 ? "14100C" : MUTED, align: "center", valign: "middle", margin: 0 });
    }
  });
  hline(s, M, 5.15, 12.1);
  s.addText("20%", { x: M, y: 5.5, w: 2.6, h: 1.0, fontFace: F, fontSize: 60, bold: true, color: TERRA, margin: 0 });
  s.addText("北极星指标：入睡潜伏期，连续使用 4 周缩短 20%（雷达判定，对照首周基线）", { x: 3.3, y: 5.62, w: 9.3, h: 0.75, fontFace: F, fontSize: 16.5, bold: true, color: TEXT, margin: 0, lineSpacingMultiple: 1.2 });
  s.addText("次指标：7 日留存（公式 A 的命门）· 订阅转化率 · 语音互动率 · 晨间报告打开率｜反向约束：“捂住顶部”否决率 < 2%（Agent 误干预）· 规则误触发率 < 5% · 记忆层 14 晚收敛", { x: 3.3, y: 6.45, w: 9.3, h: 0.65, fontFace: F, fontSize: 12.5, color: MUTED, margin: 0, lineSpacingMultiple: 1.2 });
  s.addNotes("路线四步，第一步已经走完：十八个版本、全功能原型，接下来真机试用一周拿留存数据。第二步硬件迁移用 openvela 开发板，屏幕按键麦克风扬声器都是现成映射。第三步把模拟分期换成毫米波真数据——代码里就是单点替换。第四步商业化。北极星指标只有一个：入睡潜伏期，四周缩短百分之二十。同时我们给自己上了反向约束——捂住顶部否决率必须小于 2%，Agent 不能变成新的打扰源。");
})();

/* ============ 14 团队（占位） ============ */
(() => {
  const s = p.addSlide();
  pageChrome(s, 14, "13 / 团队");
  title(s, "团队与分工");
  const team = [
    ["姓名 A", "产品负责人 · Agent 与软件", "负责产品定义、Agent 规则引擎与 App 原型（m.html v1.8，零依赖单文件）。"],
    ["姓名 B", "硬件与嵌入式", "负责 openvela 移植、60GHz 毫米波雷达选型与音频硬件、BOM 成本控制。"],
    ["姓名 C", "内容与运营", "负责音景配方与被动学习内容库、订阅运营与 B 端渠道拓展。"],
  ];
  team.forEach((t, i) => {
    const x = M + i * 4.18;
    s.addShape(p.shapes.ROUNDED_RECTANGLE, { x, y: 1.95, w: 3.95, h: 3.7, rectRadius: 0.12, fill: { color: CARD }, line: { color: LINE, width: 0.75 } });
    s.addShape(p.shapes.OVAL, { x: x + 1.475, y: 2.3, w: 1.0, h: 1.0, fill: { color: CARD2 }, line: { color: VIOLET_DIM, width: 1 } });
    s.addText(t[0].slice(-1), { x: x + 1.475, y: 2.3, w: 1.0, h: 1.0, fontFace: F, fontSize: 22, bold: true, color: VIOLET, align: "center", valign: "middle", margin: 0 });
    s.addText(t[0], { x, y: 3.45, w: 3.95, h: 0.4, fontFace: F, fontSize: 19, bold: true, color: TEXT, align: "center", margin: 0 });
    s.addText(t[1], { x, y: 3.9, w: 3.95, h: 0.35, fontFace: F, fontSize: 13.5, bold: true, color: TERRA, align: "center", margin: 0 });
    s.addText(t[2], { x: x + 0.35, y: 4.35, w: 3.25, h: 1.1, fontFace: F, fontSize: 13, color: MUTED, align: "center", margin: 0, lineSpacingMultiple: 1.25 });
  });
  s.addText("注：本页姓名为占位，路演前替换为真实信息 · 顾问与联系方式见参赛材料", { x: M, y: 6.15, w: 12.1, h: 0.35, fontFace: F, fontSize: 12.5, color: MUTED2, align: "center", margin: 0 });
  s.addNotes("团队页请替换为真实信息后使用。建议按“产品+Agent / 硬件+嵌入式 / 内容+运营”三块讲，突出三人分别对应本页前三列的分工闭环；如有导师或产业资源（如睡眠医学顾问、米家供应链联系人），在下方一行补充，评委会加分。");
})();

/* ============ 15 结尾 ============ */
(() => {
  const s = p.addSlide();
  s.background = { color: BG };
  s.addImage({ path: PETS + "/warp.png", x: 7.5, y: 0, w: 5.83, h: 7.5, sizing: { type: "cover", w: 5.83, h: 7.5 } });
  s.addText("眠屿 SleepIsle", { x: M, y: 1.7, w: 6.5, h: 0.4, fontFace: F, fontSize: 16, bold: true, color: TERRA, charSpacing: 3, margin: 0 });
  s.addText("让每一个夜晚，\n都有人值守。", { x: M, y: 2.3, w: 6.6, h: 2.0, fontFace: F, fontSize: 46, bold: true, color: TEXT, margin: 0, lineSpacingMultiple: 1.25 });
  s.addText("会陪伴的睡眠编排中枢 · 原型已就绪，硬件蓄势待发", { x: M, y: 4.45, w: 6.4, h: 0.4, fontFace: F, fontSize: 16, color: MUTED, margin: 0 });
  s.addText("谢谢 · 欢迎评委提问与现场体验 Demo", { x: M, y: 5.3, w: 6.4, h: 0.45, fontFace: F, fontSize: 19, bold: true, color: TEXT, margin: 0 });
  s.addText("参赛团队（占位） · 2026 年 11 月", { x: M, y: 6.25, w: 4.5, h: 0.4, fontFace: F, fontSize: 13.5, color: MUTED2, margin: 0 });
  s.addNotes("收尾 20 秒：睡眠经济里，有人卖监测、有人卖床垫、有人卖内容，我们想做那个在夜里真正陪着你、并指挥整个卧室的大脑。让每一个夜晚，都有人值守。谢谢，欢迎提问，也欢迎现场体验我们的 Demo。");
})();

p.writeFile({ fileName: OUT }).then(() => console.log("OK 15 slides -> " + OUT));
