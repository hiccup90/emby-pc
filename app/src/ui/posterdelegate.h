#pragma once

#include <QPersistentModelIndex>
#include <QStyledItemDelegate>

namespace ui {

class ThumbnailCache;

// 媒体库海报 delegate：圆角海报 + 标题/年份 + 评分角标 + 观看进度条。
// 只负责单个单元格的绘制；滚动性能由 QListView 的 icon 模式 + 固定网格保证。
class PosterDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit PosterDelegate(ThumbnailCache *cache, QObject *parent = nullptr);

    void setHoveredIndex(const QModelIndex &index);

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;

    // 给定单元格宽度时，单元格应有的高度（宽高由海报 2:3 + 两行文字决定）。
    static QSize cellSizeForWidth(int cellWidth);

private:
    ThumbnailCache *m_cache = nullptr;
    QPersistentModelIndex m_hovered;
};

} // namespace ui
