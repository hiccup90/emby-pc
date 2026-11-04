#include "embythumbnailcache.h"

#include "embyclient.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslError>
#include <QUrl>

namespace ui {

EmbyThumbnailCache::EmbyThumbnailCache(QObject *parent)
    : ThumbnailCache(parent)
    , m_network(new QNetworkAccessManager(this))
{
}

void EmbyThumbnailCache::setClient(emby::EmbyClient *client)
{
    m_client = client;
}

void EmbyThumbnailCache::loadAsync(const QString &key, const QSize &size)
{
    if (!m_client || !key.startsWith(QLatin1String("http"))) {
        ThumbnailCache::loadAsync(key, size);
        return;
    }

    QNetworkRequest request{QUrl(key)};
    request.setRawHeader("Authorization", m_client->authorizationHeader().toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply *reply = m_network->get(request);
    if (m_client->profile().ignoreSslErrors) {
        connect(reply, &QNetworkReply::sslErrors, reply, [reply](const QList<QSslError> &) {
            reply->ignoreSslErrors();
        });
    }
    connect(reply, &QNetworkReply::finished, this, [this, reply, key, size]() {
        const QByteArray data = reply->readAll();
        const QNetworkReply::NetworkError error = reply->error();
        reply->deleteLater();

        QPixmap pixmap;
        if (error == QNetworkReply::NoError && pixmap.loadFromData(data)) {
            insert(key, size, pixmap);
            return;
        }

        // 失败也回填占位图，避免每次重绘都重新请求；真实图片恢复可 clear() 后重试。
        const QPixmap fallback = generate(key, size);
        if (!fallback.isNull())
            insert(key, size, fallback);
    });
}

} // namespace ui
