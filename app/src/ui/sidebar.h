#pragma once

#include <QList>
#include <QString>
#include <QWidget>

class QButtonGroup;
class QLabel;
class QPushButton;
class QVBoxLayout;

namespace ui {

// 侧边栏里的一个媒体库入口（来自 /Users/{userId}/Views）。
struct SidebarSection
{
    QString id;
    QString name;
    QString collectionType; // movies / tvshows / boxsets / ...
};

// 左侧导航栏：品牌区 + 首页 + 动态媒体库列表 + 设置 + 服务器状态卡片。
// 导航索引约定：0 = 首页，1..N = 媒体库，N+1 = 设置。
class Sidebar : public QWidget
{
    Q_OBJECT
public:
    explicit Sidebar(QWidget *parent = nullptr);

    void setSections(const QList<SidebarSection> &sections);
    QList<SidebarSection> sections() const { return m_sections; }

    // 导航索引（0 = 首页，1..N = 媒体库，N+1 = 设置）。
    void setCurrentIndex(int index);
    int currentIndex() const { return m_currentIndex; }

    // 服务器卡片：mock 模式与真实登录共用。
    void setServerInfo(const QString &serverName, const QString &userName, bool mock);

signals:
    void sectionSelected(int index);

private:
    void buildUi();
    void rebuildNavigation();

    QVBoxLayout *m_navLayout = nullptr;
    QButtonGroup *m_group = nullptr;
    QList<QPushButton *> m_buttons;
    QList<SidebarSection> m_sections;
    QLabel *m_sectionLabel = nullptr;

    QLabel *m_serverName = nullptr;
    QLabel *m_serverStatus = nullptr;
    QLabel *m_avatar = nullptr;
    QLabel *m_version = nullptr;
    int m_currentIndex = 0;
};

} // namespace ui
