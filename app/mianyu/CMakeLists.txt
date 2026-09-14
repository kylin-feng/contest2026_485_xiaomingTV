############################################################################
# apps/examples/mianyu/CMakeLists.txt
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
############################################################################

if(CONFIG_LVX_USE_DEMO_CONTEST2026_485_MIANYU)

  set(APP_NAME mianyu)

  # 注意命名：核心层【API】是 my_ 前缀（my_sched_* / my_noise_* ...），
  # 但【实现文件名】是 mianyu_ 前缀（src/mianyu_schedule.c 等）。
  # 另：my_datetime_* 没有独立实现文件，它就在 src/mianyu_schedule.c 内。
  # 后端：真机链 hal/sf32lb52/（vela，RTC + NuttX），PC 链 hal/sim/。
  set(APP_SRCS
    mianyu_app_main.c
    src/mianyu_schedule.c
    src/mianyu_noise_gen.c
    src/mianyu_sleep_detect.c
    src/mianyu_fade.c
    src/mianyu_sleep_memory.c
    src/mianyu_breathe.c
    hal/sf32lb52/mianyu_hal_vela.c
    ui/breathe_lvgl.c
  )
  set(APP_INCDIRS
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${CMAKE_CURRENT_LIST_DIR}/hal
  )

  # The first source file in APP_SRCS is automatically aliased by NuttX
  # from "main" to "<appname>_main"; see nuttx_add_application.cmake.
  nuttx_add_application(
    NAME
    ${APP_NAME}
    SRCS
    ${APP_SRCS}
    STACKSIZE
    32768
    PRIORITY
    100
    INCLUDE_DIRECTORIES
    ${APP_INCDIRS}
    COMPILE_FLAGS
    -Wno-unused-parameter -DMIANYU_DEMO_BOUNDED=0
  )

endif()