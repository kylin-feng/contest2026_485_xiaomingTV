# 眠语（mianyu）— openvela 应用

> 主动式哄睡智能体 · openvela AI 硬件开发者大赛 2026 · 队伍 485 · 小鸣TV

`app/mianyu/` 是作品在 openvela / NuttX 上的应用入口和平台无关核心层。

## 模块组成

- `mianyu_app_main.c` — 真机主循环（入口 `mianyu_main`）
- `src/` — 平台无关核心层 7 模块：调度 / 噪声 / 入睡判定 / 淡出 / 记忆 / 时间 / 呼吸
- `hal/sim/` — PC 模拟后端（`make app` 跑这层）
- `ui/` — LVGL 呼吸动画渲染（真机 `make menuconfig` 启 LVGL 时启用）

## 构建

启用：
```
make menuconfig
# → Application Configuration → Demos → "Mianyu" Soothing Sleep Agent
```

编译：
```
cd /path/to/openvela
./build.sh vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap/ --cmake
```

## 本地验证（PC）

不在 openvela 工程内也可独立跑核心层（无需 Linux）：
```
make test   # 465 项单元测试
make app    # 整晚闭环（链接 hal/sim）
make demo   # 单晚压缩演示
```