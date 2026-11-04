#pragma once

#include "uitypes.h"

#include <QPersistentModelIndex>
#include <QWidget>

class QComboBox;
class QLabel;
class QListView;
class QPushButton;

namespace ui {

class HeroBanner;
class MediaFilterProxyModel;
class MediaListModel;
class PosterDelegate;
class ThumbnailCache;

// 媒体库海报网格：QListView(IconMode) + PosterDelegate。
// - 200 个 mock item 时依靠固定网格 + uniformItemSizes + 分批布局保证滚动流畅；
// - 支持搜索过滤、排序（最近添加/名称/评分/年份）、空状态；
// - 首页可复用并附带一张 Hero banner（setHeroItem）。
class LibraryView : public QWidget
{
    Q_OBJECT
public:
    explicit LibraryView(ThumbnailCache *cache, QWidget *parent = nullptr);

    void setTitle(const QString &title);
    void setItems(const QList<MediaItem> &items);
    void appendItems(const QList<MediaItem> &items, bool hasMore);
    void clearItems();
    void setFilterText(const QString &text);
    void clearFilter();
    void setHeroItem(const MediaItem &item);
    void setSortMode(int mode);
    int sortMode() const;

    // 真实 Emby 数据：服务器端排序/搜索/分页，本地 proxy 不参与过滤排序。
    void setLocalFilteringEnabled(bool enabled);
    void setLocalSortingEnabled(bool enabled);

    void setTotalCount(int totalCount);
    void setHasMore(bool hasMore);
    void setInitialLoading(bool loading, const QString &message = QString());
    void setLoadingMore(bool loading);
    void setError(const QString &message);

    int itemCount() const;  // 当前过滤后可见数量
    int totalCount() const; // 服务器总数 / 已加载项数
    QStringList visibleItemIds() const;

signals:
    void itemActivated(const QString &itemId);
    void playRequested(const QString &itemId);
    void loadMoreRequested();
    void sortChanged(int mode);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void buildUi();
    void applySort();
    void updateCountLabel();
    void updateGridGeometry();
    void showEmptyState(bool empty);
    void updateHover(const QModelIndex &index);
    void setStateText(const QString &text, const QString &state);
    void maybeRequestMore();

    ThumbnailCache *m_cache = nullptr;
    MediaListModel *m_model = nullptr;
    MediaFilterProxyModel *m_proxy = nullptr;
    PosterDelegate *m_delegate = nullptr;

    QLabel *m_titleLabel = nullptr;
    QLabel *m_countLabel = nullptr;
    QLabel *m_emptyLabel = nullptr;
    QComboBox *m_sortCombo = nullptr;
    QListView *m_grid = nullptr;
    HeroBanner *m_hero = nullptr;

    QList<MediaItem> m_items;
    MediaItem m_heroItem;
    QString m_filterText;
    QPersistentModelIndex m_hovered;
    int m_totalCount = -1;
    bool m_hasMore = false;
    bool m_loadingMore = false;
    bool m_initialLoading = false;
    bool m_localFiltering = true;
    bool m_localSorting = true;
};

} // namespace ui