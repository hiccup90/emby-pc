#pragma once

#include <QString>

namespace emby {
struct ApiError;
}

namespace ui {

// 把 EmbyClient 的错误统一翻译成中文提示（登录框、启动恢复、设置页共用）。
// 注意：只使用 error.operation / httpStatus / networkError / message，
// 不读取 payload，也绝不打印 token。
QString friendlyApiError(const emby::ApiError &error);

// 规范化用户输入的服务器地址：补 http://、去尾部斜杠、检查 scheme/host。
// 成功返回规范化 URL；失败返回空字符串并写入 errorMessage。
QString normalizeServerUrl(const QString &input, QString *errorMessage = nullptr);

} // namespace ui
