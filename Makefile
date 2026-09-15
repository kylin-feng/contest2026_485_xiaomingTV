# 安眠科技 · 核心算法层构建（PC 端单元测试，无需任何硬件/RTOS）
#
# 这一层是【平台无关纯 C】：不含任何 openvela/NuttX/SDK 头文件，
# 只依赖 <math.h>/<string.h>/<stdint.h>。因此可以在任意 PC（Linux/macOS）上
# 完整编译并跑单元测试，也便于评审「clone 专属仓 → make test」一键验证。
#
# 真机移植时，这一层的 .c/.h 原样进 openvela 工程，只替换 hal/ 后端。
#
# 常用目标：
#   make test     编译并运行全部单元测试（评审看这个）
#   make          等价于 make test
#   make demo     编译并运行"整晚闭环演示"（真机主循环的 PC 镜像）
#   make clean    清理产物
#   make report   打印逐套件断言统计（取自各程序的 MY_RESULT 权威行）
#
# 注意（踩过的坑）：
#   1. Makefile 里 shell 的 $(( 算术展开必须写成 $$((，
#      否则 make 会把 $(( ... )) 当成变量引用吃掉，得到空值。
#      曾因此让 lim 变量为空，超时保护形同虚设（if 恒假 → 真死循环时永远转）。
#   2. 断言统计不要 grep 数 [PASS] 行数，要用程序打印的 MY_RESULT 行。
#      两套口径必然对不齐（见 test 目标注释）。

# 编译器选择：Linux/macOS 上 `cc` 一定有；Windows（MinGW-w64）上只有 gcc.exe，
# 没有 cc.exe —— 而 README 写的就是 `mingw32-make test`，写死 CC ?= cc 会以
#   process_begin: CreateProcess(NULL, cc ...) failed
#   make (e=2): 系统找不到指定的文件
# 收场，评审第一步就卡住。所以按平台分流。
#
# 平台怎么判（踩过的坑，别改回去）：
#   * 不能只看 $(OS)：实测本机 shell 里 OS 是【空的】，ifeq($(OS),Windows_NT)
#     恒假，分流形同虚设。
#   * 也不能用 $(shell which cc || which gcc)：$(shell) 是通的，但它返回的是
#     msys 风格全路径（/c/Users/.../gcc），而 make 在 Windows 上执行配方走
#     CreateProcess【不经 shell】，这种路径解析不了。
#   最后用的证据是 C:/Windows/System32/ntoskrnl.exe —— 只可能在 Windows 主机
#   上存在，$(wildcard) 直接可判，不依赖任何外部命令。$(OS) 那条并行保留做兜底。
#
# 为什么是 := 而不是 ?= （最容易白折腾半小时的一条）：
#   CC 是 make 的【内建变量】，出厂就带着默认值 "cc"。`?=` 的语义是"仅在变量
#   未定义时赋值"，而 CC 永远"已定义"，所以 CC ?= gcc 写了等于没写，实测仍然
#   去调 cc 然后报 (e=2)。改用 := 直接赋值。不用担心失去可覆盖性：命令行变量
#   的优先级高于 makefile 里的任何赋值，`mingw32-make CC=clang test` 依然生效。
ifeq ($(OS),Windows_NT)
  MIANYU_ON_WINDOWS := 1
endif
ifneq ($(wildcard C:/Windows/System32/ntoskrnl.exe),)
  MIANYU_ON_WINDOWS := 1
endif
ifdef MIANYU_ON_WINDOWS
  CC := gcc
else
  CC := cc
endif
CSTD    ?= -std=c11
CFLAGS  := -O2 -Wall -Wextra -Wshadow -Iapp/mianyu/include $(CSTD)
LDLIBS  ?= -lm

SRC_DIR  := app/mianyu/src
INC_DIR  := app/mianyu/include
TEST_DIR := tests
BUILD    := build

# 核心算法模块（新增模块时在此登记，会自动被 test 目标拾取）
MODULES  := noise_gen sleep_detect fade schedule sleep_memory breathe

SRCS     := $(addprefix $(SRC_DIR)/mianyu_,$(addsuffix .c,$(MODULES)))
TESTS    := $(addprefix $(BUILD)/test_,$(MODULES))

.PHONY: all test demo app clean report
all: test

# ---- 整晚闭环演示 ----
# 链接全部核心模块，跑一个压缩版"模拟一整晚"：cron 触发睡前任务 →
# 噪声合成 → 入睡判定 → 音量淡出 → 夜醒安抚 → 晨唤 → 一晚记忆落盘 + 简报。
# 这是真机 app 主循环的 PC 镜像（demo/night_demo.c 头注释有逐行说明）。
$(BUILD)/night_demo: demo/night_demo.c $(SRCS) | $(BUILD)
	@$(CC) -Idemo $(CFLAGS) -o $@ demo/night_demo.c $(SRCS) $(LDLIBS)

demo: $(BUILD)/night_demo
	@echo "=================================================="
	@echo " 安眠科技 · 整晚闭环演示"
	@echo "=================================================="
	@$(BUILD)/night_demo

# ---- HAL 接入版「真机主循环」（PC 模拟器） ----
# 与 demo 的区别：demo/night_demo.c 是自包含单文件（信号合成写死在文件里）；
# app/mianyu/mianyu_app_main.c 是【真正的真机主循环】，时间/麦克风/音频/存储全走
# hal/mianyu_hal.h 接口，此处链接 hal/sim 后端在 PC 上跑。上真机时换 hal/sf32lb52。
# 同一套 app 代码，换 backend 即换平台——这是「双 HAL」的落点。
#
# 【别把 ui/*.c 加进来】：ui/breathe_lvgl.c 和 ui/watch_ui.c 都 #include
# <lvgl/lvgl.h>，只在 openvela 工程里编译。PC 侧界面接口由 hal/sim 提供空实现
# （见 mianyu_hal_sim.c 里"手表界面"那段），主循环照样能整晚跑完。
# 曾把 breathe_lvgl.c 写进 APP_SRCS，结果是 PC 上 make app 直接找不到 lvgl.h。
#
# hal/sim/pc_main.c 是 PC 侧进程入口：真机上应用入口叫 mianyu_main()，由
# nuttx_add_application 自动把 "main" 别名过去；PC 没这套框架，得自己提供一个
# main，否则链接报 undefined reference to `WinMain'。详见该文件头注释。
APP_CFLAGS := $(CFLAGS) -Iapp/mianyu/hal -Iapp/mianyu -Iapp/mianyu/ui
APP_SRCS   := $(SRCS) app/mianyu/mianyu_app_main.c app/mianyu/hal/sim/mianyu_hal_sim.c app/mianyu/hal/sim/pc_main.c

$(BUILD)/mianyu_app: $(APP_SRCS) | $(BUILD)
	@$(CC) $(APP_CFLAGS) -o $@ $(APP_SRCS) $(LDLIBS)

app: $(BUILD)/mianyu_app
	@echo "=================================================="
	@echo " 安眠科技 · 真机主循环（HAL 接入版，PC 模拟器）"
	@echo "=================================================="
	@$(BUILD)/mianyu_app

# 每个测试可执行文件 = 对应测试源 + 对应模块源（一对一，互不依赖）
$(BUILD)/test_%: $(TEST_DIR)/test_%.c $(SRC_DIR)/mianyu_%.c | $(BUILD)
	@$(CC) $(CFLAGS) -o $@ $< $(SRC_DIR)/mianyu_$*.c $(LDLIBS)

$(BUILD):
	@mkdir -p $(BUILD)

# 跑全部测试：任一失败则整体非零退出（CI 可直接用）
#
# 每个测试包一层【不依赖 timeout/gtimeout 命令】的超时保护（shell 后台进程 +
# kill 实现，macOS/Linux 通用）。教训：schedule 模块曾因 civil_from_days 的
# 月份笔误陷入死循环，没有超时会让整个 make test 卡死，评审 clone 下来就挂住。
# 超时上限 30s（最慢的往返扫描也 <1s），触发即判 FAIL 并打印"疑似死循环"。
#
# 【判据口径】只认测试程序打印的权威行 `MY_RESULT: pass=N fail=M`，
# 不再 grep 数 [PASS] 行数。数行法必然对不齐：手动打印的明细行不计入
# 程序的 g_pass，而 ADD_OK 之类静默断言计入 g_pass 却不打印。
# 曾因此得到"合计 423，但各套件相加≠程序内部数字"的假统计。
TIMEOUT_S := 30
SHELL     := /bin/sh

test: $(TESTS)
	@echo "=================================================="
	@echo " 安眠科技 · 核心算法层单元测试"
	@echo "=================================================="
	@pass=0; fail=0; lim=$$(( $(TIMEOUT_S) * 10 )); \
	for t in $(MODULES); do \
	  echo ""; echo ">>> $$t"; \
	  $(BUILD)/test_$$t > $(BUILD)/$$t.log 2>&1 & \
	  tpid=$$!; i=0; \
	  while kill -0 $$tpid 2>/dev/null; do \
	    if [ $$i -ge $$lim ]; then kill -9 $$tpid 2>/dev/null; echo "__TIMEOUT__" >> $(BUILD)/$$t.log; break; fi; \
	    sleep 0.1; i=$$((i+1)); \
	  done; \
	  wait $$tpid 2>/dev/null; \
	  if grep -q __TIMEOUT__ $(BUILD)/$$t.log 2>/dev/null; then \
	    echo "    TIMEOUT — 超过 $(TIMEOUT_S)s，疑似死循环；日志：$(BUILD)/$$t.log"; \
	    fail=$$((fail+1)); \
	  elif grep -qE '^MY_RESULT: pass=[0-9]+ fail=0$$' $(BUILD)/$$t.log; then \
	    grep -E '^MY_RESULT: ' $(BUILD)/$$t.log | tail -n 1 | sed 's/^/    /'; \
	    pass=$$((pass+1)); \
	  elif grep -qE '^MY_RESULT: ' $(BUILD)/$$t.log; then \
	    echo "    FAILED — 完整日志：$(BUILD)/$$t.log"; \
	    grep -E '^MY_RESULT: ' $(BUILD)/$$t.log | tail -n 1 | sed 's/^/    /'; \
	    grep -E '\[FAIL\]' $(BUILD)/$$t.log | head -n 20 | sed 's/^/    /'; \
	    fail=$$((fail+1)); \
	  else \
	    echo "    NO_RESULT — 未打印 MY_RESULT 行（程序异常退出？）；日志：$(BUILD)/$$t.log"; \
	    tail -n 10 $(BUILD)/$$t.log | sed 's/^/    /'; \
	    fail=$$((fail+1)); \
	  fi; \
	done; \
	echo ""; echo "=================================================="; \
	echo " 套件： $$pass 通过 / $$fail 失败"; \
	echo "=================================================="; \
	test $$fail -eq 0

# 打印各套件断言统计（从 MY_RESULT 权威行取值，不数 [PASS] 行数）
report: test
	@echo ""
	@echo "断言统计（逐套件，取自各程序的 MY_RESULT 权威行）："
	@total_p=0; total_f=0; \
	for t in $(MODULES); do \
	  line=$$(grep -E '^MY_RESULT: ' $(BUILD)/$$t.log | tail -n 1); \
	  p=$$(echo "$$line" | sed -n 's/^MY_RESULT: pass=\([0-9]*\) fail=.*/\1/p'); \
	  f=$$(echo "$$line" | sed -n 's/^MY_RESULT: pass=[0-9]* fail=\([0-9]*\)/\1/p'); \
	  p=$${p:-0}; f=$${f:-0}; \
	  printf "  %-14s %3d PASS / %3d FAIL\n" $$t $$p $$f; \
	  total_p=$$((total_p+p)); total_f=$$((total_f+f)); \
	done; \
	echo "  ------------------------------"; \
	printf "  %-14s %3d PASS / %3d FAIL\n" "合计" $$total_p $$total_f

clean:
	@rm -rf $(BUILD)
	@echo "已清理 $(BUILD)/"
