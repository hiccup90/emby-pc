# T34 M4-2 崩溃转储 + 上次异常退出提示

> 任务：task-34 · 负责人：build-verifier · 日期：2026-11-05
> 范围：app/src/crashhandler.h/.cpp（新）、app/src/main.cpp、app/CMakeLists.txt、app/src/ui/settingspage.cpp、本报告。

## 1. 路径与文件

- 应用数据目录：`QStandardPaths::AppLocalDataLocation`（Windows 实机为 `%LOCALAPPDATA%/emby-pc/emby-pc`）。
- 崩溃目录：`%LOCALAPPDATA%/emby-pc/emby-pc/crashes`。
- 崩溃文件：
  - `crash-<yyyyMMdd-HHmmss>-<pid>.dmp`：`MiniDumpWriteDump` 生成的 minidump；
  - `crash-<yyyyMMdd-HHmmss>-<pid>.txt`：应用版本、Qt 版本、OS、异常码/地址、最后 80 行 app.log、加载模块列表。
- 运行标志：`%LOCALAPPDATA%/emby-pc/emby-pc/running.flag`；启动时写入，正常退出删除，每 5 秒心跳刷新。

## 2. 实现

- `CrashHandler::initialize()`（QApplication 创建、applicationName/organizationName 设置后、MainWindow 创建前）：
  1. 确保 app data 与 `crashes` 目录存在；
  2. 若发现残留 `running.flag`，记录 `previousCrashDetected()=true`，读取上次标志内容；
  3. 写入本次 `running.flag`；
  4. Windows 安装 `SetUnhandledExceptionFilter`；
  5. 启动 5 秒心跳 QTimer。
- Windows 未处理异常过滤器：
  - `MiniDumpWriteDump`（`dbghelp`）写 `.dmp`，含 `MiniDumpWithThreadInfo/DataSegs/UnloadedModules`；
  - 写 `.txt` 崩溃上下文（版本、Qt、OS、异常、最后 app.log、模块列表）；
  - 用 `InterlockedExchange` 防止二次异常递归；
  - 不删除 `running.flag`（保证下次启动提示）。
- 正常退出：`main()` 在 `app.exec()` 返回后调用 `CrashHandler::shutdown()`，删除 `running.flag` 并恢复原异常过滤器；不产生 dump。
- 启动提示：`previousCrashDetected()` 为真时，`window.show()` 后 `QTimer::singleShot(0)` 弹出一次“上次异常退出”对话框；带“打开崩溃目录”按钮，显示上次运行标志内容；不阻塞正常启动。
- 设置页“存储与日志”新增“崩溃目录”按钮（`SettingsOpenCrashDirButton`），点击打开并创建 crashes 目录。
- `EMBY_PC_TEST_CRASH=1`：窗口显示 1.2 秒后调用 `RaiseException(EXCEPTION_ACCESS_VIOLATION)`，用于 Windows 实机验证 dump + 重启提示；正常运行不受影响。
- 非 Windows：`#ifdef Q_OS_WIN` 隔离 dbghelp/异常过滤器；仍实现 running.flag 生命周期、目录打开、启动检测，保证 Linux/offscreen 编译与基础测试。

## 3. CMake

- `APP_SOURCES` 增加 `src/crashhandler.cpp src/crashhandler.h`；
- Windows 链接 `dbghelp`（`user32 dwmapi shell32 dbghelp`）；
- 未引入 QWindowKit，无新增第三方依赖。

## 4. 自测（Linux/offscreen）

`/tmp/t34-test` 直接链接 crashhandler.cpp：

```text
PASS initialize writes running.flag
PASS first run no previous crash
PASS shutdown removes running.flag
PASS normal shutdown no crash files
PASS residual flag detected as previous crash
exit=0
```

- Linux app Debug 构建通过；`-Wall -Wextra` 无新增 warning。
- 设置页按钮存在（T28 selftest 基线 151 项仍通过，见 T31 后构建）。

## 5. Windows 实机验收

1. 正常启动、关闭：`%LOCALAPPDATA%/emby-pc/emby-pc/running.flag` 不存在，`crashes` 目录不新增 dump。
2. 设 `EMBY_PC_TEST_CRASH=1` 启动：约 1.2 秒后进程崩溃，`crashes` 生成 `crash-*.dmp` 和 `crash-*.txt`；`running.flag` 仍存在。
3. 不设环境变量重启：弹出“上次异常退出”对话框；点“打开崩溃目录”能看到上述文件；对话框只弹一次。
4. 设置页“崩溃目录”按钮可打开同一目录。
5. 用 WinDbg/Visual Studio 打开 dmp，应能加载崩溃线程/模块信息（需对应 pdb 可选）。

## 6. 已知限制

- 只有 Windows 生成 minidump；Linux 仅做标志/提示/目录入口。
- 崩溃文件不自动上传；用户通过“打开崩溃目录”或导出日志回传。
- `qFatal`/`std::abort` 等非 SEH 退出不走未处理异常过滤器；`EMBY_PC_TEST_CRASH` 使用 SEH AV 覆盖验收路径。