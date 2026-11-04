#pragma once

#include "uitypes.h"

#include <QWidget>

class QHBoxLayout;
class QLabel;

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
    void setLoading(bool loading, const QString &message = QString());
    void setError(const QString &message);

    LibraryView *recentView() const { return m_recent; }
    int resumeItemCount() const { return m_resumeCount; }

signals:
    void itemActivated(const QString &itemId);
    void playRequested(const QString &itemId);
    void loadMoreRequested();
    void sortChanged(int mode);

private:
    void clearResume();
    void rebuildResume();
    void refreshThumbnails();

    ThumbnailCache *m_cache = nullptr;
    LibraryView *m_recent = nullptr;
    QWidget *m_resumeContent = nullptr;
    QHBoxLayout *m_resumeLayout = nullptr;
    QLabel *m_resumeEmpty = nullptr;
    QLabel *m_resumeTitle = nullptr;
    QList<MediaItem> m_resumeItems;
    QList<QWidget *> m_resumeCards;
    bool m_loading = false;
    QString m_error;
    int m_resumeCount = 0;
};

} // namespace ui
