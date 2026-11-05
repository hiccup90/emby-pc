#include "homeview.h"

#include "icons.h"
#include "libraryview.h"
#include "thumbnailcache.h"

#include <QDebug>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QProgressBar>
#include <QElapsedTimer>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <functional>

namespace ui {
namespace {

class ResumeCard : public QWidget
{
public:
    ResumeCard(const MediaItem &item, ThumbnailCache *cache, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_cache(cache)
        , m_item(item)
    {
        setObjectName(QStringLiteral("ResumeCard"));
        setAttribute(Qt::WA_StyledBackground, true);
        setCursor(Qt::PointingHandCursor);
        setFixedWidth(236);

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setSpacing(5);

        m_thumbnail = new QLabel(this);
        m_thumbnail->setObjectName(QStringLiteral("ResumeThumbnail"));
        m_thumbnail->setFixedSize(220, 124);
        m_thumbnail->setScaledContents(true);
        m_thumbnail->setAlignment(Qt::AlignCenter);
        layout->addWidget(m_thumbnail);

        m_title = new QLabel(this);
        m_title->setObjectName(QStringLiteral("ResumeCardTitle"));
        m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        layout->addWidget(m_title);

        m_meta = new QLabel(this);
        m_meta->setObjectName(QStringLiteral("ResumeCardMeta"));
        m_meta->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        layout->addWidget(m_meta);

        m_progress = new QProgressBar(this);
        m_progress->setObjectName(QStringLiteral("ResumeProgress"));
        m_progress->setRange(0, 100);
        m_progress->setValue(qRound(qBound(0.0, m_item.progress, 1.0) * 100.0));
        m_progress->setTextVisible(false);
        m_progress->setFixedHeight(3);
        layout->addWidget(m_progress);

        const QString subtitle = m_item.isSeries() ? QStringLiteral("剧集 · 已看 %1%")
                                                         .arg(qRound(m_item.progress * 100))
                                                   : QStringLiteral("电影 · 已看 %1%")
                                                         .arg(qRound(m_item.progress * 100));
        m_meta->setText(subtitle);
        m_meta->setToolTip(m_item.overview);
        QFont titleFont = m_title->font();
        titleFont.setWeight(QFont::DemiBold);
        m_title->setFont(titleFont);
        m_title->setText(m_item.title);

        refreshThumbnail();
    }

    void refreshThumbnail()
    {
        if (!m_cache)
            return;
        const QString key = !m_item.backdropKey.isEmpty() ? m_item.backdropKey : m_item.posterKey;
        if (key.isEmpty())
            return;
        const QSize pixelSize = m_thumbnail->size();
        QPixmap pixmap = m_cache->get(key, pixelSize);
        if (pixmap.isNull()) {
            m_cache->request(key, pixelSize);
            return;
        }
        m_thumbnail->setPixmap(pixmap);
    }

    const MediaItem &item() const { return m_item; }
    std::function<void()> onClick;

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && onClick)
            onClick();
        QWidget::mousePressEvent(event);
    }

private:
    ThumbnailCache *m_cache = nullptr;
    MediaItem m_item;
    QLabel *m_thumbnail = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_meta = nullptr;
    QProgressBar *m_progress = nullptr;
};

// T38：通用首页 section 卡片；继续观看/最近添加/各库最新共用。
class HomeSectionCard : public QWidget
{
public:
    HomeSectionCard(const MediaItem &item, ThumbnailCache *cache, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_cache(cache)
        , m_item(item)
    {
        setObjectName(QStringLiteral("HomeSectionCard"));
        setAttribute(Qt::WA_StyledBackground, true);
        setCursor(Qt::PointingHandCursor);
        setFixedWidth(236);

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setSpacing(5);

        m_thumbnail = new QLabel(this);
        m_thumbnail->setObjectName(QStringLiteral("HomeSectionThumbnail"));
        m_thumbnail->setFixedSize(220, 124);
        m_thumbnail->setScaledContents(true);
        m_thumbnail->setAlignment(Qt::AlignCenter);
        layout->addWidget(m_thumbnail);

        m_title = new QLabel(this);
        m_title->setObjectName(QStringLiteral("HomeSectionCardTitle"));
        m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        m_title->setText(item.title);
        layout->addWidget(m_title);

        m_meta = new QLabel(this);
        m_meta->setObjectName(QStringLiteral("HomeSectionCardMeta"));
        m_meta->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        if (item.progress > 0.0) {
            m_meta->setText((item.isSeries() ? QStringLiteral("剧集 · 已看 %1%")
                                                 : QStringLiteral("电影 · 已看 %1%"))
                                .arg(qRound(qBound(0.0, item.progress, 1.0) * 100.0)));
        } else {
            m_meta->setText(item.isSeries() ? QStringLiteral("剧集") : QStringLiteral("电影"));
        }
        layout->addWidget(m_meta);

        if (item.progress > 0.0) {
            auto *progress = new QProgressBar(this);
            progress->setObjectName(QStringLiteral("HomeSectionProgress"));
            progress->setRange(0, 100);
            progress->setValue(qRound(qBound(0.0, item.progress, 1.0) * 100.0));
            progress->setTextVisible(false);
            progress->setFixedHeight(3);
            layout->addWidget(progress);
        } else {
            layout->addSpacing(3);
        }

        refreshThumbnail();
    }

    void refreshThumbnail()
    {
        if (!m_cache)
            return;
        const QString key = !m_item.backdropKey.isEmpty() ? m_item.backdropKey : m_item.posterKey;
        if (key.isEmpty())
            return;
        const QSize pixelSize = m_thumbnail->size();
        const QPixmap pixmap = m_cache->get(key, pixelSize);
        if (pixmap.isNull()) {
            m_cache->request(key, pixelSize);
            return;
        }
        m_thumbnail->setPixmap(pixmap);
    }

    const MediaItem &item() const { return m_item; }
    std::function<void()> onClick;

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && onClick)
            onClick();
        QWidget::mousePressEvent(event);
    }

private:
    ThumbnailCache *m_cache = nullptr;
    MediaItem m_item;
    QLabel *m_thumbnail = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_meta = nullptr;
};

} // namespace

HomeView::HomeView(ThumbnailCache *cache, QWidget *parent)
    : QWidget(parent)
    , m_cache(cache)
{
    setObjectName(QStringLiteral("HomePage"));
    setAttribute(Qt::WA_StyledBackground, true);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // T38：legacy 两段（mock 兼容）与 T37 多 section 页二选一。
    m_viewStack = new QStackedWidget(this);
    m_viewStack->setObjectName(QStringLiteral("HomeViewStack"));
    m_legacyPage = new QWidget(m_viewStack);
    m_legacyPage->setObjectName(QStringLiteral("HomeLegacyPage"));
    auto *legacyLayout = new QVBoxLayout(m_legacyPage);
    legacyLayout->setContentsMargins(0, 0, 0, 0);
    legacyLayout->setSpacing(0);

    auto *resumeSection = new QWidget(m_legacyPage);
    resumeSection->setObjectName(QStringLiteral("ResumeSection"));
    m_resumeSection = resumeSection;
    auto *resumeSectionLayout = new QVBoxLayout(resumeSection);
    resumeSectionLayout->setContentsMargins(30, 18, 30, 4);
    resumeSectionLayout->setSpacing(6);

    auto *header = new QHBoxLayout();
    header->setSpacing(10);
    m_resumeTitle = new QLabel(QStringLiteral("继续观看"), resumeSection);
    m_resumeTitle->setObjectName(QStringLiteral("LibraryTitle"));
    header->addWidget(m_resumeTitle);
    header->addStretch(1);
    resumeSectionLayout->addLayout(header);

    auto *scroll = new QScrollArea(resumeSection);
    scroll->setObjectName(QStringLiteral("ResumeScroll"));
    m_resumeScroll = scroll;
    scroll->setWidgetResizable(false);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // 卡片内容高度约 188，加上横向滚动条 10，留 200 避免底部进度条被裁切。
    scroll->setFixedHeight(200);

    m_resumeContent = new QWidget(scroll);
    m_resumeContent->setObjectName(QStringLiteral("ResumeContent"));
    m_resumeLayout = new QHBoxLayout(m_resumeContent);
    m_resumeLayout->setContentsMargins(0, 0, 0, 0);
    m_resumeLayout->setSpacing(14);
    m_resumeLayout->addStretch(1);
    scroll->setWidget(m_resumeContent);
    resumeSectionLayout->addWidget(scroll);

    m_resumeEmpty = new QLabel(QStringLiteral("暂无继续观看（可能已看完，或该账号没有未看完条目）"), resumeSection);
    m_resumeEmpty->setObjectName(QStringLiteral("EmptyState"));
    m_resumeEmpty->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_resumeEmpty->setFixedHeight(40);
    resumeSectionLayout->addWidget(m_resumeEmpty);

    legacyLayout->addWidget(resumeSection);

    m_recent = new LibraryView(m_cache, m_legacyPage);
    m_recent->setObjectName(QStringLiteral("HomeLibrary"));
    m_recent->setTitle(QStringLiteral("最近添加"));
    legacyLayout->addWidget(m_recent, 1);
    m_viewStack->addWidget(m_legacyPage);

    // T38：整页可滚动的多 section 容器。
    m_sectionsScroll = new QScrollArea(m_viewStack);
    m_sectionsScroll->setObjectName(QStringLiteral("HomeSectionsScroll"));
    m_sectionsScroll->setWidgetResizable(true);
    m_sectionsScroll->setFrameShape(QFrame::NoFrame);
    m_sectionsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_sectionsScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_sectionsContent = new QWidget(m_sectionsScroll);
    m_sectionsContent->setObjectName(QStringLiteral("HomeSectionsContent"));
    m_sectionsLayout = new QVBoxLayout(m_sectionsContent);
    m_sectionsLayout->setContentsMargins(30, 18, 30, 24);
    m_sectionsLayout->setSpacing(18);
    m_sectionsLayout->addStretch(1);
    m_sectionsScroll->setWidget(m_sectionsContent);
    m_viewStack->addWidget(m_sectionsScroll);
    m_viewStack->setCurrentWidget(m_legacyPage);

    root->addWidget(m_viewStack);

    connect(m_recent, &LibraryView::itemActivated, this, &HomeView::itemActivated);
    connect(m_recent, &LibraryView::playRequested, this, &HomeView::playRequested);
    connect(m_recent, &LibraryView::loadMoreRequested, this, &HomeView::loadMoreRequested);
    connect(m_recent, &LibraryView::sortChanged, this, &HomeView::sortChanged);
    m_lazyBuildTimer = new QTimer(this);
    m_lazyBuildTimer->setSingleShot(true);
    m_lazyBuildTimer->setInterval(80);
    connect(m_lazyBuildTimer, &QTimer::timeout, this, &HomeView::buildVisibleSections);
    m_thumbnailRefreshTimer = new QTimer(this);
    m_thumbnailRefreshTimer->setSingleShot(true);
    m_thumbnailRefreshTimer->setInterval(120);
    connect(m_thumbnailRefreshTimer, &QTimer::timeout, this, &HomeView::refreshThumbnailsNow);
    connect(m_sectionsScroll->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this](int) { scheduleSectionBuild(); });
    connect(m_cache, &ThumbnailCache::thumbnailReady, this,
            [this](const QString &, const QSize &) { scheduleSectionBuild(); m_thumbnailRefreshTimer->start(); });
}

void HomeView::clearResume()
{
    while (QLayoutItem *item = m_resumeLayout->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }
    m_resumeCards.clear();
    m_resumeLayout->addStretch(1);
}

void HomeView::setResumeItems(const QList<MediaItem> &items)
{
    showLegacyPage();
    qInfo() << "HomeView::setResumeItems: count" << items.size()
            << "oldCount" << m_resumeCount
            << "loading" << m_loading
            << "error" << m_error
            << "sectionVisible" << (m_resumeSection && m_resumeSection->isVisible());
    m_resumeItems = items;
    m_resumeCount = items.size();
    clearResume();
    rebuildResume();
}

QString HomeView::diagnosticSummary() const
{
    QStringList lines;
    const QRect sectionRect = m_resumeSection ? m_resumeSection->geometry() : QRect();
    const QRect scrollRect = m_resumeScroll ? m_resumeScroll->geometry() : QRect();
    const QSize viewportSize = m_resumeScroll && m_resumeScroll->viewport()
                                   ? m_resumeScroll->viewport()->size() : QSize();
    lines.append(QStringLiteral("HomeView: resumeCount=%1 cards=%2 loading=%3 error=%4")
                     .arg(m_resumeCount)
                     .arg(m_resumeCards.size())
                     .arg(m_loading)
                     .arg(m_error));
    lines.append(QStringLiteral("HomeView: sectionVisible=%1 section=(%2,%3 %4x%5) "
                                "scrollVisible=%6 scroll=(%7,%8 %9x%10) viewport=%11x%12 "
                                "content=%13x%14 emptyVisible=%15 emptyText=%16")
                     .arg(m_resumeSection && m_resumeSection->isVisible())
                     .arg(sectionRect.x()).arg(sectionRect.y())
                     .arg(sectionRect.width()).arg(sectionRect.height())
                     .arg(m_resumeScroll && m_resumeScroll->isVisible())
                     .arg(scrollRect.x()).arg(scrollRect.y())
                     .arg(scrollRect.width()).arg(scrollRect.height())
                     .arg(viewportSize.width()).arg(viewportSize.height())
                     .arg(m_resumeContent ? m_resumeContent->width() : 0)
                     .arg(m_resumeContent ? m_resumeContent->height() : 0)
                     .arg(m_resumeEmpty && m_resumeEmpty->isVisible())
                     .arg(m_resumeEmpty ? m_resumeEmpty->text() : QString()));
    if (!m_resumeCards.isEmpty()) {
        QWidget *card = m_resumeCards.first();
        lines.append(QStringLiteral("HomeView: firstCardVisible=%1 hidden=%2 geometry=(%3,%4 %5x%6)")
                         .arg(card->isVisible())
                         .arg(card->isHidden())
                         .arg(card->x()).arg(card->y())
                         .arg(card->width()).arg(card->height()));
    }
    return lines.join(QLatin1Char('\n'));
}

void HomeView::rebuildResume()
{
    qInfo() << "HomeView::rebuildResume: items" << m_resumeItems.size()
            << "cards" << m_resumeCards.size()
            << "loading" << m_loading
            << "error" << m_error
            << "sectionVisible" << (m_resumeSection && m_resumeSection->isVisible());
    if (m_loading && m_resumeItems.isEmpty()) {
        m_resumeEmpty->setText(QStringLiteral("正在加载继续观看…"));
        m_resumeEmpty->setProperty("state", QStringLiteral("loading"));
        m_resumeEmpty->show();
        qInfo() << "HomeView::rebuildResume: loading empty state visible";
        return;
    }
    if (!m_error.isEmpty() && m_resumeItems.isEmpty()) {
        m_resumeEmpty->setText(m_error);
        m_resumeEmpty->setProperty("state", QStringLiteral("error"));
        m_resumeEmpty->show();
        qInfo() << "HomeView::rebuildResume: error empty state visible" << m_error;
        return;
    }
    if (m_resumeItems.isEmpty()) {
        m_resumeEmpty->setText(QStringLiteral("暂无继续观看（可能已看完，或该账号没有未看完条目）"));
        m_resumeEmpty->setProperty("state", QStringLiteral("empty"));
        m_resumeEmpty->show();
        qInfo() << "HomeView::rebuildResume: empty state visible";
        return;
    }
    m_resumeEmpty->hide();

    int index = 0;
    for (const MediaItem &item : m_resumeItems) {
        auto *card = new ResumeCard(item, m_cache, m_resumeContent);
        card->onClick = [this, item]() {
            // 继续观看：优先交给播放链路；T8 未接入前 MainWindow 会退回详情页。
            emit playRequested(item.id);
        };
        m_resumeLayout->insertWidget(index++, card);
        m_resumeCards.append(card);
    }
    // 关键：新增卡片后必须先激活 layout，sizeHint 才会从 0 变成卡片总宽度；
    // 只调 adjustSize() 在 QScrollArea 里可能仍是 0x0，卡片虽创建但高度被压成 0，
    // 首页看起来空白。直接用 layout 的 sizeHint 固定 content 尺寸。
    resizeResumeContent();
    // 首次 show/layout 前 sizeHint 可能还是 0，下一轮事件循环再定一次尺寸。
    QTimer::singleShot(0, this, &HomeView::resizeResumeContent);
    qInfo() << "HomeView::rebuildResume: cards" << m_resumeCards.size()
            << "emptyVisible" << m_resumeEmpty->isVisible()
            << "content" << m_resumeContent->size()
            << "scrollViewport" << (m_resumeScroll && m_resumeScroll->viewport()
                                         ? m_resumeScroll->viewport()->size() : QSize())
            << "sectionVisible" << (m_resumeSection && m_resumeSection->isVisible());
    if (!m_resumeCards.isEmpty()) {
        QWidget *first = m_resumeCards.first();
        qInfo() << "HomeView::rebuildResume: firstCard" << first->geometry()
                << "visible" << first->isVisible() << "hidden" << first->isHidden();
    }
}

void HomeView::resizeResumeContent()
{
    if (!m_resumeContent || !m_resumeLayout)
        return;
    m_resumeLayout->activate();
    const QSize contentHint = m_resumeLayout->sizeHint();
    if (contentHint.width() > 0 && contentHint.height() > 0)
        m_resumeContent->setFixedSize(contentHint);
    else
        m_resumeContent->adjustSize();
    qInfo() << "HomeView::resizeResumeContent: hint" << contentHint
            << "content" << m_resumeContent->size();
}

void HomeView::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // 数据可能在首页隐藏时已经加载；显示后补一次 content 尺寸，避免 0 高度。
    QTimer::singleShot(0, this, &HomeView::resizeResumeContent);
    QTimer::singleShot(0, this, &HomeView::resizeHomeSectionRows);
    if (m_usingSections)
        scheduleSectionBuild();
}

void HomeView::refreshThumbnails()
{
    if (m_thumbnailRefreshTimer)
        m_thumbnailRefreshTimer->start();
    else
        refreshThumbnailsNow();
}

void HomeView::refreshThumbnailsNow()
{
    for (QWidget *widget : m_resumeCards) {
        if (auto *card = dynamic_cast<ResumeCard *>(widget))
            card->refreshThumbnail();
    }
    for (SectionRow &row : m_sectionRows) {
        if (!row.cardsBuilt)
            continue;
        for (QWidget *widget : row.cards) {
            if (auto *card = dynamic_cast<HomeSectionCard *>(widget))
                card->refreshThumbnail();
        }
    }
}

void HomeView::setRecentItems(const QList<MediaItem> &items)
{
    showLegacyPage();
    m_recent->setItems(items);
}

void HomeView::showLegacyPage()
{
    if (m_usingSections) {
        m_usingSections = false;
        if (m_viewStack && m_legacyPage)
            m_viewStack->setCurrentWidget(m_legacyPage);
    }
}

void HomeView::setHomeSections(const QList<HomeSection> &sections)
{
    m_usingSections = true;
    if (m_viewStack && m_sectionsScroll)
        m_viewStack->setCurrentWidget(m_sectionsScroll);
    m_sectionRows.clear();
    clearHomeSections();
    for (const HomeSection &section : sections) {
        SectionRow row;
        row.section = section;
        row.row = new QWidget(m_sectionsContent);
        row.row->setObjectName(QStringLiteral("HomeSectionRow"));
        row.row->setProperty("sectionKind", static_cast<int>(section.kind));
        row.row->setProperty("libraryId", section.libraryId);
        auto *rowLayout = new QVBoxLayout(row.row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(6);

        QString title = section.title;
        if (title.isEmpty()) {
            if (section.kind == HomeSection::Kind::Resume)
                title = QStringLiteral("继续观看");
            else if (section.kind == HomeSection::Kind::Recent)
                title = QStringLiteral("最近添加");
            else
                title = QStringLiteral("媒体库");
        }
        row.section.title = title;
        auto *titleLabel = new QLabel(title, row.row);
        titleLabel->setObjectName(QStringLiteral("HomeSectionTitle"));
        titleLabel->setProperty("sectionKind", static_cast<int>(section.kind));
        rowLayout->addWidget(titleLabel);

        row.scroll = new QScrollArea(row.row);
        row.scroll->setObjectName(QStringLiteral("HomeSectionScroll"));
        row.scroll->setWidgetResizable(false);
        row.scroll->setFrameShape(QFrame::NoFrame);
        row.scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        row.scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        row.scroll->setFixedHeight(200);
        row.content = new QWidget(row.scroll);
        row.content->setObjectName(QStringLiteral("HomeSectionContent"));
        row.layout = new QHBoxLayout(row.content);
        row.layout->setContentsMargins(0, 0, 0, 0);
        row.layout->setSpacing(14);
        row.layout->addStretch(1);
        row.scroll->setWidget(row.content);
        rowLayout->addWidget(row.scroll);

        const bool visible = !section.items.isEmpty() && section.error.isEmpty();
        row.row->setVisible(visible);
        row.scroll->setVisible(false); // 懒构建：进入视口附近才创建卡片
        if (visible) {
            m_sectionsLayout->insertWidget(m_sectionsLayout->count() - 1, row.row);
            m_sectionRows.append(row);
        } else {
            delete row.row;
        }
    }
    m_builtSectionCount = 0;
    m_thumbnailMisses = 0;
    resizeHomeSectionRows();
    scheduleSectionBuild();
    QTimer::singleShot(0, this, &HomeView::buildVisibleSections);
    qInfo() << "HomeView::setHomeSections: sections" << sections.size()
            << "visibleRows" << visibleSectionCount() << "lazyPending" << true;
}

void HomeView::scheduleSectionBuild()
{
    if (m_lazyBuildTimer && m_usingSections)
        m_lazyBuildTimer->start();
}

void HomeView::buildVisibleSections()
{
    if (!m_usingSections || !m_sectionsScroll || !m_sectionsLayout)
        return;
    QElapsedTimer timer;
    timer.start();
    int builtNow = 0;
    const int viewportBottom = m_sectionsScroll->viewport()->height() + 320;
    for (SectionRow &row : m_sectionRows) {
        if (row.cardsBuilt || !row.row || !row.layout)
            continue;
        const QPoint rowTop = row.row->mapTo(m_sectionsScroll->viewport(), QPoint(0, 0));
        // 首屏至少 2 个 section，其余按视口 + 320px 预取。
        if (m_builtSectionCount >= 2 && rowTop.y() > viewportBottom)
            continue;
        int cardIndex = 0;
        for (const MediaItem &item : row.section.items) {
            auto *card = new HomeSectionCard(item, m_cache, row.content);
            const bool playOnClick = row.section.kind == HomeSection::Kind::Resume;
            card->onClick = [this, item, playOnClick]() {
                if (playOnClick)
                    emit playRequested(item.id);
                else
                    emit itemActivated(item.id);
            };
            row.layout->insertWidget(cardIndex++, card);
            row.cards.append(card);
        }
        row.scroll->setVisible(true);
        row.cardsBuilt = true;
        ++m_builtSectionCount;
        ++builtNow;
    }
    if (builtNow > 0) {
        resizeHomeSectionRows();
        qInfo() << "HomeView::buildVisibleSections: builtNow" << builtNow
                << "builtTotal" << m_builtSectionCount << "visibleRows"
                << visibleSectionCount() << "elapsedMs" << timer.elapsed()
                << "thumbnailMisses" << m_thumbnailMisses;
    }
}

int HomeView::visibleSectionCount() const
{
    int count = 0;
    for (const SectionRow &row : m_sectionRows)
        if (row.row && row.row->isVisible())
            ++count;
    return count;
}

QStringList HomeView::sectionTitles() const
{
    QStringList titles;
    for (const SectionRow &row : m_sectionRows)
        titles.append(row.section.title);
    return titles;
}

QList<MediaItem> HomeView::visibleHomeItems() const
{
    if (!m_usingSections)
        return m_recent ? m_recent->visibleItems() : QList<MediaItem>();
    QList<MediaItem> items;
    for (const SectionRow &row : m_sectionRows) {
        if (!row.row || !row.row->isVisible())
            continue;
        items.append(row.section.items);
    }
    return items;
}

QStringList HomeView::visibleHomeItemIds() const
{
    if (!m_usingSections)
        return m_recent ? m_recent->visibleItemIds() : QStringList();
    QStringList ids;
    for (const SectionRow &row : m_sectionRows) {
        if (!row.row || !row.row->isVisible())
            continue;
        for (const MediaItem &item : row.section.items)
            ids.append(item.id);
    }
    return ids;
}

void HomeView::clearHomeSections()
{
    if (!m_sectionsLayout)
        return;
    while (m_sectionsLayout->count() > 0) {
        QLayoutItem *item = m_sectionsLayout->takeAt(0);
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    m_sectionRows.clear();
    m_builtSectionCount = 0;
    if (m_lazyBuildTimer)
        m_lazyBuildTimer->stop();
    m_sectionsLayout->addStretch(1);
}

void HomeView::resizeHomeSectionRows()
{
    for (SectionRow &row : m_sectionRows) {
        if (!row.layout || !row.content)
            continue;
        row.layout->activate();
        const QSize hint = row.layout->sizeHint();
        if (hint.width() > 0 && hint.height() > 0)
            row.content->setFixedSize(hint);
    }
}

void HomeView::setLoading(bool loading, const QString &message)
{
    m_loading = loading;
    m_error.clear();
    if (loading) {
        m_recent->setInitialLoading(true, message);
        if (m_resumeItems.isEmpty()) {
            m_resumeEmpty->setText(QStringLiteral("正在加载继续观看…"));
            m_resumeEmpty->setProperty("state", QStringLiteral("loading"));
            m_resumeEmpty->show();
        }
    } else {
        m_recent->setInitialLoading(false);
        rebuildResume();
    }
}

void HomeView::setError(const QString &message)
{
    m_loading = false;
    m_error = message;
    m_recent->setError(message);
    if (m_resumeItems.isEmpty()) {
        m_resumeEmpty->setText(message);
        m_resumeEmpty->setProperty("state", QStringLiteral("error"));
        m_resumeEmpty->show();
    }
}

} // namespace ui
