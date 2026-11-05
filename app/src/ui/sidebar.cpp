#include "sidebar.h"

#include "icons.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QVBoxLayout>

namespace ui {
namespace {

class LogoMark : public QWidget
{
public:
    explicit LogoMark(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setFixedSize(38, 38);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        QLinearGradient g(0, 0, width(), height());
        g.setColorAt(0.0, QColor(0x3B, 0x82, 0xF6));
        g.setColorAt(1.0, QColor(0x1D, 0x4E, 0xD8));
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawRoundedRect(rect(), 10, 10);
        p.setBrush(QColor(0xFF, 0xFF, 0xFF));
        QPainterPath tri;
        tri.moveTo(15.5, 12);
        tri.lineTo(27.0, 19);
        tri.lineTo(15.5, 26);
        tri.closeSubpath();
        p.drawPath(tri);
    }
};

Icon iconForCollectionType(const QString &collectionType)
{
    if (collectionType == QLatin1String("movies"))
        return Icon::Movie;
    if (collectionType == QLatin1String("tvshows"))
        return Icon::Tv;
    if (collectionType == QLatin1String("music"))
        return Icon::Volume;
    if (collectionType == QLatin1String("boxsets"))
        return Icon::Grid;
    return Icon::Grid;
}

QIcon makeNavIcon(Icon kind)
{
    QIcon result;
    result.addPixmap(iconPixmap(kind, QColor(0x94, 0xA3, 0xB8), 18), QIcon::Normal, QIcon::Off);
    result.addPixmap(iconPixmap(kind, QColor(0xF8, 0xFA, 0xFC), 18), QIcon::Normal, QIcon::On);
    result.addPixmap(iconPixmap(kind, QColor(0xF8, 0xFA, 0xFC), 18), QIcon::Active, QIcon::Off);
    return result;
}

QPushButton *makeNavButton(Icon iconKind, const QString &text, const QString &objectName,
                           QWidget *parent)
{
    auto *button = new QPushButton(makeNavIcon(iconKind), text, parent);
    button->setObjectName(objectName);
    button->setProperty("navButton", true);
    button->setCheckable(true);
    button->setAutoExclusive(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setIconSize(QSize(18, 18));
    button->setFixedHeight(42);
    button->setToolTip(text);
    return button;
}

} // namespace

Sidebar::Sidebar(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("Sidebar"));
    setAttribute(Qt::WA_StyledBackground, true);
    setMinimumWidth(180);
    setMaximumWidth(320);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    buildUi();
    rebuildNavigation();
}

void Sidebar::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 20, 16, 18);
    root->setSpacing(6);

    // 品牌
    auto *brand = new QWidget(this);
    auto *brandLayout = new QHBoxLayout(brand);
    brandLayout->setContentsMargins(2, 0, 0, 0);
    brandLayout->setSpacing(11);
    brandLayout->addWidget(new LogoMark(brand));
    auto *brandText = new QVBoxLayout();
    brandText->setSpacing(0);
    auto *title = new QLabel(QStringLiteral("Emby PC"), brand);
    title->setObjectName(QStringLiteral("BrandTitle"));
    auto *sub = new QLabel(QStringLiteral("自研客户端"), brand);
    sub->setObjectName(QStringLiteral("BrandSub"));
    brandText->addWidget(title);
    brandText->addWidget(sub);
    brandLayout->addLayout(brandText);
    brandLayout->addStretch(1);
    root->addWidget(brand);
    root->addSpacing(18);

    m_sectionLabel = new QLabel(QStringLiteral("浏览"), this);
    m_sectionLabel->setObjectName(QStringLiteral("SidebarSection"));
    root->addWidget(m_sectionLabel);
    root->addSpacing(2);

    m_group = new QButtonGroup(this);
    m_group->setExclusive(true);
    connect(m_group, &QButtonGroup::idClicked, this, [this](int id) {
        setCurrentIndex(id);
        emit sectionSelected(id);
    });

    m_navLayout = new QVBoxLayout();
    m_navLayout->setSpacing(4);
    m_navLayout->setContentsMargins(0, 0, 0, 0);
    root->addLayout(m_navLayout);

    root->addStretch(1);

    // 服务器卡片
    auto *server = new QWidget(this);
    server->setObjectName(QStringLiteral("ServerCard"));
    server->setAttribute(Qt::WA_StyledBackground, true);
    auto *serverLayout = new QHBoxLayout(server);
    serverLayout->setContentsMargins(12, 10, 12, 10);
    serverLayout->setSpacing(10);

    m_avatar = new QLabel(QStringLiteral("E"), server);
    m_avatar->setObjectName(QStringLiteral("AvatarLabel"));
    m_avatar->setAlignment(Qt::AlignCenter);
    m_avatar->setFixedSize(34, 34);
    serverLayout->addWidget(m_avatar);

    auto *serverText = new QVBoxLayout();
    serverText->setSpacing(2);
    m_serverName = new QLabel(QStringLiteral("Emby PC"), server);
    m_serverName->setObjectName(QStringLiteral("ServerName"));
    auto *statusRow = new QHBoxLayout();
    statusRow->setSpacing(5);
    auto *dot = new QLabel(server);
    dot->setObjectName(QStringLiteral("StatusDot"));
    dot->setFixedSize(7, 7);
    m_serverStatus = new QLabel(QStringLiteral("演示数据 · 本地 Mock"), server);
    m_serverStatus->setObjectName(QStringLiteral("ServerStatus"));
    statusRow->addWidget(dot);
    statusRow->addWidget(m_serverStatus);
    statusRow->addStretch(1);
    serverText->addWidget(m_serverName);
    serverText->addLayout(statusRow);
    serverLayout->addLayout(serverText, 1);

    root->addWidget(server);
    root->addSpacing(10);

    m_version = new QLabel(QStringLiteral("v0.2.0 · mock"), this);
    m_version->setObjectName(QStringLiteral("SidebarVersion"));
    m_version->setAlignment(Qt::AlignHCenter);
    root->addWidget(m_version);

    setServerInfo(QString(), QString(), true);
}

void Sidebar::rebuildNavigation()
{
    if (!m_navLayout)
        return;

    for (QPushButton *button : m_buttons) {
        m_group->removeButton(button);
        m_navLayout->removeWidget(button);
        button->deleteLater();
    }
    m_buttons.clear();

    // 0 = 首页
    m_buttons.append(
        makeNavButton(Icon::Home, QStringLiteral("首页"), QStringLiteral("SidebarHomeButton"), this));
    for (int i = 0; i < m_sections.size(); ++i) {
        const SidebarSection &section = m_sections.at(i);
        m_buttons.append(makeNavButton(iconForCollectionType(section.collectionType), section.name,
                                       QStringLiteral("SidebarLibraryButton"), this));
    }
    // 最后一个 = 设置
    m_buttons.append(makeNavButton(Icon::Settings, QStringLiteral("设置"),
                                   QStringLiteral("SidebarSettingsButton"), this));

    for (int i = 0; i < m_buttons.size(); ++i) {
        m_group->addButton(m_buttons.at(i), i);
        m_navLayout->addWidget(m_buttons.at(i));
    }

    const int maxIndex = m_buttons.size() - 1;
    if (m_currentIndex > maxIndex)
        m_currentIndex = 0;
    if (m_currentIndex >= 0 && m_currentIndex < m_buttons.size())
        m_buttons.at(m_currentIndex)->setChecked(true);
}

void Sidebar::setSections(const QList<SidebarSection> &sections)
{
    m_sections = sections;
    rebuildNavigation();
}

void Sidebar::setCurrentIndex(int index)
{
    if (index < 0 || index >= m_buttons.size())
        return;
    m_currentIndex = index;
    m_buttons.at(index)->setChecked(true);
}

void Sidebar::setServerInfo(const QString &serverName, const QString &userName, bool mock)
{
    if (m_serverName) {
        m_serverName->setText(serverName.isEmpty()
                                  ? (mock ? QStringLiteral("本地演示数据")
                                          : QStringLiteral("Emby 服务器"))
                                  : serverName);
    }
    if (m_serverStatus) {
        m_serverStatus->setText(mock ? QStringLiteral("演示数据 · 本地 Mock")
                                     : QStringLiteral("已连接 · 真实媒体库"));
    }
    if (m_avatar) {
        const QString initial = userName.isEmpty() ? QStringLiteral("E")
                                                   : userName.left(1).toUpper();
        m_avatar->setText(initial);
    }
    if (m_version)
        m_version->setText(mock ? QStringLiteral("v0.2.0 · mock")
                                : QStringLiteral("v0.2.0 · live"));
}

} // namespace ui
