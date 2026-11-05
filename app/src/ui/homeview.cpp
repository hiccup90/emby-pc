#include "homeview.h"

#include "icons.h"
#include "libraryview.h"
#include "thumbnailcache.h"

#include <QDebug>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QProgressBar>
#include <QScrollArea>
#include <QShowEvent>
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

    auto *resumeSection = new QWidget(this);
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

    root->addWidget(resumeSection);

    m_recent = new LibraryView(m_cache, this);
    m_recent->setObjectName(QStringLiteral("HomeLibrary"));
    m_recent->setTitle(QStringLiteral("最近添加"));
    root->addWidget(m_recent, 1);

    connect(m_recent, &LibraryView::itemActivated, this, &HomeView::itemActivated);
    connect(m_recent, &LibraryView::playRequested, this, &HomeView::playRequested);
    connect(m_recent, &LibraryView::loadMoreRequested, this, &HomeView::loadMoreRequested);
    connect(m_recent, &LibraryView::sortChanged, this, &HomeView::sortChanged);
    connect(m_cache, &ThumbnailCache::thumbnailReady, this,
            [this](const QString &, const QSize &) { refreshThumbnails(); });
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
}

void HomeView::refreshThumbnails()
{
    for (QWidget *widget : m_resumeCards) {
        if (auto *card = dynamic_cast<ResumeCard *>(widget))
            card->refreshThumbnail();
    }
}

void HomeView::setRecentItems(const QList<MediaItem> &items)
{
    m_recent->setItems(items);
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
