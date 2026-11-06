#include "detailview.h"

#include "icons.h"
#include "thumbnailcache.h"

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>

#include <functional>

namespace ui {
namespace {

QPixmap roundedPixmap(const QPixmap &source, const QSize &pixelSize, int radius)
{
    if (source.isNull())
        return {};
    QPixmap scaled = source.scaled(pixelSize, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    QPixmap rounded(pixelSize);
    rounded.fill(Qt::transparent);
    QPainter p(&rounded);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(0, 0, pixelSize.width(), pixelSize.height()), radius, radius);
    p.setClipPath(clip);
    const QRect target((pixelSize.width() - scaled.width()) / 2,
                       (pixelSize.height() - scaled.height()) / 2,
                       scaled.width(), scaled.height());
    p.drawPixmap(target, scaled);
    p.end();
    return rounded;
}

QString formatRuntime(int minutes)
{
    if (minutes <= 0)
        return QStringLiteral("—");
    const int h = minutes / 60;
    const int m = minutes % 60;
    if (h <= 0)
        return QStringLiteral("%1 分钟").arg(m);
    if (m == 0)
        return QStringLiteral("%1 小时").arg(h);
    return QStringLiteral("%1 小时 %2 分").arg(h).arg(m);
}

} // namespace

// 背景大图绘制面板
class BackdropPanel : public QWidget
{
public:
    explicit BackdropPanel(ThumbnailCache *cache, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_cache(cache)
    {
        setObjectName(QStringLiteral("DetailHero"));
        setAttribute(Qt::WA_StyledBackground, false);
    }

    void setKey(const QString &key)
    {
        m_key = key;
        update();
    }

    void refresh() { update(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);

        const qreal dpr = devicePixelRatioF();
        const QSize pixelSize(qRound(width() * dpr), qRound(height() * dpr));
        QPixmap backdrop;
        if (m_cache && !m_key.isEmpty())
            backdrop = m_cache->get(m_key, pixelSize);

        if (backdrop.isNull()) {
            QLinearGradient fallback(0, 0, width(), height());
            fallback.setColorAt(0.0, QColor(0x15, 0x20, 0x33));
            fallback.setColorAt(1.0, QColor(0x0B, 0x12, 0x20));
            p.fillRect(rect(), fallback);
            if (m_cache && !m_key.isEmpty())
                m_cache->request(m_key, pixelSize);
        } else {
            p.drawPixmap(rect(), backdrop);
        }

        QLinearGradient leftShade(0, 0, width() * 0.85, 0);
        leftShade.setColorAt(0.0, QColor(0x0B, 0x12, 0x20, 245));
        leftShade.setColorAt(0.55, QColor(0x0B, 0x12, 0x20, 150));
        leftShade.setColorAt(1.0, QColor(0x0B, 0x12, 0x20, 40));
        p.fillRect(rect(), leftShade);

        QLinearGradient bottomShade(0, height() * 0.55, 0, height());
        bottomShade.setColorAt(0.0, QColor(0x0B, 0x12, 0x20, 0));
        bottomShade.setColorAt(1.0, QColor(0x0B, 0x12, 0x20, 235));
        p.fillRect(rect(), bottomShade);
    }

private:
    ThumbnailCache *m_cache = nullptr;
    QString m_key;
};

// 单集行：缩略图 + 标题 + 时长 + 进度
class EpisodeRow : public QWidget
{
public:
    EpisodeRow(const Episode &episode, int seasonNumber, ThumbnailCache *cache,
               const QString &thumbnailKey, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_cache(cache)
        , m_thumbnailKey(thumbnailKey)
    {
        setObjectName(QStringLiteral("EpisodeRow"));
        setAttribute(Qt::WA_StyledBackground, true);
        setCursor(Qt::PointingHandCursor);

        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(12, 8, 12, 8);
        layout->setSpacing(14);

        m_thumbnail = new QLabel(this);
        m_thumbnail->setObjectName(QStringLiteral("EpisodeThumbnail"));
        m_thumbnail->setFixedSize(132, 74);
        layout->addWidget(m_thumbnail);

        auto *textColumn = new QVBoxLayout();
        textColumn->setSpacing(4);
        auto *title = new QLabel(QStringLiteral("%1. %2")
                                     .arg(episode.number, 2, 10, QLatin1Char('0'))
                                     .arg(episode.title),
                                 this);
        title->setObjectName(QStringLiteral("EpisodeTitle"));
        auto *meta = new QLabel(QStringLiteral("%1 · %2")
                                    .arg(formatRuntime(episode.runtimeMinutes),
                                         episode.overview.simplified().left(26) + QStringLiteral("…")),
                                this);
        meta->setObjectName(QStringLiteral("EpisodeMeta"));
        meta->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        textColumn->addWidget(title);
        textColumn->addWidget(meta);
        layout->addLayout(textColumn, 1);

        if (episode.progress > 0.01 && episode.progress < 0.99) {
            auto *progress = new QProgressBar(this);
            progress->setObjectName(QStringLiteral("EpisodeProgress"));
            progress->setRange(0, 100);
            progress->setValue(qRound(episode.progress * 100));
            progress->setTextVisible(false);
            progress->setFixedSize(92, 4);
            layout->addWidget(progress, 0, Qt::AlignVCenter);
        } else if (episode.played) {
            auto *badge = new QLabel(QStringLiteral("已看完"), this);
            badge->setObjectName(QStringLiteral("EpisodePlayedBadge"));
            layout->addWidget(badge, 0, Qt::AlignVCenter);
        }

        m_season = seasonNumber;
        m_episode = episode.number;
    }

    void setThumbnail(const QPixmap &pixmap)
    {
        if (!pixmap.isNull())
            m_thumbnail->setPixmap(pixmap);
    }

    QString thumbnailKey() const { return m_thumbnailKey; }
    int seasonNumber() const { return m_season; }
    int episodeNumber() const { return m_episode; }

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
    QString m_thumbnailKey;
    QLabel *m_thumbnail = nullptr;
    int m_season = 0;
    int m_episode = 0;
};

DetailView::DetailView(ThumbnailCache *cache, QWidget *parent)
    : QWidget(parent)
    , m_cache(cache)
{
    buildUi();
    connect(m_cache, &ThumbnailCache::thumbnailReady, this,
            [this](const QString &key, const QSize &) {
                if (key == m_item.posterKey)
                    updateHero();
                else if (key == m_item.backdropKey && m_hero)
                    m_hero->refresh();
                else
                    updateEpisodeThumbnails();
            });
}

void DetailView::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *topBar = new QWidget(this);
    topBar->setObjectName(QStringLiteral("DetailTopBar"));
    auto *topLayout = new QHBoxLayout(topBar);
    topLayout->setContentsMargins(32, 8, 32, 8);
    topLayout->setSpacing(10);
    m_backButton = new QPushButton(icon(Icon::Back, QColor(0xE2, 0xE8, 0xF0), 18),
                                   QStringLiteral("返回"), topBar);
    m_backButton->setObjectName(QStringLiteral("DetailBackButton"));
    m_backButton->setProperty("role", QStringLiteral("ghost"));
    m_backButton->setCursor(Qt::PointingHandCursor);
    topLayout->addWidget(m_backButton);
    auto *crumb = new QLabel(QStringLiteral("媒体库 / 详情"), topBar);
    crumb->setObjectName(QStringLiteral("DetailBreadcrumb"));
    topLayout->addWidget(crumb);
    topLayout->addStretch(1);
    root->addWidget(topBar);

    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName(QStringLiteral("DetailScroll"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *content = new QWidget();
    content->setObjectName(QStringLiteral("DetailContent"));
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);

    m_hero = new BackdropPanel(m_cache, content);
    m_hero->setFixedHeight(360);
    auto *heroLayout = new QHBoxLayout(m_hero);
    heroLayout->setContentsMargins(32, 28, 32, 24);
    heroLayout->setSpacing(28);

    m_poster = new QLabel(m_hero);
    m_poster->setObjectName(QStringLiteral("DetailPoster"));
    m_poster->setFixedSize(188, 282);
    m_poster->setAlignment(Qt::AlignCenter);
    heroLayout->addWidget(m_poster, 0, Qt::AlignBottom);

    auto *infoColumn = new QVBoxLayout();
    infoColumn->setSpacing(8);
    infoColumn->addStretch(1);

    m_typeBadge = new QLabel(m_hero);
    m_typeBadge->setObjectName(QStringLiteral("DetailTypeBadge"));
    m_typeBadge->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    infoColumn->addWidget(m_typeBadge);

    m_title = new QLabel(m_hero);
    m_title->setObjectName(QStringLiteral("DetailTitle"));
    m_title->setWordWrap(true);
    m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    infoColumn->addWidget(m_title);

    m_meta = new QLabel(m_hero);
    m_meta->setObjectName(QStringLiteral("DetailMeta"));
    infoColumn->addWidget(m_meta);

    m_tagline = new QLabel(m_hero);
    m_tagline->setObjectName(QStringLiteral("DetailTagline"));
    m_tagline->setWordWrap(true);
    m_tagline->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    infoColumn->addWidget(m_tagline);

    auto *actions = new QHBoxLayout();
    actions->setSpacing(8);
    m_playButton = new QPushButton(icon(Icon::Play, QColor(0xFF, 0xFF, 0xFF), 18),
                                   QStringLiteral("播放"), m_hero);
    m_playButton->setObjectName(QStringLiteral("DetailPlayButton"));
    m_playButton->setProperty("role", QStringLiteral("primary"));
    m_playButton->setCursor(Qt::PointingHandCursor);
    m_favButton = new QPushButton(icon(Icon::Heart, QColor(0xE2, 0xE8, 0xF0), 18),
                                  QStringLiteral("收藏"), m_hero);
    m_favButton->setObjectName(QStringLiteral("DetailFavButton"));
    m_favButton->setProperty("role", QStringLiteral("secondary"));
    m_favButton->setCursor(Qt::PointingHandCursor);
    auto *more = new QPushButton(icon(Icon::More, QColor(0xE2, 0xE8, 0xF0), 18), QString(), m_hero);
    more->setObjectName(QStringLiteral("DetailMoreButton"));
    more->setProperty("role", QStringLiteral("secondary"));
    more->setFixedWidth(44);
    more->setCursor(Qt::PointingHandCursor);
    actions->addWidget(m_playButton);
    actions->addWidget(m_favButton);
    actions->addWidget(more);
    actions->addStretch(1);
    infoColumn->addLayout(actions);
    infoColumn->addStretch(1);

    heroLayout->addLayout(infoColumn, 1);
    contentLayout->addWidget(m_hero);

    auto *body = new QWidget(content);
    body->setObjectName(QStringLiteral("DetailBody"));
    auto *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(32, 24, 32, 48);
    bodyLayout->setSpacing(16);

    m_chipsRow = new QWidget(body);
    auto *chipsLayout = new QHBoxLayout(m_chipsRow);
    chipsLayout->setContentsMargins(0, 0, 0, 0);
    chipsLayout->setSpacing(10);
    chipsLayout->addStretch(1);
    bodyLayout->addWidget(m_chipsRow);

    m_sectionTitle = new QLabel(QStringLiteral("简介"), body);
    m_sectionTitle->setObjectName(QStringLiteral("SectionTitle"));
    bodyLayout->addWidget(m_sectionTitle);

    m_overview = new QLabel(body);
    m_overview->setObjectName(QStringLiteral("DetailOverview"));
    m_overview->setWordWrap(true);
    m_overview->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_overview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bodyLayout->addWidget(m_overview);

    m_overviewToggle = new QPushButton(QStringLiteral("展开"), body);
    m_overviewToggle->setObjectName(QStringLiteral("DetailOverviewToggle"));
    m_overviewToggle->setProperty("role", QStringLiteral("ghost"));
    m_overviewToggle->setCursor(Qt::PointingHandCursor);
    m_overviewToggle->hide();
    bodyLayout->addWidget(m_overviewToggle, 0, Qt::AlignLeft);
    connect(m_overviewToggle, &QPushButton::clicked, this, [this]() {
        m_overviewExpanded = !m_overviewExpanded;
        m_overviewToggle->setText(m_overviewExpanded ? QStringLiteral("收起")
                                                     : QStringLiteral("展开"));
        m_overview->setMaximumHeight(m_overviewExpanded ? QWIDGETSIZE_MAX : 66);
    });

    m_episodesSection = new QWidget(body);
    auto *episodesLayout = new QVBoxLayout(m_episodesSection);
    episodesLayout->setContentsMargins(0, 16, 0, 0);
    episodesLayout->setSpacing(10);

    auto *episodesHeader = new QHBoxLayout();
    episodesHeader->setSpacing(12);
    auto *episodeTitle = new QLabel(QStringLiteral("剧集"), m_episodesSection);
    episodeTitle->setObjectName(QStringLiteral("SectionTitle"));
    episodesHeader->addWidget(episodeTitle);
    m_seasonCombo = new QComboBox(m_episodesSection);
    m_seasonCombo->setObjectName(QStringLiteral("SeasonCombo"));
    m_seasonCombo->setCursor(Qt::PointingHandCursor);
    episodesHeader->addWidget(m_seasonCombo);
    episodesHeader->addStretch(1);
    m_episodeCount = new QLabel(m_episodesSection);
    m_episodeCount->setObjectName(QStringLiteral("EpisodeCount"));
    episodesHeader->addWidget(m_episodeCount);
    episodesLayout->addLayout(episodesHeader);

    m_episodeLayout = new QVBoxLayout();
    m_episodeLayout->setSpacing(8);
    episodesLayout->addLayout(m_episodeLayout);
    bodyLayout->addWidget(m_episodesSection);

    bodyLayout->addStretch(1);
    contentLayout->addWidget(body);

    m_scroll->setWidget(content);
    root->addWidget(m_scroll, 1);

    connect(m_backButton, &QPushButton::clicked, this, &DetailView::backRequested);
    connect(m_playButton, &QPushButton::clicked, this, [this]() {
        if (!m_item.id.isEmpty())
            emit playRequested(m_item.id, m_defaultSeason, m_defaultEpisode);
    });
    connect(m_favButton, &QPushButton::clicked, this, [this]() {
        m_favButton->setText(QStringLiteral("已收藏"));
        m_favButton->setEnabled(false);
    });
    connect(m_seasonCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { rebuildEpisodes(); });

    m_episodesSection->hide();
}

void DetailView::setItem(const MediaItem &item)
{
    m_item = item;
    m_cache->setTitleHint(item.posterKey, item.title);
    m_defaultSeason = -1;
    m_defaultEpisode = -1;

    updateHero();
    updateChips();
    m_overview->setText(item.overview);
    m_overviewExpanded = true;
    const bool longOverview = item.overview.size() > 140;
    m_overviewToggle->setVisible(longOverview);
    m_overviewToggle->setText(QStringLiteral("展开"));
    m_overview->setMaximumHeight(longOverview ? 66 : QWIDGETSIZE_MAX);
    m_sectionTitle->setText(QStringLiteral("简介"));

    m_seasonCombo->blockSignals(true);
    m_seasonCombo->clear();
    for (const Season &season : item.seasons)
        m_seasonCombo->addItem(season.name, season.number);
    m_seasonCombo->blockSignals(false);

    m_episodesSection->setVisible(item.isSeries() && !item.seasons.isEmpty());
    m_favButton->setText(QStringLiteral("收藏"));
    m_favButton->setEnabled(true);
    rebuildEpisodes();
    updatePlayButton();

    if (m_scroll)
        m_scroll->verticalScrollBar()->setValue(0);
}

void DetailView::updateHero()
{
    if (!m_hero)
        return;

    m_hero->setKey(m_item.backdropKey);
    m_typeBadge->setText(m_item.isSeries() ? QStringLiteral("剧集") : QStringLiteral("电影"));
    m_title->setText(m_item.title);
    m_meta->setText(QStringLiteral("%1 · %2 · %3 · ★ %4")
                        .arg(m_item.year)
                        .arg(m_item.genreText().isEmpty() ? QStringLiteral("影视") : m_item.genreText())
                        .arg(formatRuntime(m_item.runtimeMinutes))
                        .arg(m_item.communityRating, 0, 'f', 1));
    QString tagline = m_item.tagline;
    if (tagline.isEmpty() && m_item.isSeries())
        tagline = QStringLiteral("全 %1 季").arg(m_item.seasons.size());
    m_tagline->setText(tagline);
    m_tagline->setVisible(!m_tagline->text().isEmpty());

    const QSize posterSize(188, 282);
    const QPixmap poster = m_cache->get(m_item.posterKey, posterSize);
    if (poster.isNull()) {
        m_poster->setPixmap(QPixmap());
        m_poster->setText(QStringLiteral("海报"));
        m_cache->request(m_item.posterKey, posterSize);
    } else {
        m_poster->setText(QString());
        m_poster->setPixmap(roundedPixmap(poster, posterSize, 10));
        m_poster->setScaledContents(true);
    }
}

void DetailView::updateChips()
{
    if (!m_chipsRow)
        return;
    auto *layout = qobject_cast<QHBoxLayout *>(m_chipsRow->layout());
    if (!layout)
        return;
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }

    const QStringList chips = {
        QString::number(m_item.year),
        m_item.genreText().isEmpty() ? QStringLiteral("未分类") : m_item.genreText(),
        formatRuntime(m_item.runtimeMinutes),
        QStringLiteral("★ %1").arg(m_item.communityRating, 0, 'f', 1),
        m_item.officialRating,
    };
    for (const QString &text : chips) {
        auto *chip = new QLabel(text, m_chipsRow);
        chip->setObjectName(QStringLiteral("DetailChip"));
        layout->addWidget(chip);
    }
    layout->addStretch(1);
}

void DetailView::rebuildEpisodes()
{
    if (!m_episodeLayout)
        return;

    while (QLayoutItem *item = m_episodeLayout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }

    if (!m_item.isSeries() || m_item.seasons.isEmpty()) {
        m_episodeCount->clear();
        return;
    }

    int seasonNumber = m_seasonCombo->currentData().toInt();
    if (seasonNumber <= 0)
        seasonNumber = m_item.seasons.first().number;

    const Season *season = nullptr;
    for (const Season &candidate : m_item.seasons) {
        if (candidate.number == seasonNumber) {
            season = &candidate;
            break;
        }
    }
    if (!season)
        return;

    m_episodeCount->setText(QStringLiteral("%1 集").arg(season->episodes.size()));

    for (const Episode &episode : season->episodes) {
        const QString thumbKey = episode.imageKey.isEmpty()
                                     ? QStringLiteral("poster:ep/%1/%2/%3")
                                           .arg(m_item.id)
                                           .arg(seasonNumber)
                                           .arg(episode.number)
                                     : episode.imageKey;
        auto *row = new EpisodeRow(episode, seasonNumber, m_cache, thumbKey, m_episodesSection);
        row->onClick = [this, seasonNumber, episode]() {
            m_defaultSeason = seasonNumber;
            m_defaultEpisode = episode.number;
            emit episodeActivated(m_item.id, seasonNumber, episode.number);
        };
        m_episodeLayout->addWidget(row);

        const QSize thumbSize(132, 74);
        const QPixmap thumb = m_cache->get(thumbKey, thumbSize);
        if (thumb.isNull())
            m_cache->request(thumbKey, thumbSize);
        else
            row->setThumbnail(roundedPixmap(thumb, thumbSize, 6));
    }
}

void DetailView::updatePlayButton()
{
    m_defaultSeason = -1;
    m_defaultEpisode = -1;
    if (!m_item.isSeries()) {
        m_playButton->setText(QStringLiteral("播放"));
        return;
    }

    int bestSeason = -1;
    int bestEpisode = -1;
    bool foundProgress = false;
    for (const Season &season : m_item.seasons) {
        for (const Episode &episode : season.episodes) {
            if (episode.progress > 0.01 && episode.progress < 0.99) {
                bestSeason = season.number;
                bestEpisode = episode.number;
                foundProgress = true;
                break;
            }
        }
        if (foundProgress)
            break;
    }
    if (!foundProgress) {
        for (const Season &season : m_item.seasons) {
            for (const Episode &episode : season.episodes) {
                if (!episode.played) {
                    bestSeason = season.number;
                    bestEpisode = episode.number;
                    break;
                }
            }
            if (bestEpisode >= 0)
                break;
        }
    }
    if (bestSeason < 0 && !m_item.seasons.isEmpty() && !m_item.seasons.first().episodes.isEmpty()) {
        bestSeason = m_item.seasons.first().number;
        bestEpisode = m_item.seasons.first().episodes.first().number;
    }

    m_defaultSeason = bestSeason;
    m_defaultEpisode = bestEpisode;
    if (bestSeason > 0 && bestEpisode > 0) {
        m_playButton->setText(QStringLiteral("继续观看 S%1:E%2")
                                  .arg(bestSeason)
                                  .arg(bestEpisode, 2, 10, QLatin1Char('0')));
        if (m_seasonCombo->count() > 0) {
            const int index = m_seasonCombo->findData(bestSeason);
            if (index >= 0)
                m_seasonCombo->setCurrentIndex(index);
        }
    } else {
        m_playButton->setText(QStringLiteral("播放"));
    }
}

void DetailView::updateEpisodeThumbnails()
{
    if (!m_episodeLayout)
        return;
    for (int i = 0; i < m_episodeLayout->count(); ++i) {
        auto *row = dynamic_cast<EpisodeRow *>(m_episodeLayout->itemAt(i)->widget());
        if (!row)
            continue;
        const QSize thumbSize(132, 74);
        const QPixmap thumb = m_cache->get(row->thumbnailKey(), thumbSize);
        if (!thumb.isNull())
            row->setThumbnail(roundedPixmap(thumb, thumbSize, 6));
    }
}

} // namespace ui