#include "posterdelegate.h"

#include "icons.h"
#include "mediamodel.h"
#include "thumbnailcache.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>

namespace ui {
namespace {
constexpr int kPosterRadius = 9;
constexpr int kTextHeight = 58;
} // namespace

PosterDelegate::PosterDelegate(ThumbnailCache *cache, QObject *parent)
    : QStyledItemDelegate(parent)
    , m_cache(cache)
{
}

void PosterDelegate::setHoveredIndex(const QModelIndex &index)
{
    m_hovered = QPersistentModelIndex(index);
}

QSize PosterDelegate::cellSizeForWidth(int cellWidth)
{
    const int cardWidth = qBound(120, cellWidth - 24, 200);
    return QSize(cellWidth, qRound(cardWidth * 1.5) + kTextHeight);
}

QSize PosterDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const
{
    return cellSizeForWidth(option.rect.width() > 0 ? option.rect.width() : 190);
}

void PosterDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                           const QModelIndex &index) const
{
    painter->save();
    painter->setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing
                            | QPainter::SmoothPixmapTransform);

    const QRect cell = option.rect;
    const int cardWidth = qMin(qBound(120, cell.width() - 24, 200), qMax(80, cell.width() - 12));
    const int posterHeight = qRound(cardWidth * 1.5);
    const QRect poster(cell.x() + (cell.width() - cardWidth) / 2, cell.y() + 8,
                       cardWidth, posterHeight);

    const QString title = index.data(MediaListModel::TitleRole).toString();
    const QString subtitle = index.data(MediaListModel::SubtitleRole).toString();
    const QString posterKey = index.data(MediaListModel::PosterKeyRole).toString();
    const double progress = index.data(MediaListModel::ProgressRole).toDouble();
    const double rating = index.data(MediaListModel::RatingRole).toDouble();

    const bool hovered = m_hovered.isValid() && m_hovered == index;
    const bool selected = option.state & QStyle::State_Selected;

    const qreal dpr = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
    const QSize pixelSize(qRound(poster.width() * dpr), qRound(poster.height() * dpr));

    QPixmap pixmap;
    if (m_cache)
        pixmap = m_cache->get(posterKey, pixelSize);

    QPainterPath clip;
    clip.addRoundedRect(poster, kPosterRadius, kPosterRadius);

    painter->save();
    painter->setClipPath(clip);
    if (pixmap.isNull()) {
        painter->fillRect(poster, QColor(0x16, 0x20, 0x2F));
        if (m_cache)
            m_cache->request(posterKey, pixelSize);
    } else {
        painter->drawPixmap(poster, pixmap);
    }

    // 进度条：只画在图片底部，未观看不画
    if (progress > 0.005) {
        const int barHeight = 4;
        const QRectF track(poster.left(), poster.bottom() - barHeight, poster.width(), barHeight);
        painter->fillRect(track, QColor(0, 0, 0, 110));
        painter->fillRect(QRectF(track.left(), track.top(), track.width() * qBound(0.0, progress, 1.0),
                                 barHeight),
                          QColor(0x3B, 0x82, 0xF6));
    }
    painter->restore();

    // 海报描边/悬浮态
    QPen borderPen;
    if (selected)
        borderPen = QPen(QColor(0x60, 0xA5, 0xFA), 2.0);
    else if (hovered)
        borderPen = QPen(QColor(0x3B, 0x82, 0xF6), 1.6);
    else
        borderPen = QPen(QColor(255, 255, 255, 18), 1.0);
    painter->setPen(borderPen);
    painter->setBrush(Qt::NoBrush);
    painter->drawRoundedRect(QRectF(poster).adjusted(0.5, 0.5, -0.5, -0.5), kPosterRadius,
                             kPosterRadius);

    // 评分角标
    if (rating > 0.0) {
        const QRectF badge(poster.right() - 46, poster.top() + 8, 38, 20);
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0, 0, 0, 150));
        painter->drawRoundedRect(badge, 10, 10);

        QFont badgeFont = option.font;
        badgeFont.setPixelSize(11);
        badgeFont.setWeight(QFont::DemiBold);
        painter->setFont(badgeFont);
        painter->setPen(QColor(0xF8, 0xFA, 0xFC));
        painter->drawText(badge, Qt::AlignCenter,
                          QStringLiteral("★ %1").arg(rating, 0, 'f', 1));
    }

    // 悬浮/选中时显示播放按钮
    if (hovered || selected) {
        const QPointF center(poster.center().x(), poster.center().y());
        painter->setPen(QPen(QColor(255, 255, 255, 60), 1));
        painter->setBrush(QColor(15, 23, 42, 200));
        painter->drawEllipse(center, 24, 24);
        painter->drawPixmap(QPointF(center.x() - 9, center.y() - 11),
                            iconPixmap(Icon::Play, QColor(0xF8, 0xFA, 0xFC), 22));
    }

    // 标题
    QFont titleFont = option.font;
    titleFont.setPixelSize(14);
    titleFont.setWeight(QFont::DemiBold);
    painter->setFont(titleFont);
    painter->setPen(QColor(0xE2, 0xE8, 0xF0));
    const QFontMetrics titleMetrics(titleFont);
    const QRect titleRect(poster.left(), poster.bottom() + 9, cardWidth, 20);
    painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                      titleMetrics.elidedText(title, Qt::ElideRight, cardWidth));

    // 副标题
    QFont subFont = option.font;
    subFont.setPixelSize(11);
    painter->setFont(subFont);
    painter->setPen(QColor(0x94, 0xA3, 0xB8));
    const QFontMetrics subMetrics(subFont);
    const QRect subRect(poster.left(), poster.bottom() + 29, cardWidth, 18);
    painter->drawText(subRect, Qt::AlignLeft | Qt::AlignVCenter,
                      subMetrics.elidedText(subtitle, Qt::ElideRight, cardWidth));

    painter->restore();
}

} // namespace ui
