# Windows 登录启动修复

## 问题与行为

原实现的 `gui/startCoreWithGui` 只控制 GUI 启动后是否启动键鼠 core，Windows 服务选项管理后台服务。原设置窗口和安装脚本没有注册当前用户登录后启动 GUI 的入口，所以不能把这些设置当成桌面应用自启动。

在“设置 → 常规”新增“登录 Windows 时启动 Deskflow”，默认关闭。勾选后保存，将当前 GUI exe 的完整路径写入当前用户 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 下的 `Deskflow` 值。取消勾选并保存，只移除此值。启动命令始终带引号，校验实际文件、绝对路径、exe 后缀和命令长度，并在写入后同步、回读确认。失败时保留设置窗口并说明原因。

复选框从实际启动项读取，不额外保存一个 INI 布尔值。入口已存在但指向其他目录时，提示用户保存以使用当前程序；不在启动时自动覆盖，避免多个安装目录互相争抢。便携版应保持位置固定，移动后在新位置重新保存自启动设置；删除程序前先关闭该选项。

界面明确显示“已注册”，不声称 Windows 一定允许它执行。任务管理器、系统设置或策略仍可禁用启动项；“打开 Windows 启动应用设置”按钮打开系统启动页。实现不读写未公开的 `StartupApproved` 数据，也不会覆盖用户在系统中的禁用决定。

窗口是否显示继续遵守“窗口 → 应用启动时”的选项；键鼠是否恢复继续遵守已有 core 状态。此功能发生在当前用户登录后，登录前的键鼠共享仍属于现有服务模式的范围。

## 键鼠优先

单独修改登录启动项时，设置窗口不保存共享配置，主窗口不调用 `applyConfig()` 或重启 core。测试同时验证没有 `settingsChanged` 信号，且 `AutoStartCore` 保持不变。端口、日志级别等原配置确实变化时，保留原有应用流程。

应用配置文件和当前用户启动项分别判断权限。配置只读时，启动项可写仍允许启用、关闭、修复旧路径；保存按钮为此保持可用，其他配置控件仍只读，也不会尝试写入 INI。反过来，启动项只读时，仍允许保存可写的应用配置，不会因为旧启动路径需要修复而阻断保存。共享按钮组件只有设置窗口显式开启独立保存入口，其他对话框维持原行为。

TLS 密钥长度只在保存时写入，证书重新生成也改为待保存请求，按钮显示“保存时重新生成”。取消和重置均不会生成证书。保存时先使用选定路径和长度完成显式生成，再将 TLS 相关字段一起提交、统一发出变更通知；主窗口读取的是完整最终配置。同路径、同长度重新生成仍发送证书通知，以刷新指纹并应用 core。自启动与 TLS 一起修改不会误走“只改自启动”的分支。

回归还发现默认日志级别 `Info` 与选项值 `INFO` 会被误判为修改。现在按不区分大小写的语义比较，避免未修改日志级别时也进入 core 重启路径。

## 安装版卸载

`tools/audio/build_integrated_installer.py` 构建并嵌入独立 x64 原生 MSI 操作，源码在 `tools/audio/windows-autostart-cleanup/`。DLL 使用静态 CRT，不依赖已被卸载的 GUI、Qt 或动态 VC 运行库；可用 `--cmake` 指定构建工具。`--emit-only` 仍会编译该 DLL，但不调用 WiX 或安装程序。

最终卸载按系统 ProfileList 中的用户 SID 检查启动项。已登录用户直接使用加载的 hive；未登录用户在系统身份下临时启用备份/还原权限、将 NTUSER.DAT 加载到唯一临时 HKU 名称，处理后关闭句柄、卸载并恢复权限。不能用要求 hive 内部 ACL 一致的 RegLoadAppKey 代替普通用户注册表加载。损坏、权限不足或无法打开的用户 hive 逐项记入 MSI 日志，继续处理其他用户，不阻塞卸载。

每个用户的两个注册视图只检查 `Deskflow` 值。仅删除命令精确指向当前卸载目录 exe 的入口，路径按绝对路径、大小写及分隔符规范化核对；不同安装目录、便携版、带额外参数、非字符串或畸形命令均保留，其他程序注册值不变。不会修改 StartupApproved、用户配置或信任记录。

升级卸载通过 `UPGRADINGPRODUCTCODE` 排除，保留已有启动项。正常最终卸载在成功提交时清理，失败回滚时保留启动项；明确禁用回滚时使用 RemoveFiles 之后、InstallFinalize 之前的延迟操作。两个分支互斥。

## 验证与边界

测试代码位于 `src/unittests/gui/WindowsAutoStartTests.cpp`，接入原 GUI 单测工程。测试 exe 使用独立输出目录，以免并行测试共享便携配置。每个用例使用随机注册表测试子键、临时 exe 文件及独立配置；不修改真实 Run、服务、信任记录，也不启动 core 或声卡。

覆盖：启用与关闭、重复保存、保留其他注册值、中文及空格路径、旧路径修复、无效文件及长路径拒绝、取消不落地、恢复默认/重置等待保存、只改自启动不应用 core 配置、真实配置修改仍应用，以及失败时保留窗口和配置。另覆盖真实只读配置下启用/关闭/旧路径修复、隔离注册表键拒绝写入而应用配置仍可保存、自启动与 TLS 组合修改、TLS 取消/重置/改回原值、TLS 统一通知、保存后生成并验证真实证书。切换语言事件验证提示和未保存选择保留，系统设置按钮由 URL handler 截获。

前期验证环境使用 `build/gui-check-src` 隔离工程，编译真实注册管理、设置窗口、TLS 控件提交和 Qt 证书校验，并编译真实 MainWindow 的音频集成分支。该环境缺少完整 OpenSSL 开发依赖，底层生成链接符号由仅测试适配器调用真实 OpenSSL CLI 生成临时证书；这不等同于生产原生 OpenSSL 生成函数的完整构建验收。已有音频控制回归继续运行真实 TLS/媒体测试。

前期自动回归结果：

| 范围 | 结果 | 日志 |
| --- | --- | --- |
| GUI/TLS/后台清理/自启动 Release | 3/3 套件通过 | `build/gui-check/autostart-review-gui-release.log` |
| 自启动窗口 Release / Debug | 各 26 项通过、0 失败、0 跳过，含 QtTest 初始化/清理 | `build/gui-check/autostart-review-release-results.txt`、`autostart-review-debug-results.txt` |
| 音频引擎/协议/会话/恢复 Debug | 17/17 通过、无跳过 | `build/audio/autostart-review-debug.log` |
| 音频引擎/协议/会话/恢复 Release | 16 项通过，实时堆检查按设计仅在 Debug 执行 | `build/audio/autostart-review-release.log` |
| 卸载清理 Release / Debug | 各 5 个隔离场景通过 | `build/autostart-cleanup/Testing/Temporary/LastTest.log` |
| Python 延迟分析与安装清理调度 | 25/25 通过 | `build/audio/autostart-review-python.log` |
| 清理操作最小 MSI | WiX 3.14.1 编译、链接、ICE 校验通过；只读核对实际表及嵌入 DLL | `build/autostart-cleanup/package-smoke/verified-tables.json` |

真实 MainWindow 音频集成分支在 Release、Debug 均编译通过。卸载测试使用随机 HKCU 子键模拟多用户和未登录用户 hive，覆盖路径归属、异常类型、幂等、多用户失败隔离及登录竞态；不读取或挂载现场用户 hive。原生清理 DLL 两配置均以 `/W4 /WX` 编译。

最小 MSI 只验证清理操作的真实生成结果，不包含完整应用，也未运行 MSI 操作。已核对动作类型、系统身份、提交/延迟分支条件、CustomActionData 设置顺序、RemoveFiles/InstallFinalize 之间的位置和嵌入 DLL 字节。完整包的只读验证工具 `tools/audio/verify_integrated_msi.py` 也增加相同调度契约检查。DLL 导入仅有系统的 MSI、ADVAPI32、KERNEL32。

新增/修改 C++ 的 clang-format 检查、UI/中文翻译 XML 解析、Python 编译及 `git diff --check` 均通过。前期完整工程配置首先因缺少 Qt LinguistTools 未能完成，证据见 `build/full-regression-configure.log`；当时未执行全部通用单元测试，不能把这部分结果写成“完整工程回归全部通过”。后续完整依赖验证见下文。

### 2026-11-14 完整依赖补充验证

使用 `build/commit-review-20261114/source` 中的当前工作区源码快照、完整 Qt 6.8.3 和 OpenSSL 3.0.16 开发依赖重新配置并编译整个 Release 工程，包括原生证书生成实现，不使用前期的 CLI 链接适配器。补齐自启动测试的 CMake 链接签名，以及布局校验测试的 GUI 头文件搜索目录后，完整构建通过。

| 范围 | 结果 | 日志（相对 `build/commit-review-20261114/`） |
| --- | --- | --- |
| 完整配置及 Release 构建 | 通过 | `configure.log`、`build-release.log` |
| 通用单元测试 | 30/30 套件通过，含真实自启动设置窗口及布局校验 | `generic-release.log` |
| 音频引擎、协议、会话、恢复 Release | 16 项通过；实时堆检查仅在 Debug 执行，Release 跳过 | `audio-release.log` |
| 音频 GUI 集成 | 5/5 套件通过，使用隔离配置与合成媒体 | `deskflow-*-tests.log` |
| 原生卸载清理 Release / Debug | 两种配置各 5 个隔离场景通过 | `cleanup-test-Release.log`、`cleanup-test-Debug.log` |
| Python 延迟分析及安装清理调度 | 25/25 通过 | `python-tests.log` |
| 1.27.6 实际 MSI 只读核验 | 清理调度、桌面快捷方式、全部程序/依赖/翻译字节和干净 PATH 启动通过 | `msi-verification.log` |

本次没有运行会清空系统剪贴板的 `MSWindowsClipboardTests`，也没有运行实际播放或改变设备静音状态的 GUI 测试。源码清单与哈希记录在 `source-manifest.json`，各步骤退出状态记录在 `results.json`；上述自动检查不替代下列真实环境验收。

真实 Windows 注销/登录启动、系统禁用后的人工启用、完整安装包升级/卸载/回滚、系统身份下离线 hive 清理和双机鼠标/键盘/拖动验收尚未执行。源码修改不会自动更新已安装程序，本轮不修改现场启动项，也不注销或重启电脑。
