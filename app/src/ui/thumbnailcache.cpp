#include "thumbnailcache.h"

#include "postergenerator.h"

#include <QTimer>

namespace ui {

ThumbnailCache::ThumbnailCache(QObject *parent)
    : QObject(parent)
{
}

ThumbnailCache::~ThumbnailCache() = default;

QString ThumbnailCache::cacheKey(const QString &key, const QSize &size)
{
    return key + QLatin1Char('|') + QString::number(size.width()) + QLatin1Char('x')
           + QString::number(size.height());
}

QPixmap ThumbnailCache::get(const QString &key, const QSize &size)
{
    const QString id = cacheKey(key, size);
    auto it = m_cache.find(id);
    if (it == m_cache.end())
        return {};
    it->lastUsed = ++m_tick;
    return it->pixmap;
}

void ThumbnailCache::request(const QString &key, const QSize &size)
{
    if (key.isEmpty() || size.isEmpty())
        return;

    const QString id = cacheKey(key, size);
    if (m_cache.contains(id) || m_pending.contains(id))
        return;

    m_pending.insert(id);

    // 用 0ms 单次定时器把生成/加载推迟到事件循环，避免在 paintEvent 里做重活。
    QTimer::singleShot(0, this, [this, key, size, id]() {
        m_pending.remove(id);
        if (m_cache.contains(id))
            return;
        loadAsync(key, size);
    });
}

void ThumbnailCache::loadAsync(const QString &key, const QSize &size)
{
    const QPixmap pixmap = generate(key, size);
    if (!pixmap.isNull())
        insert(key, size, pixmap);
}

void ThumbnailCache::insert(const QString &key, const QSize &size, const QPixmap &pixmap)
{
    if (pixmap.isNull())
        return;
    const QString id = cacheKey(key, size);
    Entry entry;
    entry.pixmap = pixmap;
    entry.lastUsed = ++m_tick;
    m_cache.insert(id, entry);
    evictIfNeeded();
    emit thumbnailReady(key, size);
}

void ThumbnailCache::setTitleHint(const QString &key, const QString &title)
{
    m_titleHints.insert(key, title);
}

void ThumbnailCache::clear()
{
    m_cache.clear();
    m_pending.clear();
    m_titleHints.clear();
}

void ThumbnailCache::setCapacity(int entries)
{
    m_capacity = qMax(16, entries);
    evictIfNeeded();
}

void ThumbnailCache::evictIfNeeded()
{
    while (m_cache.size() > m_capacity) {
        auto oldest = m_cache.begin();
        for (auto it = m_cache.begin(); it != m_cache.end(); ++it) {
            if (it->lastUsed < oldest->lastUsed)
                oldest = it;
        }
        m_cache.erase(oldest);
    }
}

QPixmap ThumbnailCache::generate(const QString &key, const QSize &size)
{
    const QString title = m_titleHints.value(key);
    if (key.startsWith(QLatin1String("backdrop:")))
        return makeBackdropArt(key, size, title);
    return makePosterArt(key, size, title);
}

} // namespace ui