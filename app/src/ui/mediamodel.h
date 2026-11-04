#pragma once

#include "uitypes.h"

#include <QAbstractListModel>
#include <QSortFilterProxyModel>

namespace ui {

// 海报网格的数据模型。角色直接对应 delegate 的绘制需求，
// 后续真实 API 返回的 MediaItem 列表可直接 setItems()。
class MediaListModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Roles {
        ItemIdRole = Qt::UserRole + 1,
        TitleRole,
        SubtitleRole,
        PosterKeyRole,
        ProgressRole,
        PlayedRole,
        RatingRole,
        YearRole,
        TypeRole,
    };

    explicit MediaListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;

    void setItems(const QList<MediaItem> &items);
    void appendItems(const QList<MediaItem> &items);
    void clear();
    const QList<MediaItem> &items() const { return m_items; }
    const MediaItem &itemAt(int row) const;
    int indexOfId(const QString &id) const;

private:
    QList<MediaItem> m_items;
};

// 搜索框使用的过滤代理：匹配标题 / 原名 / 类型 / 年份。
class MediaFilterProxyModel : public QSortFilterProxyModel
{
    Q_OBJECT
public:
    explicit MediaFilterProxyModel(QObject *parent = nullptr);

    void setFilterText(const QString &text);
    QString filterText() const { return m_text; }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;

private:
    QString m_text;
};

} // namespace ui