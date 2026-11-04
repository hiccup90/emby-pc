#include "mediamodel.h"

namespace ui {

MediaListModel::MediaListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int MediaListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_items.size();
}

QVariant MediaListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size())
        return {};

    const MediaItem &item = m_items.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case TitleRole:
        return item.title;
    case SubtitleRole: {
        QString sub = QString::number(item.year);
        if (!item.genres.isEmpty())
            sub += QStringLiteral(" · ") + item.genres.first();
        if (item.isSeries())
            sub += QStringLiteral(" · 剧集");
        return sub;
    }
    case PosterKeyRole:
        return item.posterKey;
    case ProgressRole:
        return item.progress;
    case PlayedRole:
        return item.played;
    case RatingRole:
        return item.communityRating;
    case YearRole:
        return item.year;
    case TypeRole:
        return item.type;
    case Qt::ToolTipRole:
        return item.overview;
    default:
        return {};
    }
}

void MediaListModel::setItems(const QList<MediaItem> &items)
{
    beginResetModel();
    m_items = items;
    endResetModel();
}

void MediaListModel::appendItems(const QList<MediaItem> &items)
{
    if (items.isEmpty())
        return;
    beginInsertRows(QModelIndex(), m_items.size(), m_items.size() + items.size() - 1);
    m_items.append(items);
    endInsertRows();
}

void MediaListModel::clear()
{
    beginResetModel();
    m_items.clear();
    endResetModel();
}

const MediaItem &MediaListModel::itemAt(int row) const
{
    static const MediaItem empty;
    if (row < 0 || row >= m_items.size())
        return empty;
    return m_items.at(row);
}

int MediaListModel::indexOfId(const QString &id) const
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).id == id)
            return i;
    }
    return -1;
}

MediaFilterProxyModel::MediaFilterProxyModel(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    setDynamicSortFilter(true);
}

void MediaFilterProxyModel::setFilterText(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (m_text == trimmed)
        return;
    m_text = trimmed;
    invalidateFilter();
}

bool MediaFilterProxyModel::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    if (m_text.isEmpty())
        return true;

    const QModelIndex index = sourceModel()->index(sourceRow, 0, sourceParent);
    if (!index.isValid())
        return false;

    const QString haystack = QStringLiteral("%1 %2 %3 %4")
                                 .arg(index.data(MediaListModel::TitleRole).toString(),
                                      index.data(MediaListModel::TypeRole).toString(),
                                      index.data(MediaListModel::SubtitleRole).toString(),
                                      index.data(MediaListModel::YearRole).toString());
    return haystack.contains(m_text, Qt::CaseInsensitive);
}

} // namespace ui