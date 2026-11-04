#include "libraryview.h"

#include "icons.h"
#include "mediamodel.h"
#include "posterdelegate.h"
#include "thumbnailcache.h"

#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QListView>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>

namespace ui {

class HeroBanner : public QWidget
{
public:
    HeroBanner(ThumbnailCache *cache, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_cache(cache)
    {
        setObjectName(QStringLiteral("HeroBanner"));
        setFixedHeight(262);

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(36, 26, 36, 26);
        layout->setSpacing(8);
        layout->addStretch(1);

        auto *badge = new QLabel(QStringLiteral("本周推荐"), this);
        badge->setObjectName(QStringLiteral("HeroBadge"));
        badge->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
        layout->addWidget(badge);

        m_title = new QLabel(this);
        m_title->setObjectName(QStringLiteral("HeroTitle"));
        layout->addWidget(m_title);

        m_meta = new QLabel(this);
        m_meta->setObjectName(QStringLiteral("HeroMeta"));
        layout->addWidget(m_meta);

        m_overview = new QLabel(this);
        m_overview->setObjectName(QStringLiteral("HeroOverview"));
        m_overview->setWordWrap(true);
        m_overview->setMaximumWidth(620);
        m_overview->setMaximumHeight(60);
        layout->addWidget(m_overview);

        auto *buttons = new QHBoxLayout();
        buttons->setSpacing(10);
        buttons->addSpacing(2);
        m_play = new QPushButton(icon(Icon::Play, QColor(0xFF, 0xFF, 0xFF), 16),
                                 QStringLiteral("播放"), this);
        m_play->setObjectName(QStringLiteral("HeroPlayButton"));
        m_play->setProperty("role", QStringLiteral("primary"));
        m_play->setCursor(Qt::PointingHandCursor);
        m_detail = new QPushButton(QStringLiteral("查看详情"), this);
        m_detail->setObjectName(QStringLiteral("HeroDetailButton"));
        m_detail->setProperty("role", QStringLiteral("secondary"));
        m_detail->setCursor(Qt::PointingHandCursor);
        buttons->addWidget(m_play);
        buttons->addWidget(m_detail);
        buttons->addStretch(1);
        layout->addLayout(buttons);
    }

    void setItem(const MediaItem &item)
    {
        m_item = item;
        m_title->setText(item.title);
        m_meta->setText(QStringLiteral("%1 · %2 · %3")
                            .arg(item.year)
                            .arg(item.genreText().isEmpty() ? QStringLiteral("影视")
                                                            : item.genreText())
                            .arg(item.isSeries() ? QStringLiteral("剧集") : QStringLiteral("电影")));
        m_overview->setText(item.overview);
        update();
    }

    QPushButton *playButton() const { return m_play; }
    QPushButton *detailButton() const { return m_detail; }
    const MediaItem &item() const { return m_item; }

    // 小窗口下隐藏简介，避免 banner 压缩后文字与按钮重叠。
    void setCompact(bool compact)
    {
        if (m_overview->isVisible() == compact)
            m_overview->setVisible(!compact);
    }

    void refreshBackdrop() { update(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);

        QPainterPath clip;
        clip.addRoundedRect(QRectF(rect()), 12, 12);
        p.setClipPath(clip);

        const qreal dpr = devicePixelRatioF();
        const QSize pixelSize(qRound(width() * dpr), qRound(height() * dpr));
        QPixmap backdrop;
        if (m_cache)
            backdrop = m_cache->get(m_item.backdropKey, pixelSize);

        if (backdrop.isNull()) {
            QLinearGradient fallback(0, 0, 0, height());
            fallback.setColorAt(0.0, QColor(0x1B, 0x26, 0x3A));
            fallback.setColorAt(1.0, QColor(0x0B, 0x12, 0x20));
            p.fillRect(rect(), fallback);
            if (m_cache)
                m_cache->request(m_item.backdropKey, pixelSize);
        } else {
            p.drawPixmap(rect(), backdrop);
        }

        QLinearGradient shade(0, 0, width(), 0);
        shade.setColorAt(0.0, QColor(0x08, 0x0E, 0x18, 242));
        shade.setColorAt(0.52, QColor(0x08, 0x0E, 0x18, 190));
        shade.setColorAt(1.0, QColor(0x08, 0x0E, 0x18, 30));
        p.fillRect(rect(), shade);

        QLinearGradient bottom(0, height() * 0.55, 0, height());
        bottom.setColorAt(0.0, QColor(0x08, 0x0E, 0x18, 0));
        bottom.setColorAt(1.0, QColor(0x08, 0x0E, 0x18, 170));
        p.fillRect(rect(), bottom);

        p.setPen(QPen(QColor(255, 255, 255, 18), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 12, 12);
    }

private:
    ThumbnailCache *m_cache = nullptr;
    MediaItem m_item;
    QLabel *m_title = nullptr;
    QLabel *m_meta = nullptr;
    QLabel *m_overview = nullptr;
    QPushButton *m_play = nullptr;
    QPushButton *m_detail = nullptr;
};

LibraryView::LibraryView(ThumbnailCache *cache, QWidget *parent)
    : QWidget(parent)
    , m_cache(cache)
{
    buildUi();
    connect(m_cache, &ThumbnailCache::thumbnailReady, this, [this](const QString &key, const QSize &) {
        if (m_hero && key == m_heroItem.backdropKey)
            m_hero->refreshBackdrop();
        // 缩略图是异步生成的：回填后重绘视口，否则滚动前海报会一直是占位色。
        if (m_grid)
            m_grid->viewport()->update();
    });
}

void LibraryView::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *header = new QWidget(this);
    header->setObjectName(QStringLiteral("LibraryHeader"));
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(30, 20, 30, 10);
    headerLayout->setSpacing(12);

    m_titleLabel = new QLabel(QStringLiteral("媒体库"), header);
    m_titleLabel->setObjectName(QStringLiteral("LibraryTitle"));
    m_countLabel = new QLabel(QStringLiteral("共 0 部"), header);
    m_countLabel->setObjectName(QStringLiteral("LibraryCount"));
    headerLayout->addWidget(m_titleLabel);
    headerLayout->addWidget(m_countLabel, 0, Qt::AlignBottom);
    headerLayout->addStretch(1);

    m_sortCombo = new QComboBox(header);
    m_sortCombo->setObjectName(QStringLiteral("SortCombo"));
    m_sortCombo->addItems({QStringLiteral("最近添加"), QStringLiteral("名称"),
                           QStringLiteral("评分"), QStringLiteral("年份")});
    m_sortCombo->setCursor(Qt::PointingHandCursor);
    headerLayout->addWidget(m_sortCombo);

    root->addWidget(header);

    m_hero = new HeroBanner(m_cache, this);
    m_hero->hide();
    auto *heroWrap = new QVBoxLayout();
    heroWrap->setContentsMargins(30, 0, 30, 18);
    heroWrap->addWidget(m_hero);
    root->addLayout(heroWrap);

    auto *gridWrap = new QVBoxLayout();
    gridWrap->setContentsMargins(18, 0, 18, 14);
    gridWrap->setSpacing(0);

    m_model = new MediaListModel(this);
    m_proxy = new MediaFilterProxyModel(this);
    m_proxy->setSourceModel(m_model);
    m_delegate = new PosterDelegate(m_cache, this);

    m_grid = new QListView(this);
    m_grid->setObjectName(QStringLiteral("PosterGrid"));
    m_grid->setViewMode(QListView::IconMode);
    m_grid->setMovement(QListView::Static);
    m_grid->setResizeMode(QListView::Adjust);
    m_grid->setLayoutMode(QListView::Batched);
    m_grid->setBatchSize(48);
    m_grid->setUniformItemSizes(true);
    m_grid->setSpacing(0);
    m_grid->setSelectionMode(QAbstractItemView::SingleSelection);
    m_grid->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_grid->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_grid->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_grid->setFrameShape(QFrame::NoFrame);
    m_grid->setMouseTracking(true);
    m_grid->setModel(m_proxy);
    m_grid->setItemDelegate(m_delegate);
    m_grid->setGridSize(QSize(190, 343));
    m_grid->installEventFilter(this);
    m_grid->viewport()->installEventFilter(this);

    m_emptyLabel = new QLabel(QStringLiteral("没有找到匹配的影片\n试试更换关键词或清空搜索框"), this);
    m_emptyLabel->setObjectName(QStringLiteral("EmptyState"));
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setWordWrap(true);
    m_emptyLabel->hide();

    gridWrap->addWidget(m_grid, 1);
    gridWrap->addWidget(m_emptyLabel, 1);
    root->addLayout(gridWrap, 1);

    connect(m_grid, &QListView::clicked, this, [this](const QModelIndex &index) {
        const QString id = index.data(MediaListModel::ItemIdRole).toString();
        if (!id.isEmpty())
            emit itemActivated(id);
    });
    connect(m_sortCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int mode) {
                applySort();
                emit sortChanged(mode);
            });
    connect(m_grid->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this](int) { maybeRequestMore(); });

    connect(m_proxy, &QAbstractItemModel::rowsInserted, this, [this]() { updateCountLabel(); });
    connect(m_proxy, &QAbstractItemModel::rowsRemoved, this, [this]() { updateCountLabel(); });
    connect(m_proxy, &QAbstractItemModel::modelReset, this, [this]() { updateCountLabel(); });

    connect(m_hero->playButton(), &QPushButton::clicked, this, [this]() {
        if (!m_heroItem.id.isEmpty())
            emit playRequested(m_heroItem.id);
    });
    connect(m_hero->detailButton(), &QPushButton::clicked, this, [this]() {
        if (!m_heroItem.id.isEmpty())
            emit itemActivated(m_heroItem.id);
    });
}

void LibraryView::setTitle(const QString &title)
{
    m_titleLabel->setText(title);
}

void LibraryView::setItems(const QList<MediaItem> &items)
{
    m_items = items;
    m_totalCount = items.size();
    m_hasMore = false;
    m_initialLoading = false;
    m_loadingMore = false;
    for (const MediaItem &item : items)
        m_cache->setTitleHint(item.posterKey, item.title);
    if (m_localSorting) {
        applySort();
    } else {
        m_model->setItems(m_items);
        updateCountLabel();
        updateGridGeometry();
    }
    if (m_items.isEmpty()) {
        showEmptyState(true);
    } else {
        m_emptyLabel->hide();
        m_grid->show();
    }
    QTimer::singleShot(0, this, [this]() { maybeRequestMore(); });
}

void LibraryView::appendItems(const QList<MediaItem> &items, bool hasMore)
{
    m_hasMore = hasMore;
    m_loadingMore = false;
    for (const MediaItem &item : items)
        m_cache->setTitleHint(item.posterKey, item.title);
    m_items.append(items);
    if (m_localSorting) {
        applySort();
    } else {
        m_model->appendItems(items);
        updateGridGeometry();
    }
    if (!m_items.isEmpty()) {
        m_emptyLabel->hide();
        m_grid->show();
    }
    updateCountLabel();
    QTimer::singleShot(0, this, [this]() { maybeRequestMore(); });
}

void LibraryView::clearItems()
{
    m_items.clear();
    m_model->clear();
    m_totalCount = -1;
    m_hasMore = false;
    m_loadingMore = false;
    m_initialLoading = false;
    m_emptyLabel->hide();
    m_grid->show();
    updateCountLabel();
}

void LibraryView::setFilterText(const QString &text)
{
    if (!m_localFiltering) {
        m_filterText.clear();
        m_proxy->setFilterText(QString());
        updateCountLabel();
        return;
    }
    m_filterText = text;
    m_proxy->setFilterText(text);
    if (m_grid)
        m_grid->scrollToTop();
    updateCountLabel();
    if (m_items.isEmpty())
        showEmptyState(true);
    else
        showEmptyState(itemCount() == 0);
}

void LibraryView::clearFilter()
{
    setFilterText(QString());
}

void LibraryView::setSortMode(int mode)
{
    if (!m_sortCombo || m_sortCombo->currentIndex() == mode)
        return;
    QSignalBlocker blocker(m_sortCombo);
    m_sortCombo->setCurrentIndex(mode);
    if (m_localSorting)
        applySort();
}

int LibraryView::sortMode() const
{
    return m_sortCombo ? m_sortCombo->currentIndex() : 0;
}

void LibraryView::setLocalFilteringEnabled(bool enabled)
{
    m_localFiltering = enabled;
    if (!enabled) {
        m_filterText.clear();
        m_proxy->setFilterText(QString());
    }
}

void LibraryView::setLocalSortingEnabled(bool enabled)
{
    m_localSorting = enabled;
}

void LibraryView::setTotalCount(int totalCount)
{
    m_totalCount = totalCount;
    updateCountLabel();
}

void LibraryView::setHasMore(bool hasMore)
{
    m_hasMore = hasMore;
}

void LibraryView::setInitialLoading(bool loading, const QString &message)
{
    m_initialLoading = loading;
    m_loadingMore = false;
    if (loading) {
        setStateText(message.isEmpty() ? QStringLiteral("正在加载媒体库…") : message,
                     QStringLiteral("loading"));
        m_emptyLabel->show();
        m_grid->hide();
    } else if (m_items.isEmpty()) {
        showEmptyState(true);
    } else {
        m_emptyLabel->hide();
        m_grid->show();
    }
    updateCountLabel();
}

void LibraryView::setLoadingMore(bool loading)
{
    m_loadingMore = loading;
    updateCountLabel();
}

void LibraryView::setError(const QString &message)
{
    m_initialLoading = false;
    m_loadingMore = false;
    setStateText(message, QStringLiteral("error"));
    m_emptyLabel->show();
    m_grid->hide();
    updateCountLabel();
}

void LibraryView::setHeroItem(const MediaItem &item)
{
    m_heroItem = item;
    if (item.id.isEmpty()) {
        m_hero->hide();
        return;
    }
    m_hero->setItem(item);
    m_hero->show();
    m_hero->refreshBackdrop();
}

int LibraryView::itemCount() const
{
    return m_proxy ? m_proxy->rowCount() : 0;
}

int LibraryView::totalCount() const
{
    return m_totalCount >= 0 ? m_totalCount : m_items.size();
}

QStringList LibraryView::visibleItemIds() const
{
    QStringList ids;
    if (!m_proxy)
        return ids;
    ids.reserve(m_proxy->rowCount());
    for (int row = 0; row < m_proxy->rowCount(); ++row)
        ids.append(m_proxy->index(row, 0).data(MediaListModel::ItemIdRole).toString());
    return ids;
}

void LibraryView::applySort()
{
    if (!m_localSorting) {
        updateCountLabel();
        return;
    }
    QList<MediaItem> sorted = m_items;
    const int mode = m_sortCombo ? m_sortCombo->currentIndex() : 0;
    switch (mode) {
    case 1:
        std::stable_sort(sorted.begin(), sorted.end(), [](const MediaItem &a, const MediaItem &b) {
            return QString::localeAwareCompare(a.title, b.title) < 0;
        });
        break;
    case 2:
        std::stable_sort(sorted.begin(), sorted.end(), [](const MediaItem &a, const MediaItem &b) {
            if (a.communityRating == b.communityRating)
                return a.year > b.year;
            return a.communityRating > b.communityRating;
        });
        break;
    case 3:
        std::stable_sort(sorted.begin(), sorted.end(), [](const MediaItem &a, const MediaItem &b) {
            return a.year > b.year;
        });
        break;
    default:
        break;
    }
    m_model->setItems(sorted);
    updateCountLabel();
    updateGridGeometry();
}

void LibraryView::updateCountLabel()
{
    if (!m_countLabel)
        return;
    if (m_initialLoading) {
        m_countLabel->setText(QStringLiteral("加载中…"));
        return;
    }
    const int visible = itemCount();
    const int total = totalCount();
    QString text;
    if (!m_filterText.trimmed().isEmpty())
        text = QStringLiteral("找到 %1 部").arg(visible);
    else
        text = QStringLiteral("共 %1 部").arg(qMax(total, 0));
    if (m_loadingMore)
        text += QStringLiteral(" · 正在加载更多…");
    m_countLabel->setText(text);
}

void LibraryView::setStateText(const QString &text, const QString &state)
{
    if (!m_emptyLabel)
        return;
    m_emptyLabel->setText(text);
    m_emptyLabel->setProperty("state", state);
    m_emptyLabel->style()->unpolish(m_emptyLabel);
    m_emptyLabel->style()->polish(m_emptyLabel);
    m_emptyLabel->update();
}

void LibraryView::maybeRequestMore()
{
    if (!m_grid || !m_hasMore || m_loadingMore || m_initialLoading)
        return;
    if (!m_grid->isVisible())
        return;
    QScrollBar *bar = m_grid->verticalScrollBar();
    if (bar->maximum() - bar->value() < 420)
        emit loadMoreRequested();
}

void LibraryView::updateGridGeometry()
{
    if (!m_grid)
        return;
    const int width = m_grid->viewport()->width();
    // 隐藏页面（QStackedWidget 非当前页）的 viewport 可能只有几十像素，
    // 用它算网格会把 item 压扁；真正显示后 showEvent/Resize 会重新计算。
    if (width < 240)
        return;
    // 留 6px 给滚动条/取整误差，否则 1010/202 恰好整除时 QListView 会少算一列
    const int columns = qMax(1, (width - 6) / 190);
    const int cellWidth = (width - 6) / columns;
    const QSize cellSize = PosterDelegate::cellSizeForWidth(cellWidth);
    if (m_grid->gridSize() != cellSize)
        m_grid->setGridSize(cellSize);
}

void LibraryView::showEmptyState(bool empty)
{
    if (!m_emptyLabel || !m_grid || m_initialLoading)
        return;
    if (empty) {
        setStateText(m_filterText.trimmed().isEmpty()
                         ? QStringLiteral("这个媒体库还没有内容")
                         : QStringLiteral("没有找到匹配的影片\n试试更换关键词或清空搜索框"),
                     QStringLiteral("empty"));
        m_emptyLabel->show();
        m_grid->hide();
    } else {
        m_emptyLabel->hide();
        m_grid->show();
    }
}

void LibraryView::updateHover(const QModelIndex &index)
{
    if (m_hovered == index && index.isValid())
        return;
    m_hovered = QPersistentModelIndex(index);
    m_delegate->setHoveredIndex(index);
    if (m_grid)
        m_grid->viewport()->update();
}

bool LibraryView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_grid && event->type() == QEvent::Resize) {
        updateGridGeometry();
        return false;
    }

    if (m_grid && watched == m_grid->viewport()) {
        if (event->type() == QEvent::Resize) {
            updateGridGeometry();
            return false;
        }
        switch (event->type()) {
        case QEvent::MouseMove: {
            auto *mouse = static_cast<QMouseEvent *>(event);
            updateHover(m_grid->indexAt(mouse->position().toPoint()));
            break;
        }
        case QEvent::Leave:
            updateHover(QModelIndex());
            break;
        case QEvent::Wheel: {
            auto *wheel = static_cast<QWheelEvent *>(event);
            QScrollBar *bar = m_grid->verticalScrollBar();
            const int step = qMax(48, fontMetrics().height() * 4);
            const int delta = wheel->angleDelta().y();
            if (delta != 0)
                bar->setValue(bar->value() - (delta > 0 ? step : -step));
            return true;
        }
        default:
            break;
        }
    }

    return QWidget::eventFilter(watched, event);
}

void LibraryView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_hero && !m_heroItem.id.isEmpty()) {
        const int heroHeight = qBound(220, qRound(height() * 0.40), 262);
        m_hero->setFixedHeight(heroHeight);
        m_hero->setCompact(heroHeight < 250);
    }
    updateGridGeometry();
}

void LibraryView::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (m_hero && !m_heroItem.id.isEmpty()) {
        const int heroHeight = qBound(220, qRound(height() * 0.40), 262);
        m_hero->setFixedHeight(heroHeight);
        m_hero->setCompact(heroHeight < 250);
    }
    // QStackedWidget 切页时 child 的 resize 时机可能早于 viewport 完成布局，
    // 这里再推迟到事件循环末尾重算一次，确保切页后的网格列数正确。
    QTimer::singleShot(0, this, [this]() { updateGridGeometry(); });
}

} // namespace ui