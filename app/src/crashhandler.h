#pragma once

#include <QString>

class QWidget;

// M4-2 / T34：崩溃转储与上次异常退出检测。
//
// 目录约定（Windows）：
//   %LOCALAPPDATA%/emby-pc/emby-pc/crashes/crash-<time>-<pid>.dmp/.txt
//   %LOCALAPPDATA%/emby-pc/emby-pc/running.flag
// Linux/其他平台只实现 running.flag 生命周期与目录打开，不产生 dump，保证编译。
namespace CrashHandler {

// QApplication 创建、applicationName/organizationName 设置后调用。
// 检测残留 running.flag -> previousCrashDetected()，随后写本次标志并安装
// Windows 未处理异常过滤器/心跳。
void initialize();

// 正常退出调用：停止心跳并删除 running.flag。崩溃路径不会调用。
void shutdown();

bool previousCrashDetected();

// 显示一次“上次异常退出”对话框（GUI 启动后调用）；带“打开崩溃目录”按钮。
void showPreviousCrashDialog(QWidget *parent = nullptr);

// 打开并确保 crashes 目录存在；失败时可选提示。
void openCrashDirectory(QWidget *parent = nullptr);

QString crashDirectory();
QString flagFilePath();

// EMBY_PC_TEST_CRASH=1 时由 main 的定时器调用；Windows 上抛 AV 交给过滤器。
void triggerTestCrash();

} // namespace CrashHandler
