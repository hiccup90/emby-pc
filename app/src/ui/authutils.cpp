#include "authutils.h"

#include "embyclient.h"

#include <QNetworkReply>
#include <QUrl>

namespace ui {

QString friendlyApiError(const emby::ApiError &error)
{
    if (!error.isError())
        return QString();

    switch (error.networkError) {
    case QNetworkReply::ConnectionRefusedError:
        return QStringLiteral("连接被拒绝：请确认服务器地址和端口正确，且 Emby 服务已启动。");
    case QNetworkReply::HostNotFoundError:
        return QStringLiteral("找不到服务器：请检查主机名或 IP 地址是否正确。");
    case QNetworkReply::TimeoutError:
        return QStringLiteral("连接超时：请检查网络、防火墙或代理设置后重试。");
    case QNetworkReply::OperationCanceledError:
        return QStringLiteral("请求已取消或超时，请稍后重试。");
    case QNetworkReply::SslHandshakeFailedError:
        return QStringLiteral("HTTPS 证书校验失败：如果服务器使用自签证书，请勾选“忽略 HTTPS 证书错误”后重试。");
    case QNetworkReply::RemoteHostClosedError:
        return QStringLiteral("服务器提前关闭了连接，请确认服务是否正常。");
    case QNetworkReply::TemporaryNetworkFailureError:
        return QStringLiteral("网络暂时不可用，请稍后重试。");
    case QNetworkReply::AuthenticationRequiredError:
        return QStringLiteral("用户名或密码错误（HTTP 401）：请检查用户名和密码。");
    default:
        break;
    }

    if (error.httpStatus == 401)
        return QStringLiteral("用户名或密码错误，或登录状态已过期（HTTP 401）。");
    if (error.httpStatus == 403)
        return QStringLiteral("当前账号没有访问权限（HTTP 403）。");
    if (error.httpStatus == 404)
        return QStringLiteral("接口不存在（HTTP 404）：请确认地址是 Emby/Jellyfin 服务器根地址。");
    if (error.httpStatus >= 500)
        return QStringLiteral("服务器内部错误（HTTP %1），请稍后重试。").arg(error.httpStatus);
    if (!error.message.isEmpty())
        return QStringLiteral("请求失败：%1").arg(error.message);
    return QStringLiteral("连接失败，请稍后重试。");
}

QString normalizeServerUrl(const QString &input, QString *errorMessage)
{
    QString text = input.trimmed();
    if (text.isEmpty()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("请输入服务器地址，例如 http://192.168.1.10:8096");
        return {};
    }
    if (!text.contains(QStringLiteral("://")))
        text.prepend(QStringLiteral("http://"));

    const QUrl url(text);
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || url.host().isEmpty()
        || (scheme != QLatin1String("http") && scheme != QLatin1String("https"))) {
        if (errorMessage)
            *errorMessage = QStringLiteral("服务器地址格式不正确，请输入 http:// 或 https:// 开头的地址。");
        return {};
    }

    while (text.endsWith(QLatin1Char('/')))
        text.chop(1);
    if (errorMessage)
        errorMessage->clear();
    return text;
}

} // namespace ui