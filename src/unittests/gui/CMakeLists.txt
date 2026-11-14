# SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
# SPDX-License-Identifier: MIT

add_subdirectory(config)
add_subdirectory(core)

if(WIN32)
  # 使用随机注册表测试子键和独立配置，覆盖真实设置窗口，不改机器上的登录启动项。
  create_test(
    NAME WindowsAutoStartTests
    DEPENDS gui
    SOURCE WindowsAutoStartTests.cpp
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
  )
  target_compile_options(WindowsAutoStartTests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
  # create_test 使用不带可见性关键字的链接签名；追加系统库须保持相同形式，避免完整工程配置失败。
  target_link_libraries(WindowsAutoStartTests advapi32)
  # 便携配置以 exe 所在目录定位；独立输出目录防止并行测试意外共享该配置。
  set_target_properties(WindowsAutoStartTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/autostart")
  set_tests_properties(WindowsAutoStartTests PROPERTIES TIMEOUT 45 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

create_test(
  NAME LoggerTests
  DEPENDS gui
  SOURCE LoggerTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
)
