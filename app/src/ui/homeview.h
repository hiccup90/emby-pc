#pragma once

#include "librarydata.h"
#include "uitypes.h"

#include <QWidget>

class QGridLayout;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QScrollArea;
class QShowEvent;
class QStackedWidget;
class QVBoxLayout;

namespace ui {

class LibraryView;
class ThumbnailCache;

// 首页：继续观看（Resume 横向列表）+ 最近添加（复用 LibraryView 的海报网格）。
// 数据由 MainWindow/LibraryData 注入，本类只负责展示与转发交互信号。
class HomeView : public QWidget
{
    Q_OBJECT
public:
    explicit HomeView(ThumbnailCache *cache, QWidget *parent = nullptr);

    void setResumeItems(const QList<MediaItem> &items);
    void setRecentItems(const QList<MediaItem> &items);
    // T38：按折叠顺序渲染 T37 的多行首页（resume/recent/每库最新）。
    void setHomeSections(const QList<HomeSection> &sections);
    int sectionCount() const { return m_sectionRows.size(); }
    int visibleSectionCount() const;
    int builtSectionCount() const { return m_builtSectionCount; }
    QStringList sectionTitles() const;
    QList<MediaItem> visibleHomeItems() const;
    QStringList visibleHomeItemIds() const;
    QList<MediaItem> sectionItems(HomeSection::Kind kind,
                                  const QString &libraryId = QString()) const;
    void setLoading(bool loading, const QString &message = QString());
    void setError(const QString &message);

protected:
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

public:

    LibraryView *recentView() const { return m_recent; }
    int resumeItemCount() const { return m_resumeCount; }
    int resumeCardCount() const { return m_resumeCards.size(); }

    // T24：诊断导出用，包含 resume 数/卡片数/空态可见性/布局几何。
    QString diagnosticSummary() const;

signals:
    void itemActivated(const QString &itemId);
    void playRequested(const QString &itemId);
    // T40：最近添加/媒体库标题点击，跳转整页。
    void sectionActivated(int kind, const QString &libraryId);
    void loadMoreRequested();
    void sortChanged(int mode);
    void retryRequested();

private:
    struct SectionRow
    {
        QWidget *row = nullptr;
        QScrollArea *scroll = nullptr;
        QWidget *content = nullptr;
        QHBoxLayout *layout = nullptr;
        QWidget *preview = nullptr;
        QGridLayout *grid = nullptr;
        QLabel *titleLabel = nullptr;
        QList<QWidget *> cards;
        HomeSection section;
        bool cardsBuilt = false;
    };

    void clearResume();
    void rebuildResume();
    void resizeResumeContent();
    void rebuildHomeSections();
    void clearHomeSections();
    void resizeHomeSectionRows();
    void updateSectionsContentGeometry();
    void setSectionsBanner(const QString &text, const QString &state, bool retry);
    void showLegacyPage();
    void buildVisibleSections();
    void scheduleSectionBuild();
    void layoutPreviewRow(SectionRow &row);
    void relayoutPreviewRows();
    void refreshThumbnails();
    void refreshThumbnailsNow();

    ThumbnailCache *m_cache = nullptr;
    LibraryView *m_recent = nullptr;
    QWidget *m_resumeSection = nullptr;
    QScrollArea *m_resumeScroll = nullptr;
    QWidget *m_resumeContent = nullptr;
    QHBoxLayout *m_resumeLayout = nullptr;
    QLabel *m_resumeEmpty = nullptr;
    QLabel *m_resumeTitle = nullptr;
    QList<MediaItem> m_resumeItems;
    QList<QWidget *> m_resumeCards;
    QStackedWidget *m_viewStack = nullptr;
    QWidget *m_legacyPage = nullptr;
    QScrollArea *m_sectionsScroll = nullptr;
    QWidget *m_sectionsContent = nullptr;
    QVBoxLayout *m_sectionsLayout = nullptr;
    QWidget *m_sectionsBanner = nullptr;
    QLabel *m_sectionsBannerLabel = nullptr;
    QPushButton *m_sectionsRetryButton = nullptr;
    QList<SectionRow> m_sectionRows;
    int m_builtSectionCount = 0;
    int m_thumbnailMisses = 0;
    QTimer *m_lazyBuildTimer = nullptr;
    QTimer *m_previewLayoutTimer = nullptr;
    QTimer *m_thumbnailRefreshTimer = nullptr;
    bool m_usingSections = false;
    bool m_loading = false;
    QString m_error;
    int m_resumeCount = 0;
};

} // namespace ui
