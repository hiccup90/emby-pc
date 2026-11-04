#pragma once

#include "thumbnailcache.h"

class QNetworkAccessManager;

namespace emby {
class EmbyClient;
}

namespace ui {

// 真实 Emby 图片缓存：key 为 EmbyClient::imageUrl() 生成的 URL（不含 token），
// 加载时给 QNetworkRequest 加 Authorization 头；失败时回退到离线占位图。
// mock 模式的 key（poster:... / backdrop:...）仍走父类占位图。
class EmbyThumbnailCache : public ThumbnailCache
{
    Q_OBJECT
public:
    explicit EmbyThumbnailCache(QObject *parent = nullptr);

    void setClient(emby::EmbyClient *client);
    emby::EmbyClient *client() const { return m_client; }

protected:
    void loadAsync(const QString &key, const QSize &size) override;

private:
    emby::EmbyClient *m_client = nullptr;
    QNetworkAccessManager *m_network = nullptr;
};

} // namespace ui
