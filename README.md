# 主动式哄睡智能体 · 眠语（mianyu）

> openvela AI 硬件开发者大赛 2026 · 队伍 485 · 小鸣TV

## 作品定位

`mianyu` 是在 openvela / SF32LB52 上跑的**主动式哄睡智能体**——在儿童入睡后的整晚窗口里，根据睡眠状态（入睡 / 夜醒 / 深睡 / 浅睡 / 晨起）切换噪声、灯光、衰减策略，把睡眠事件实时标注为记忆。

## 仓库结构

| 路径 | 说明 |
|---|---|
| `app/mianyu/` | openvela 应用入口 + 平台无关核心层（7 模块）+ HAL 双后端 + LVGL 呼吸动画 |
| `tests/` | 核心层 465 项单元测试，可在 PC 直接 `make test` 跑 |
| `demo/night_demo.c` | 单晚压缩演示，可 `make demo` 跑 |
| `skills/` | 自建 3 个哄睡 Skill（SKILL.md，可被 opencode 调用） |
| `docs/` | 作品设计与状态机文档 |
| `app/mianyu/ui/breathe_lvgl.c` | LVGL 呼吸动画渲染 |
| `Makefile` | 本地 `make test / app / demo` 入口 |
| `contest2026_485_xiaomingTV.xml` | repo manifest，作品链接到 `packages/demos/contest2026_485_mianyu` |

## 真机编译（openvela 工程内）

```
cd /path/to/openvela
make menuconfig
# Application Configuration → Demos → "Mianyu" Soothing Sleep Agent (contest2026_485)
./build.sh vendor/openvela/boards/sifli/...   # 按官方支持的板级配置
```

manifest 的 `<linkfile>` 会自动把本仓 `app/mianyu/` 软链到 `packages/demos/contest2026_485_mianyu/`，无需手动复制。

## 本地验证（PC 即可跑核心层）

```
mingw32-make test      # 465 项断言全绿
mingw32-make app       # 整晚闭环模拟
mingw32-make demo      # 单晚压缩演示
```

## 提交状态

- 源码：已合入官方仓 `dev-ai-contest-2026` 分支。
- AI Coding 日志：将由 opencode 在云端 openvela 工程内运行时采集，后续补到 `logs/`。
- 真机：SF32LB52 板子领用后烧录验证。

## License

Apache-2.0（详见 LICENSE）。