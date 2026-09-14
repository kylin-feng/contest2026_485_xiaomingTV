# 云机构建集成存档（2026-09-14）

编译主机（临时算力卡）关停前抓取的**构建集成侧实际生效文件**，用于事后核对与复现。

| 文件 | 来源路径 | 说明 |
|---|---|---|
| `sifli_ap.c` | `<board>/src/sifli_ap.c` | 板级 bringup：`board_late_initialize()` 里调 `bsp_audio_selftest_start()` + `bsp_voice_link_start()` |
| `CMakeLists.txt` | `<board>/src/CMakeLists.txt` | SRCS 里含 `bsp_audio_test.c` `bsp_voice_link.c` |
| `defconfig` | `<board>/configs/nsh/defconfig` | `CONFIG_INIT_ENTRYPOINT=mianyu_main` |
| `config.h` | `cmake_out/<board>/include/nuttx/config.h` | 编译器**实际读取**的配置（`CONFIG_LVX_USE_DEMO_CONTEST2026_485_MIANYU=1` 等） |
| `bsp_voice_link.c` / `bsp_audio_test.c` | 同 `../` | 与 `board/` 下一致，此处留一份生效版本快照 |

> 这些改动全部可由 `board/patch_board_build.py` **幂等重建**，本目录只是快照存档。
> `System.map` 与 `nuttx`（ELF）未入库（体积），固件 `nuttx.bin` 即
> 交付用 `nuttx_audio12.bin`（1,639,192 B）。
>
> 关键配置核对：
> - `CONFIG_INIT_ENTRYPOINT=mianyu_main` —— app 的 `mianyu_main` 是唯一 init 入口
> - `CONFIG_LVX_USE_DEMO_CONTEST2026_485_MIANYU=1` —— app 已编入构建
