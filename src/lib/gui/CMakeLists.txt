# SPDX-FileCopyrightText: (C) 2024 - 2026 Deskflow Developers
# SPDX-FileCopyrightText: (C) 2024 Symless Ltd
# SPDX-License-Identifier: MIT

set(target gui)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

if(APPLE)
  set (platform_sources
    OSXHelpers.h
    OSXHelpers.mm
  )
endif()

add_library(${target} STATIC
  ${platform_sources}
  ComputerSetupModel.cpp
  ComputerSetupModel.h
  Diagnostic.cpp
  Diagnostic.h
  FileTail.cpp
  FileTail.h
  Logger.cpp
  Logger.h
  MainWindow.cpp
  MainWindow.h
  MainWindow.ui
  Messages.cpp
  Messages.h
  StyleUtils.h
  TlsUtility.cpp
  TlsUtility.h
  VersionChecker.cpp
  VersionChecker.h
  config/ComputerConfig.h
  config/Computer.cpp
  config/Computer.h
  config/ComputerList.cpp
  config/ComputerList.h
  config/ServerConfig.cpp
  config/ServerConfig.h
  core/CoreProcess.cpp
  core/CoreProcess.h
  core/NetworkMonitor.cpp
  core/NetworkMonitor.h
  dialogs/AboutDialog.cpp
  dialogs/AboutDialog.h
  dialogs/AboutDialog.ui
  dialogs/ActionDialog.cpp
  dialogs/ActionDialog.h
  dialogs/ActionDialog.ui
  dialogs/ClientConfigDialog.h
  dialogs/ClientConfigDialog.cpp
  dialogs/ClientConfigDialog.ui
  dialogs/ComputerSettingsDialog.cpp
  dialogs/ComputerSettingsDialog.h
  dialogs/ComputerSettingsDialog.ui
  dialogs/FingerprintDialog.cpp
  dialogs/FingerprintDialog.h
  dialogs/HelpDialog.cpp
  dialogs/HelpDialog.h
  dialogs/HotkeyDialog.cpp
  dialogs/HotkeyDialog.h
  dialogs/HotkeyDialog.ui
  dialogs/ServerConfigDialog.cpp
  dialogs/ServerConfigDialog.h
  dialogs/ServerConfigDialog.ui
  dialogs/SettingsDialog.cpp
  dialogs/SettingsDialog.h
  dialogs/SettingsDialog.ui
  ipc/IpcClient.cpp
  ipc/IpcClient.h
  ipc/DaemonIpcClient.cpp
  ipc/DaemonIpcClient.h
  ipc/CoreIpcClient.cpp
  ipc/CoreIpcClient.h
  validators/AliasValidator.cpp
  validators/AliasValidator.h
  validators/ComputerDuplicationsValidator.cpp
  validators/ComputerDuplicationsValidator.h
  validators/ComputerLineNameValidator.cpp
  validators/ComputerLineNameValidator.h
  validators/ComputerNameValidator.cpp
  validators/ComputerNameValidator.h
  validators/EmptyStringValidator.cpp
  validators/EmptyStringValidator.h
  validators/IStringValidator.cpp
  validators/IStringValidator.h
  validators/LineEditValidator.cpp
  validators/LineEditValidator.h
  validators/SpacesValidator.cpp
  validators/SpacesValidator.h
  validators/ValidationError.cpp
  validators/ValidationError.h
  validators/IpAddressValidator.cpp
  validators/IpAddressValidator.h
  widgets/ComputerSetupView.cpp
  widgets/ComputerSetupView.h
  widgets/FingerprintPreview.cpp
  widgets/FingerprintPreview.h
  widgets/KeySequenceWidget.cpp
  widgets/KeySequenceWidget.h
  widgets/LogDock.cpp
  widgets/LogDock.h
  widgets/LogWidget.h
  widgets/LogWidget.cpp
  widgets/NewComputerWidget.cpp
  widgets/NewComputerWidget.h
  widgets/RemoveComputerWidget.cpp
  widgets/RemoveComputerWidget.h
  widgets/SearchWidget.h
  widgets/SearchWidget.cpp
  widgets/StatusBar.cpp
  widgets/StatusBar.h
  widgets/SettingsDialogButtonBox.cpp
  widgets/SettingsDialogButtonBox.h
)

if(WIN32 AND ENABLE_AUDIO_SHARING)
  add_executable(deskflow-startup-settings-tests EXCLUDE_FROM_ALL tests/StartupSettingsTests.cpp)
  target_compile_options(deskflow-startup-settings-tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
  target_link_libraries(deskflow-startup-settings-tests PRIVATE gui)
  target_sources(${target} PRIVATE widgets/AudioSharingWidget.cpp widgets/AudioSharingWidget.h audio/PairedAudioControl.cpp audio/PairedAudioControl.h)
  target_sources(${target} PRIVATE audio/PairedAudioGroup.cpp audio/PairedAudioGroup.h)
  add_executable(deskflow-paired-audio-group-tests EXCLUDE_FROM_ALL ../audio/tests/PairedAudioGroupTests.cpp ../audio/tests/GroupSessionFixture.cpp)
  target_compile_definitions(deskflow-paired-audio-group-tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
  target_compile_options(deskflow-paired-audio-group-tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
  target_link_libraries(deskflow-paired-audio-group-tests PRIVATE gui)
  target_compile_definitions(${target} PRIVATE DESKFLOW_AUDIO_SHARING=1)
  target_compile_options(${target} PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
  target_link_libraries(${target} PUBLIC deskflow-audio-session)
  add_executable(deskflow-audio-gui-tests EXCLUDE_FROM_ALL ../audio/tests/GuiIntegrationTests.cpp)
  target_compile_options(deskflow-audio-gui-tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
  target_link_libraries(deskflow-audio-gui-tests PRIVATE gui)
  add_executable(deskflow-paired-audio-control-tests EXCLUDE_FROM_ALL ../audio/tests/PairedAudioControlTests.cpp)
  target_compile_options(deskflow-paired-audio-control-tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
  target_link_libraries(deskflow-paired-audio-control-tests PRIVATE gui)
  add_executable(deskflow-paired-audio-media-tests EXCLUDE_FROM_ALL ../audio/tests/PairedAudioMediaTests.cpp)
  target_compile_options(deskflow-paired-audio-media-tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
  target_link_libraries(deskflow-paired-audio-media-tests PRIVATE gui)
  add_executable(deskflow-paired-audio-widget-tests EXCLUDE_FROM_ALL ../audio/tests/PairedAudioWidgetTests.cpp)
  target_compile_options(deskflow-paired-audio-widget-tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
  target_link_libraries(deskflow-paired-audio-widget-tests PRIVATE gui)
  add_executable(deskflow-paired-audio-sender-widget-tests EXCLUDE_FROM_ALL ../audio/tests/PairedAudioSenderWidgetTests.cpp ../audio/tests/SenderWidgetSessionFixture.cpp)
  target_compile_definitions(deskflow-paired-audio-sender-widget-tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
  target_compile_options(deskflow-paired-audio-sender-widget-tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
  target_link_libraries(deskflow-paired-audio-sender-widget-tests PRIVATE gui)
  add_executable(deskflow-mainwindow-audio-tests EXCLUDE_FROM_ALL ../audio/tests/MainWindowIntegrationTests.cpp ../../apps/res/deskflow.qrc)
  target_compile_options(deskflow-mainwindow-audio-tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
  target_link_libraries(deskflow-mainwindow-audio-tests PRIVATE gui)
  add_executable(deskflow-core-adoption-tests EXCLUDE_FROM_ALL ../audio/tests/CoreAdoptionTests.cpp)
  target_compile_options(deskflow-core-adoption-tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
  target_link_libraries(deskflow-core-adoption-tests PRIVATE gui app)
  add_executable(deskflow-ipc-stream-tests EXCLUDE_FROM_ALL ../audio/tests/IpcStreamTests.cpp)
  target_compile_options(deskflow-ipc-stream-tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
  target_link_libraries(deskflow-ipc-stream-tests PRIVATE gui app)
  add_executable(deskflow-mute-protocol-tests EXCLUDE_FROM_ALL ../audio/tests/MuteWatchdogProtocolTests.cpp)
  target_compile_options(deskflow-mute-protocol-tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
  target_link_libraries(deskflow-mute-protocol-tests PRIVATE ole32 uuid)
endif()

target_link_libraries(${target}
  PUBLIC
    common
    platform
    net
    Qt6::Widgets
)
