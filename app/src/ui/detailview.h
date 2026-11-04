#pragma once

#include "uitypes.h"

#include <QWidget>

class QComboBox;
class QLabel;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

namespace ui {

class BackdropPanel;
class ThumbnailCache;

// 详情页：背景图 + 海报占位、标题/元数据、简介、播放按钮、季/集列表（mock）。
// 只发信号，不直接操作播放器，方便后续接 PlaybackController。
class DetailView : public QWidget
{
    Q_OBJECT
public:
    explicit DetailView(ThumbnailCache *cache, QWidget *parent = nullptr);

    void setItem(const MediaItem &item);
    const MediaItem &currentItem() const { return m_item; }

signals:
    void backRequested();
    void playRequested(const QString &itemId, int seasonNumber, int episodeNumber);
    void episodeActivated(const QString &itemId, int seasonNumber, int episodeNumber);

private:
    void buildUi();
    void updateHero();
    void updateChips();
    void rebuildEpisodes();
    void updatePlayButton();
    void updateEpisodeThumbnails();

    ThumbnailCache *m_cache = nullptr;
    MediaItem m_item;

    BackdropPanel *m_hero = nullptr;
    QLabel *m_poster = nullptr;
    QLabel *m_typeBadge = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_meta = nullptr;
    QLabel *m_tagline = nullptr;
    QLabel *m_overview = nullptr;
    QLabel *m_sectionTitle = nullptr;
    QLabel *m_episodeCount = nullptr;
    QPushButton *m_backButton = nullptr;
    QPushButton *m_playButton = nullptr;
    QPushButton *m_favButton = nullptr;
    QComboBox *m_seasonCombo = nullptr;
    QScrollArea *m_scroll = nullptr;
    QWidget *m_episodesSection = nullptr;
    QVBoxLayout *m_episodeLayout = nullptr;
    QWidget *m_chipsRow = nullptr;

    int m_defaultSeason = -1;
    int m_defaultEpisode = -1;
};

} // namespace ui