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
  set(APP_SRCS
    mianyu_app_main.c
    src/my_datetime.c
    src/my_sched.c
    src/my_noise.c
    src/my_sleep.c
    src/my_fade.c
    src/my_memory.c
    src/my_breath.c
    hal/sim/mianyu_hal_sim.c
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
    -Wno-unused-parameter
  )

endif()