// 独立 UI demo / 截图 / 自测入口。
// 正式构建时不会编译本文件（仅 EMBY_UI_BUILD_DEMO=ON 时构建）。
//
// 用法：
//   emby-ui-demo                       正常打开 UI（需要图形环境）
//   QT_QPA_PLATFORM=offscreen emby-ui-demo --selftest
//   QT_QPA_PLATFORM=offscreen emby-ui-demo --screenshot <输出目录>

#include "authutils.h"
#include "detailview.h"
#include "embyclient.h"
#include "embythumbnailcache.h"
#include "homeview.h"
#include "librarydata.h"
#include "libraryview.h"
#include "logindialog.h"
#include "mainwindow.h"
#include "playerpage.h"
#include "settingspage.h"
#include "sidebar.h"
#include "theme.h"
#include "titlebar.h"
#include "tokens.h"
#include "tokenstore.h"
#include "windowframe.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QBuffer>
#include <QHostAddress>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>
#include <QLineEdit>
#include <QLayout>
#include <QMenu>
#include <QMenuBar>
#include <QListView>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QStatusBar>
#include <QScrollBar>
#include <QMenuBar>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QUrl>
#include <QUrlQuery>
#include <QTimer>

#include <functional>

namespace {

void settle(QApplication &app, int milliseconds = 120)
{
    app.processEvents(QEventLoop::AllEvents, 50);
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
    app.processEvents(QEventLoop::AllEvents, 50);
}

bool waitFor(const std::function<bool()> &predicate, int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return predicate();
}

// 最小假 Emby 服务器：只实现登录流程用到的三个接口。
class FakeEmbyServer : public QTcpServer
{
public:
    explicit FakeEmbyServer(QObject *parent = nullptr)
        : QTcpServer(parent)
    {
    }

    bool start()
    {
        return listen(QHostAddress::LocalHost, 0);
    }

    QString baseUrl() const
    {
        return QStringLiteral("http://127.0.0.1:%1").arg(serverPort());
    }

    QStringList requestedTargets;
    QStringList imageRequests;
    QString lastImageAuthorization;
    QString lastImageApiKey;
    QHash<QString, int> imageAttempts;
    int activeImageRequests = 0;
    int maxActiveImageRequests = 0;

protected:
    void incomingConnection(qintptr socketDescriptor) override
    {
        auto *socket = new QTcpSocket(this);
        socket->setSocketDescriptor(socketDescriptor);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { onReadyRead(socket); });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }

private:
    static QByteArray fakeResumeJson()
    {
        return R"({"Id":"m1","Name":"假电影","Type":"Movie","ProductionYear":2023,"RunTimeTicks":54000000000,"ImageTags":{"Primary":"tagm1"},"BackdropImageTags":["bdm1"],"UserData":{"Played":false,"PlaybackPositionTicks":6000000000,"PlayedPercentage":11.1}})";
    }

    QHash<QTcpSocket *, QByteArray> m_buffers;

    void onReadyRead(QTcpSocket *socket)
    {
        QByteArray &buffer = m_buffers[socket];
        buffer += socket->readAll();
        for (;;) {
            const int headerEnd = buffer.indexOf("\r\n\r\n");
            if (headerEnd < 0)
                return;
            const QByteArray headerBlock = buffer.left(headerEnd);
            const QList<QByteArray> lines = headerBlock.split('\n');
            const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
            if (requestLine.size() < 2) {
                socket->disconnectFromHost();
                return;
            }
            const QString method = QString::fromLatin1(requestLine.at(0));
            const QString target = QString::fromLatin1(requestLine.at(1));
            QMap<QString, QString> headers;
            for (int i = 1; i < lines.size(); ++i) {
                const QByteArray line = lines.at(i).trimmed();
                const int colon = line.indexOf(':');
                if (colon <= 0)
                    continue;
                headers.insert(QString::fromLatin1(line.left(colon)).trimmed().toLower(),
                               QString::fromLatin1(line.mid(colon + 1)).trimmed());
            }
            const int contentLength =
                headers.value(QStringLiteral("content-length"), QStringLiteral("0")).toInt();
            const int total = headerEnd + 4 + contentLength;
            if (buffer.size() < total)
                return;
            const QByteArray body = buffer.mid(headerEnd + 4, contentLength);
            buffer.remove(0, total);
            requestedTargets.append(target);
            respond(socket, method, target, headers, body);
        }
    }

    static QByteArray tinyPng()
    {
        QImage image(2, 2, QImage::Format_RGB32);
        image.fill(QColor(0x3B, 0x82, 0xF6));
        QByteArray data;
        QBuffer buffer(&data);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
        return data;
    }

    static void writeResponse(QTcpSocket *socket, int status, const QByteArray &reason,
                              const QByteArray &contentType, const QByteArray &responseBody)
    {
        QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + " " + reason + "\r\n";
        response += "Content-Type: " + contentType + "\r\n";
        response += "Content-Length: " + QByteArray::number(responseBody.size()) + "\r\n";
        response += "Connection: close\r\n\r\n";
        response += responseBody;
        socket->write(response);
        socket->flush();
    }

    void respond(QTcpSocket *socket, const QString &method, const QString &target,
                 const QMap<QString, QString> &headers, const QByteArray &body)
    {
        int status = 200;
        QByteArray reason = "OK";
        QByteArray responseBody = "{}";
        QByteArray contentType = "application/json";
        const QString path = QUrl(QStringLiteral("http://localhost") + target).path();

        if (method == QLatin1String("GET") && path == QLatin1String("/System/Info/Public")) {
            responseBody = R"({"ServerName":"Fake Emby","Version":"4.9.0","ProductName":"Emby Server","Id":"srv-1","OperatingSystem":"Linux"})";
        } else if (method == QLatin1String("POST")
                   && path == QLatin1String("/Users/AuthenticateByName")) {
            const QJsonObject payload = QJsonDocument::fromJson(body).object();
            const QString username = payload.value(QStringLiteral("Username")).toString();
            const QString password = payload.value(QStringLiteral("Pw")).toString();
            if (username == QLatin1String("alice") && password == QLatin1String("secret")) {
                responseBody = R"({"AccessToken":"token-test-123","ServerId":"srv-1","User":{"Id":"u1","Name":"alice","ServerId":"srv-1"},"SessionInfo":{"Id":"session-1"}})";
            } else {
                status = 401;
                reason = "Unauthorized";
                responseBody = R"({"Message":"Invalid username or password"})";
            }
        } else {
            const QString authorization = headers.value(QStringLiteral("authorization"));
            const bool authorized =
                authorization.contains(QStringLiteral("Token=\"token-test-123\""));
            if (!authorized) {
                status = 401;
                reason = "Unauthorized";
                responseBody = R"({"Message":"Invalid token"})";
            } else if (method == QLatin1String("GET") && path == QLatin1String("/Users/u1/Views")) {
                responseBody =
                    R"({"Items":[{"Id":"v-movies","Name":"电影","Type":"CollectionFolder","CollectionType":"movies"},{"Id":"v-tv","Name":"电视节目","Type":"CollectionFolder","CollectionType":"tvshows"}],"TotalRecordCount":2})";
            } else if (method == QLatin1String("GET")
                       && path.startsWith(QLatin1String("/Items/"))
                       && path.contains(QLatin1String("/Images/"))) {
                imageRequests.append(target);
                lastImageAuthorization = authorization;
                lastImageApiKey =
                    QUrlQuery(QUrl(QStringLiteral("http://localhost") + target))
                        .queryItemValue(QStringLiteral("api_key"));
                const int attempt = ++imageAttempts[path];
                if (path.contains(QLatin1String("m-none"))) {
                    status = 404;
                    reason = "Not Found";
                    responseBody = R"({"Message":"no image"})";
                } else if (path.contains(QLatin1String("zretry")) && attempt == 1) {
                    // 第一次故意 500，第二次 200：验证 EmbyThumbnailCache 的 TTL 重试。
                    status = 500;
                    reason = "Internal Server Error";
                    responseBody = R"({"Message":"transient"})";
                } else {
                    contentType = "image/png";
                    responseBody = tinyPng();
                    if (path.contains(QLatin1String("slow"))
                        || path.contains(QLatin1String("hold"))) {
                        // 延迟响应，让自测能观察并发上限/FIFO 队列与传输超时。
                        const int delayMs =
                            path.contains(QLatin1String("hold")) ? 500 : 60;
                        ++activeImageRequests;
                        maxActiveImageRequests = qMax(maxActiveImageRequests, activeImageRequests);
                        QPointer<QTcpSocket> writes = socket;
                        const int finalStatus = status;
                        const QByteArray finalReason = reason;
                        const QByteArray finalContentType = contentType;
                        const QByteArray finalBody = responseBody;
                        QTimer::singleShot(delayMs, this, [this, writes, finalStatus, finalReason,
                                                          finalContentType, finalBody]() {
                            --activeImageRequests;
                            if (writes && writes->isValid())
                                writeResponse(writes.data(), finalStatus, finalReason,
                                              finalContentType, finalBody);
                        });
                        return;
                    }
                }
            } else if (method == QLatin1String("GET")
                       && path == QLatin1String("/Users/u1/Items/Resume")) {
                responseBody = QStringLiteral(
                                   R"({"Items":[%1],"TotalRecordCount":1})")
                                   .arg(fakeResumeJson())
                                   .toUtf8();
            } else if (method == QLatin1String("GET") && path == QLatin1String("/Users/u1/Items")) {
                const QUrlQuery query(QUrl(QStringLiteral("http://localhost") + target));
                const QString parentId = query.queryItemValue(QStringLiteral("ParentId"));
                const QString includeTypes = query.queryItemValue(QStringLiteral("IncludeItemTypes"));
                const QString searchTerm = query.queryItemValue(QStringLiteral("SearchTerm"));
                if (includeTypes.contains(QLatin1String("Season"))) {
                    responseBody = R"({"Items":[{"Id":"sea1","Name":"第 1 季","Type":"Season","IndexNumber":1,"ParentId":"s1"}],"TotalRecordCount":1})";
                } else if (includeTypes.contains(QLatin1String("Episode"))) {
                    responseBody =
                        R"({"Items":[{"Id":"e1","Name":"试播集","Type":"Episode","IndexNumber":1,"ParentIndexNumber":1,"SeriesId":"s1","SeriesPrimaryImageTag":"tagser","RunTimeTicks":27000000000,"UserData":{"Played":false,"PlaybackPositionTicks":0}},{"Id":"e2","Name":"第二集","Type":"Episode","IndexNumber":2,"ParentIndexNumber":1,"SeriesId":"s1","RunTimeTicks":27000000000,"UserData":{"Played":false,"PlaybackPositionTicks":0}}],"TotalRecordCount":2})";
                } else if (searchTerm == QLatin1String("不存在")) {
                    responseBody = R"({"Items":[],"TotalRecordCount":0})";
                } else if (parentId == QLatin1String("v-tv")) {
                    responseBody =
                        R"({"Items":[{"Id":"s1","Name":"假剧集","Type":"Series","ProductionYear":2024,"CommunityRating":8.5,"ImageTags":{"Primary":"tags1"},"BackdropImageTags":["bds1"],"UserData":{"Played":false,"PlaybackPositionTicks":0}}],"TotalRecordCount":1})";
                } else {
                    responseBody =
                        R"({"Items":[{"Id":"m1","Name":"假电影","Type":"Movie","ProductionYear":2023,"CommunityRating":7.8,"RunTimeTicks":54000000000,"ImageTags":{"Primary":"tagm1"},"BackdropImageTags":["bdm1"],"Genres":["科幻"],"Overview":"测试用简介","UserData":{"Played":false,"PlaybackPositionTicks":0}},{"Id":"m-thumb","Name":"只有缩略图的电影","Type":"Movie","ProductionYear":2022,"ImageTags":{"Thumb":"thumb-m"},"UserData":{"Played":false,"PlaybackPositionTicks":0}},{"Id":"b1","Name":"只有缩略图的合集","Type":"BoxSet","ProductionYear":2021,"ImageTags":{"Thumb":"thumb-b"},"UserData":{"Played":false,"PlaybackPositionTicks":0}},{"Id":"m-none","Name":"完全没有图的电影","Type":"Movie","ProductionYear":2020,"UserData":{"Played":false,"PlaybackPositionTicks":0}},{"Id":"s1","Name":"假剧集","Type":"Series","ProductionYear":2024,"CommunityRating":8.5,"ImageTags":{"Primary":"tags1"},"BackdropImageTags":["bds1"],"UserData":{"Played":false,"PlaybackPositionTicks":0}}],"TotalRecordCount":5})";
                }
            } else if (method == QLatin1String("GET")
                       && path.startsWith(QLatin1String("/Users/u1/Items/"))) {
                const QString itemId = path.mid(QStringLiteral("/Users/u1/Items/").size());
                if (itemId == QLatin1String("s1")) {
                    responseBody =
                        R"({"Id":"s1","Name":"假剧集","Type":"Series","ProductionYear":2024,"CommunityRating":8.5,"Overview":"剧集简介","ImageTags":{"Primary":"tags1"},"BackdropImageTags":["bds1"],"UserData":{"Played":false,"PlaybackPositionTicks":0}})";
                } else {
                    responseBody =
                        R"({"Id":"m1","Name":"假电影","Type":"Movie","ProductionYear":2023,"CommunityRating":7.8,"RunTimeTicks":54000000000,"Overview":"测试用简介","ImageTags":{"Primary":"tagm1"},"BackdropImageTags":["bdm1"],"Genres":["科幻"],"UserData":{"Played":false,"PlaybackPositionTicks":0}})";
                }
            } else {
                status = 404;
                reason = "Not Found";
                responseBody = R"({"Message":"unexpected request"})";
            }
        }

        writeResponse(socket, status, reason, contentType, responseBody);
    }
};

int runSelfTest(QApplication &app)
{
    QTextStream out(stdout);
    int failures = 0;
    auto check = [&out, &failures](bool ok, const QString &name) {
        out << (ok ? "PASS  " : "FAIL  ") << name << "\n";
        out.flush();
        if (!ok)
            ++failures;
    };

    ui::MainWindow window(ui::MainWindow::Options{false});
    window.resize(1280, 760);
    window.show();
    settle(app, 160);

    check(window.libraryIds().size() == 200, QStringLiteral("mock 媒体库包含 200 部"));
    check(window.currentStackIndex() == 0, QStringLiteral("默认页面为首页（stack=0）"));
    check(qApp->styleSheet().contains(QStringLiteral("QWidget#Sidebar")),
          QStringLiteral("深色 QSS 已加载"));

    // T43：qEmby token/QSS 基础。
    const QString qss = ui::Theme::styleSheet();
    const bool tokenOk = QString::fromLatin1(ui::tokens::ColorBg) == QStringLiteral("#0F172A")
                         && QString::fromLatin1(ui::tokens::ColorSurface) == QStringLiteral("#1E293B")
                         && ui::tokens::TopBarHeight == 56
                         && ui::tokens::SidebarDefaultWidth == 240
                         && ui::tokens::PageGutter == 32;
    check(tokenOk, QStringLiteral("T43 qEmby token 常量（bg/surface/topbar/sidebar/gutter）"));
    check(qss.contains(QStringLiteral("QScrollBar::handle:vertical"))
              && qss.contains(QStringLiteral(":focus"))
              && qss.contains(QStringLiteral("#0F172A"))
              && qss.contains(QStringLiteral("#1E293B")),
          QStringLiteral("T43 dark.qss 加载 token/焦点环/滚动条"));
    auto *topBar = window.findChild<QWidget *>(QStringLiteral("TopBar"));
    check(topBar && topBar->height() == ui::tokens::TopBarHeight,
          QStringLiteral("T43 顶栏高度 56"));

    // T27：自绘 TitleBar + WindowFrame 连接。
    auto *titleBar = window.findChild<ui::TitleBar *>(QStringLiteral("TitleBar"));
    auto *windowFrame = window.windowFrame();
    check(titleBar && titleBar->isVisible() && titleBar->height() == 36,
          QStringLiteral("TitleBar 创建并显示（高 36）"));
    check(titleBar && titleBar->geometry().top() == 0
              && titleBar->width() == window.width(),
          QStringLiteral("TitleBar 位于窗口顶部并铺满宽度"));
    check(titleBar && titleBar->minimizeButton() && titleBar->maximizeButton()
              && titleBar->closeButton()
              && titleBar->minimizeButton()->size() == QSize(46, 32)
              && titleBar->closeButton()->size() == QSize(46, 32),
          QStringLiteral("TitleBar 最小化/最大化/关闭按钮存在且为 46x32"));
    check(titleBar && titleBar->title() == window.windowTitle(),
          QStringLiteral("TitleBar 标题跟随窗口标题"));
    const QString originalWindowTitle = window.windowTitle();
    window.setWindowTitle(QStringLiteral("T27 标题跟随测试"));
    settle(app, 80);
    check(titleBar && titleBar->title() == QStringLiteral("T27 标题跟随测试"),
          QStringLiteral("窗口标题变化后 TitleBar 自动更新"));
    window.setWindowTitle(originalWindowTitle);
    check(windowFrame && windowFrame->titleBarHost() == titleBar
              && windowFrame->maximizeButtonRect() == titleBar->maximizeButtonRect(),
          QStringLiteral("WindowFrame 使用 TitleBar host 与最大化按钮矩形"));

    check(window.findChild<QMenuBar *>() == nullptr && titleBar
              && titleBar->geometry().top() == 0,
          QStringLiteral("T40：主窗口无 QMenuBar，TitleBar 置顶"));

    int titleDoubleClicks = 0;
    QObject::connect(titleBar, &ui::TitleBar::maximizeToggleRequested,
                     [&titleDoubleClicks]() { ++titleDoubleClicks; });
    QMouseEvent doubleClick(QEvent::MouseButtonDblClick, QPointF(200, 18),
                            titleBar->mapToGlobal(QPoint(200, 18)), Qt::LeftButton,
                            Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(titleBar, &doubleClick);
    settle(app, 80);
    check(titleDoubleClicks == 1, QStringLiteral("标题栏双击触发最大化切换"));
    window.showNormal();
    titleBar->setMaximized(false);

    check(window.isMockMode() && window.sectionNames()
              == QStringList({QStringLiteral("电影"), QStringLiteral("剧集")}),
          QStringLiteral("mock 模式侧边栏媒体库来自 2 个 section"));
    check(window.findChild<QWidget *>(QStringLiteral("ResumeSection")) != nullptr
              && window.findChildren<QWidget *>(QStringLiteral("ResumeCard")).size() > 0,
          QStringLiteral("首页继续观看横向列表已渲染 Resume 卡片"));

    auto *homePage = window.findChild<ui::HomeView *>(QStringLiteral("HomePage"));
    const int seeAllCount = homePage
                                ? homePage->findChildren<QPushButton *>(
                                      QStringLiteral("HomeSectionSeeAll")).size()
                                : 0;
    bool hasPosterThumb = false;
    if (homePage) {
        for (QLabel *thumb : homePage->findChildren<QLabel *>(
                                   QStringLiteral("HomeSectionThumbnail"))) {
            if (thumb->height() > thumb->width()) {
                hasPosterThumb = true;
                break;
            }
        }
    }
    check(seeAllCount > 0 && hasPosterThumb,
          QStringLiteral("T44 首页 section 含查看全部与 2:3 海报"));

    auto *home = window.findChild<ui::LibraryView *>(QStringLiteral("HomeLibrary"));
    check(home != nullptr && home->totalCount() == 200,
          QStringLiteral("首页 LibraryView 显示 200 个 item"));
    auto *grid = home ? home->findChild<QListView *>(QStringLiteral("PosterGrid")) : nullptr;
    check(grid != nullptr && grid->model()->rowCount() == 200,
          QStringLiteral("QListView 模型行数 = 200"));
    check(grid != nullptr && grid->gridSize().isValid(),
          QStringLiteral("海报网格 gridSize 已生效"));
    if (grid) {
        out << "DEBUG viewport=" << grid->viewport()->width() << "x" << grid->viewport()->height()
            << " grid=" << grid->gridSize().width() << "x" << grid->gridSize().height()
            << " item4=" << grid->visualRect(grid->model()->index(4, 0)).x() << "\n";
    }

    // 搜索过滤
    window.setSearchText(QStringLiteral("星际"));
    settle(app, 80);
    const int filtered = home ? home->itemCount() : -1;
    check(filtered > 0 && filtered < 200,
          QStringLiteral("搜索“星际”过滤结果 %1 条").arg(filtered));
    window.setSearchText(QString());
    settle(app, 50);
    check(home && home->itemCount() == 200, QStringLiteral("清空搜索后恢复 200 条"));

    // T40：首页「最近添加」标题点击跳转整页。
    auto *homeView = window.findChild<ui::HomeView *>(QStringLiteral("HomePage"));
    QLabel *recentTitle = nullptr;
    if (homeView) {
        for (QLabel *label : homeView->findChildren<QLabel *>(QStringLiteral("HomeSectionTitle"))) {
            if (label->property("sectionKind").toInt()
                == static_cast<int>(ui::HomeSection::Kind::Recent)) {
                recentTitle = label;
                break;
            }
        }
    }
    if (recentTitle) {
        QMouseEvent click(QEvent::MouseButtonRelease, QPointF(8, 8),
                          recentTitle->mapToGlobal(QPoint(8, 8)), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(recentTitle, &click);
        settle(app, 100);
    }
    check(recentTitle && window.currentStackIndex() == 1,
          QStringLiteral("最近添加标题点击跳转列表页"));
    window.navigateTo(ui::MainWindow::Page::Home);
    settle(app, 80);

    // T47：窗口缩放后首页预览列宽/滚动范围跟随，不整页重建。
    auto *sectionsScroll = homePage ? homePage->findChild<QScrollArea *>(
                                          QStringLiteral("HomeSectionsScroll"))
                                    : nullptr;
    int posterWidthAt1280 = 0;
    if (homePage) {
        for (QLabel *thumb : homePage->findChildren<QLabel *>(
                                   QStringLiteral("HomeSectionThumbnail"))) {
            if (thumb->height() > thumb->width()) {
                posterWidthAt1280 = thumb->width();
                break;
            }
        }
    }
    window.resize(960, 600);
    settle(app, 280);
    int posterWidthAt960 = 0;
    if (homePage) {
        for (QLabel *thumb : homePage->findChildren<QLabel *>(
                                   QStringLiteral("HomeSectionThumbnail"))) {
            if (thumb->height() > thumb->width()) {
                posterWidthAt960 = thumb->width();
                break;
            }
        }
    }
    check(sectionsScroll && posterWidthAt1280 > 0 && posterWidthAt960 > 0
              && posterWidthAt960 < posterWidthAt1280
              && sectionsScroll->verticalScrollBar()->maximum() > 0,
          QStringLiteral("T47 首页 resize 更新海报列宽/滚动范围"));
    window.resize(1280, 760);
    settle(app, 200);

    // 滚动 200 项到底部，测量绘制耗时
    if (grid) {
        QElapsedTimer timer;
        timer.start();
        int frames = 0;
        for (int i = 0; i <= 4; ++i) {
            const int maximum = grid->verticalScrollBar()->maximum();
            grid->verticalScrollBar()->setValue(maximum * i / 4);
            app.processEvents();
            window.grab();
            ++frames;
        }
        const qint64 elapsed = timer.elapsed();
        check(grid->verticalScrollBar()->value() >= 0,
              QStringLiteral("滚动 200 项到底部（%1 帧，平均 %2 ms/帧）")
                  .arg(frames)
                  .arg(elapsed / qMax(1, frames)));
    }

    // 导航：电影/剧集复用同一个 LibraryView
    window.navigateTo(ui::MainWindow::Page::Movies);
    check(window.currentStackIndex() == 1
              && window.currentSectionIndex() == 0,
          QStringLiteral("切换到电影媒体库（stack=1, section=0）"));
    auto *library = window.findChild<ui::LibraryView *>(QStringLiteral("LibraryContent"));
    check(library != nullptr && library->itemCount() > 0,
          QStringLiteral("电影媒体库加载了 %1 个 item").arg(library ? library->itemCount() : -1));
    if (library) {
        if (auto *mg = library->findChild<QListView *>(QStringLiteral("PosterGrid"))) {
            out << "DEBUG library viewport=" << mg->viewport()->width() << "x" << mg->viewport()->height()
                << " grid=" << mg->gridSize().width() << "x" << mg->gridSize().height()
                << " item1=" << mg->visualRect(mg->model()->index(1, 0)).x() << ","
                << mg->visualRect(mg->model()->index(1, 0)).y() << "\n";
        }
    }
    window.navigateTo(ui::MainWindow::Page::Series);
    check(window.currentStackIndex() == 1
              && window.currentSectionIndex() == 1,
          QStringLiteral("切换到剧集媒体库（stack=1, section=1）"));

    // 详情页
    const QString firstId = window.libraryIds().first();
    check(!firstId.isEmpty() && window.openDetail(firstId), QStringLiteral("打开详情页"));
    check(window.currentStackIndex() == 3, QStringLiteral("详情页 stack=3"));
    auto *detail = window.findChild<ui::DetailView *>();
    check(detail != nullptr && detail->currentItem().id == firstId,
          QStringLiteral("详情页数据与选中 item 一致"));
    const int episodeRows = detail ? detail->findChildren<QWidget *>(QStringLiteral("EpisodeRow")).size() : 0;
    check(episodeRows > 0, QStringLiteral("详情页季/集列表已生成（%1 集）").arg(episodeRows));
    auto *detailPoster = detail ? detail->findChild<QLabel *>(QStringLiteral("DetailPoster")) : nullptr;
    check(detailPoster && detailPoster->size() == QSize(188, 282),
          QStringLiteral("T44 详情海报 188x282"));

    // 播放页与信号
    const int sidebarWidthBeforePlayer = window.mainSplitter()->sizes().value(0);
    window.openPlayerDemo(firstId, 1, 2);
    check(window.currentStackIndex() == 4, QStringLiteral("播放页 stack=4"));
    auto *player = window.findChild<ui::PlayerPage *>();
    check(player != nullptr && player->isPlaying(), QStringLiteral("播放页处于播放状态（mock）"));

    // T16/T18：播放器模式 = 视频铺满客户区，顶层半透明浮层跟随主窗口并自动隐藏。
    auto *playerSidebar = window.findChild<QWidget *>(QStringLiteral("Sidebar"));
    auto *playerTopBar = window.findChild<QWidget *>(QStringLiteral("TopBar"));
    check(window.isPlayerMode(), QStringLiteral("播放页进入播放器模式"));
    check(playerSidebar && playerSidebar->isHidden(), QStringLiteral("播放器模式隐藏侧边栏"));
    check(playerTopBar && playerTopBar->isHidden(), QStringLiteral("播放器模式隐藏顶栏"));
    check(window.statusBar()->isHidden(), QStringLiteral("播放器模式隐藏状态栏"));
    check(player->layout() && player->layout()->count() == 1,
          QStringLiteral("播放页布局只保留视频区（浮层不占布局）"));
    check(player->usesTopLevelOverlay(), QStringLiteral("默认使用顶层 overlay 窗口"));
    check(player->topOverlay() && player->bottomOverlay()
              && player->topOverlay()->isWindow() && player->bottomOverlay()->isWindow()
              && player->topOverlay()->parentWidget() == player,
          QStringLiteral("顶部/底部浮层是 PlayerPage 的顶层子窗口"));
    check(player->topOverlay()->windowFlags().testFlag(Qt::Tool)
              && player->topOverlay()->windowFlags().testFlag(Qt::FramelessWindowHint)
              && !player->topOverlay()->testAttribute(Qt::WA_TranslucentBackground)
              && !player->topOverlay()->testAttribute(Qt::WA_NoSystemBackground)
              && player->topOverlay()->testAttribute(Qt::WA_ShowWithoutActivating),
          QStringLiteral("T55 顶层 overlay 不透明且不抢焦点"));
    const QImage topOverlayImage = player->topOverlay()->grab().toImage();
    check(topOverlayImage.pixelColor(2, 2).alpha() == 255,
          QStringLiteral("T55 顶层浮层背景像素完全不透明"));
    check(player->controlsVisible() && player->topOverlay()->isVisible()
              && player->bottomOverlay()->isVisible(),
          QStringLiteral("进入播放器模式浮层可见"));

    auto hostRect = [&window]() {
        return QRect(window.mapToGlobal(QPoint(0, 0)), window.size());
    };
    // T56：播放模式不再留 5px 黑边，overlay 直接铺满客户区；边缘缩放由
    // VideoHost/PlayerPage 6px 命中区负责。
    const int playerInset = 0;
    QRect host = hostRect();
    check(player->topOverlay()->geometry().topLeft()
              == host.topLeft() + QPoint(12 + playerInset, 10 + playerInset)
              && player->topOverlay()->width() <= host.width() - 24 - 2 * playerInset
              && player->bottomOverlay()->geometry().left() == host.left() + playerInset
              && player->bottomOverlay()->geometry().width() == host.width() - 2 * playerInset
              && player->bottomOverlay()->geometry().bottom() == host.bottom() - playerInset,
          QStringLiteral("浮层跟随主窗口客户区（顶栏左上自适应/控制条贴底铺满）"));
    check(player->size() == window.centralWidget()->size(),
          QStringLiteral("视频区铺满主窗口客户区（%1x%2）")
              .arg(player->width())
              .arg(player->height()));

    window.resize(960, 600);
    settle(app, 120);
    host = hostRect();
    check(player->bottomOverlay()->geometry().width() == host.width() - 2 * playerInset
              && player->bottomOverlay()->geometry().bottom() == host.bottom() - playerInset
              && player->topOverlay()->geometry().left() == host.left() + 12 + playerInset,
          QStringLiteral("窗口 resize 后顶层浮层跟随（960x600）"));

    window.move(window.pos() + QPoint(40, 30));
    settle(app, 120);
    host = hostRect();
    check(player->topOverlay()->geometry().left() == host.left() + 12 + playerInset
              && player->bottomOverlay()->geometry().bottom() == host.bottom() - playerInset,
          QStringLiteral("窗口 move 后顶层浮层跟随"));

    window.resize(1280, 760);
    settle(app, 120);

    // 自动隐藏：注入假光标位置，避免依赖 offscreen 的 QCursor::pos。
    player->setOverlayHideDelay(180);
    host = hostRect();
    player->setOverlayCursorPositionForTest(host.center());
    settle(app, 80);
    check(player->controlsVisible(), QStringLiteral("鼠标移动后浮层显示"));
    settle(app, 500);
    check(!player->controlsVisible(), QStringLiteral("静止 180ms（测试值）后浮层自动隐藏"));
    player->setOverlayCursorPositionForTest(host.center() + QPoint(30, 0));
    settle(app, 80);
    check(player->controlsVisible(), QStringLiteral("再次移动鼠标浮层恢复"));
    player->setPlaying(false);
    player->setOverlayCursorPositionForTest(host.center() + QPoint(30, 0));
    settle(app, 500);
    check(player->controlsVisible(), QStringLiteral("暂停时浮层保持显示"));
    player->setPlaying(true);
    player->setControlsVisible(false);
    check(!player->controlsVisible(), QStringLiteral("setControlsVisible(false) 隐藏浮层"));
    player->setControlsVisible(true);
    settle(app, 80);
    check(player->controlsVisible(), QStringLiteral("setControlsVisible(true) 显示浮层"));
    player->setOverlayAutoHide(false);
    player->setOverlayHideDelay(2500);
    player->setOverlayCursorPositionForTest(QPoint(), false);

    check(titleBar && titleBar->isHidden(), QStringLiteral("播放器模式隐藏 TitleBar"));
    window.setPlayerMode(false);
    check(!window.isPlayerMode() && playerSidebar && !playerSidebar->isHidden()
              && playerTopBar && !playerTopBar->isHidden()
              && !window.statusBar()->isHidden() && titleBar && titleBar->isVisible()
              && !player->controlsVisible(),
          QStringLiteral("退出播放器模式恢复 chrome/TitleBar 并隐藏顶层浮层"));
    window.setPlayerMode(true);
    player->setOverlayAutoHide(false);
    auto *subtitleButton = player->findChild<QPushButton *>(QStringLiteral("SubtitleButton"));
    auto *audioButton = player->findChild<QPushButton *>(QStringLiteral("AudioButton"));
    auto *statsButton = player->findChild<QPushButton *>(QStringLiteral("StatsButton"));
    check(subtitleButton && subtitleButton->text().isEmpty()
              && subtitleButton->toolTip().contains(QStringLiteral("中文")),
          QStringLiteral("T53 字幕纯图标+tooltip：%1").arg(subtitleButton ? subtitleButton->toolTip() : QString()));
    check(audioButton && audioButton->text().isEmpty() && !audioButton->toolTip().isEmpty()
              && statsButton && statsButton->text().isEmpty() && !statsButton->toolTip().isEmpty(),
          QStringLiteral("T53 音轨/统计纯图标+tooltip"));
    auto *playButton = player->findChild<QPushButton *>(QStringLiteral("PlayPauseButton"));
    auto *prevButton = player->findChild<QPushButton *>(QStringLiteral("PrevButton"));
    auto *nextButton = player->findChild<QPushButton *>(QStringLiteral("NextButton"));
    auto *seekSlider = player->findChild<QSlider *>(QStringLiteral("SeekSlider"));
    check(prevButton && playButton && nextButton && seekSlider
              && prevButton->geometry().center().x() < playButton->geometry().center().x()
              && playButton->geometry().center().x() < nextButton->geometry().center().x(),
          QStringLiteral("T45 播放/上下集居中，seek 滑杆存在"));

    // T32：FullscreenButton clicked -> fullscreenRequested -> MainWindow::toggleFullscreen。
    auto *fullscreenButton = player->findChild<QPushButton *>(QStringLiteral("FullscreenButton"));
    int fullscreenSignals = 0;
    QObject::connect(player, &ui::PlayerPage::fullscreenRequested,
                     [&fullscreenSignals]() { ++fullscreenSignals; });
    check(fullscreenButton && fullscreenButton->isEnabled(),
          QStringLiteral("FullscreenButton 存在且可点击"));
    if (fullscreenButton) {
        fullscreenButton->click();
        settle(app, 160);
        check(fullscreenSignals == 1 && window.isFullScreen(),
              QStringLiteral("FullscreenButton 点击链路生效（全屏）"));
        fullscreenButton->click();
        settle(app, 160);
        check(fullscreenSignals == 2 && !window.isFullScreen(),
              QStringLiteral("FullscreenButton 再次点击退出全屏"));
    }

    // T51：控制条扩展命中区、SeekSlider 点击即 seek、菜单关闭后 overlay 可点。
    if (auto *controlBar = player->findChild<QWidget *>(QStringLiteral("PlayerControlBar"))) {
        int extendedClicks = 0;
        QObject::connect(player, &ui::PlayerPage::playPauseRequested,
                         [&extendedClicks]() { ++extendedClicks; });
        const QPoint edge = playButton->mapTo(controlBar, QPoint(-3, playButton->height() / 2));
        QMouseEvent hit(QEvent::MouseButtonPress, edge, controlBar->mapToGlobal(edge),
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(controlBar, &hit);
        check(extendedClicks == 1, QStringLiteral("T51 控制条按钮边缘扩展命中"));
    }
    int seekClicks = 0;
    QObject::connect(player, &ui::PlayerPage::seekRequested,
                     [&seekClicks](qint64) { ++seekClicks; });
    if (seekSlider) {
        const QPoint seekPoint(seekSlider->width() * 3 / 4, seekSlider->height() / 2);
        QMouseEvent seek(QEvent::MouseButtonPress, seekPoint,
                         seekSlider->mapToGlobal(seekPoint), Qt::LeftButton, Qt::LeftButton,
                         Qt::NoModifier);
        QApplication::sendEvent(seekSlider, &seek);
    }
    check(seekClicks > 0, QStringLiteral("T51 进度条点击任意位置发出 seek"));
    if (auto *subtitleButton = player->findChild<QPushButton *>(QStringLiteral("SubtitleButton"))) {
        QTimer::singleShot(80, []() {
            if (QWidget *popup = QApplication::activePopupWidget())
                popup->close();
        });
        subtitleButton->click();
        settle(app, 120);
        auto *controlBar = player->findChild<QWidget *>(QStringLiteral("PlayerControlBar"));
        check(controlBar && controlBar->isVisible()
                  && !controlBar->testAttribute(Qt::WA_TransparentForMouseEvents),
              QStringLiteral("T51 菜单关闭后控制条恢复可点"));
    }
    // T55：顶层浮层必须是不透明窗口，四个角点击都能命中按钮。
    auto *t55ControlBar = player->findChild<QWidget *>(QStringLiteral("PlayerControlBar"));
    auto *t55TopBar = player->findChild<QWidget *>(QStringLiteral("PlayerTopBar"));
    check(t55ControlBar && !t55ControlBar->testAttribute(Qt::WA_TranslucentBackground)
              && !t55ControlBar->testAttribute(Qt::WA_NoSystemBackground)
              && t55TopBar && !t55TopBar->testAttribute(Qt::WA_TranslucentBackground)
              && !t55TopBar->testAttribute(Qt::WA_NoSystemBackground),
          QStringLiteral("T55 控制条/顶栏为不透明 overlay"));
    int cornerClicks = 0;
    QObject::connect(player, &ui::PlayerPage::playPauseRequested,
                     [&cornerClicks]() { ++cornerClicks; });
    if (playButton) {
        const QList<QPoint> corners = {QPoint(1, 1),
                                       QPoint(playButton->width() - 2, 1),
                                       QPoint(1, playButton->height() - 2),
                                       QPoint(playButton->width() - 2, playButton->height() - 2)};
        for (const QPoint &corner : corners) {
            QMouseEvent press(QEvent::MouseButtonPress, corner,
                              playButton->mapToGlobal(corner), Qt::LeftButton,
                              Qt::LeftButton, Qt::NoModifier);
            QMouseEvent release(QEvent::MouseButtonRelease, corner,
                                playButton->mapToGlobal(corner), Qt::LeftButton,
                                Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(playButton, &press);
            QApplication::sendEvent(playButton, &release);
        }
    }
    check(cornerClicks == 4, QStringLiteral("T55 按钮四角整块命中（%1/4）").arg(cornerClicks));

    // T32：无 episode 队列时禁用上一集/下一集并给出 tooltip。
    player->setEpisodeNavigationEnabled(false, false);
    check(prevButton && nextButton && !prevButton->isEnabled() && !nextButton->isEnabled()
              && prevButton->toolTip().contains(QStringLiteral("没有上一集"))
              && nextButton->toolTip().contains(QStringLiteral("没有下一集")),
          QStringLiteral("队列为空时上一集/下一集按钮禁用并有 tooltip"));
    player->setEpisodeNavigationEnabled(true, true);

    int playSignals = 0;
    int prevSignals = 0;
    int nextSignals = 0;
    QObject::connect(player, &ui::PlayerPage::playPauseRequested, [&playSignals]() { ++playSignals; });
    QObject::connect(player, &ui::PlayerPage::previousEpisodeRequested, [&prevSignals]() { ++prevSignals; });
    QObject::connect(player, &ui::PlayerPage::nextEpisodeRequested, [&nextSignals]() { ++nextSignals; });

    if (auto *play = player->findChild<QPushButton *>(QStringLiteral("PlayPauseButton")))
        play->click();
    if (auto *prev = player->findChild<QPushButton *>(QStringLiteral("PrevButton")))
        prev->click();
    if (auto *next = player->findChild<QPushButton *>(QStringLiteral("NextButton")))
        next->click();
    check(playSignals == 1, QStringLiteral("播放/暂停按钮发出 playPauseRequested"));
    check(prevSignals == 1, QStringLiteral("上一集按钮发出 previousEpisodeRequested"));
    check(nextSignals == 1, QStringLiteral("下一集按钮发出 nextEpisodeRequested"));

    // 视频宿主挂载接口
    auto *surface = new QWidget();
    surface->setObjectName(QStringLiteral("FakeVideoSurface"));
    player->attachVideoSurface(surface);
    check(player->videoSurfaceContainer()->findChild<QWidget *>(QStringLiteral("FakeVideoSurface"))
              != nullptr,
          QStringLiteral("attachVideoSurface 可替换 VideoHost 占位"));
    player->releaseVideoSurface();
    delete surface;

    // T57：播放器缩到最小 320x180 后返回，恢复 800x480 + 标题栏按钮可见。
    window.resize(320, 180);
    settle(app, 120);
    player->findChild<QPushButton *>(QStringLiteral("PlayerBackButton"))->click();
    settle(app, 260);
    const auto titleButtonRect = [&window](const QString &name) {
        QPushButton *button = window.titleBar()
                                  ? window.titleBar()->findChild<QPushButton *>(name)
                                  : nullptr;
        return button ? QRect(button->mapTo(&window, QPoint(0, 0)), button->size()) : QRect();
    };
    const QRect closeRect = titleButtonRect(QStringLiteral("TitleBarCloseButton"));
    const QRect minRect = titleButtonRect(QStringLiteral("TitleBarMinimizeButton"));
    const QRect maxRect = titleButtonRect(QStringLiteral("TitleBarMaximizeButton"));
    check(titleBar && titleBar->isVisible() && titleBar->width() == window.width()
              && window.width() >= 800 && window.height() >= 480
              && window.rect().contains(closeRect)
              && window.rect().contains(minRect)
              && window.rect().contains(maxRect),
          QStringLiteral("T57 小窗口退出播放模式恢复标题栏三按钮（close=%1,%2）")
              .arg(closeRect.x()).arg(closeRect.y()));
    window.resize(1280, 760);
    settle(app, 180);
    auto *restoredSplitter = window.mainSplitter();
    auto *restoredSidebar = window.findChild<QWidget *>(QStringLiteral("Sidebar"));
    const int restoredSidebarWidth = restoredSplitter->sizes().value(0);
    check(!window.isFullScreen() && !window.isMaximized()
              && restoredSidebar && restoredSidebar->isVisible()
              && restoredSidebarWidth >= 180 && restoredSidebarWidth <= 320
              && qAbs(restoredSidebarWidth - sidebarWidthBeforePlayer) <= 2,
          QStringLiteral("T47 全屏/播放模式退出恢复 splitter/sidebar 宽度（%1→%2）")
              .arg(sidebarWidthBeforePlayer)
              .arg(restoredSidebarWidth));
    check(window.currentStackIndex() == 3,
          QStringLiteral("播放页返回详情页（stack=3，实际 %1）").arg(window.currentStackIndex()));

    // T27：外部 showFullScreen（T17 PlayerController 路径）也隐藏 TitleBar。
    window.showFullScreen();
    settle(app, 120);
    check(titleBar && !titleBar->isVisible(), QStringLiteral("全屏状态隐藏 TitleBar"));
    window.showNormal();
    settle(app, 120);
    check(titleBar && titleBar->isVisible(), QStringLiteral("退出全屏恢复 TitleBar"));

    // 最小窗口尺寸下布局不崩
    window.resize(700, 400);
    settle(app, 120);
    check(window.minimumWidth() == 800 && window.minimumHeight() == 480
              && window.width() >= 800 && window.height() >= 480,
          QStringLiteral("最小尺寸 800x480（resize(700,400) 被约束）"));
    auto *videoPlaceholder = window.findChild<QWidget *>(QStringLiteral("VideoHostPlaceholder"));
    check(videoPlaceholder && videoPlaceholder->minimumWidth() <= 320
              && videoPlaceholder->minimumHeight() <= 180,
          QStringLiteral("视频区 placeholder 保持 320x180 下限"));
    check(window.findChild<QLabel *>(QStringLiteral("PlayerModeBadge")) == nullptr
              && window.findChild<QLabel *>(QStringLiteral("VideoHostTitle")) == nullptr
              && window.findChild<QLabel *>(QStringLiteral("VideoHostDesc")) == nullptr,
          QStringLiteral("T40：无演示徽标/VideoHost 说明文字"));

    // EMBY_PC_PLAYER_OVERLAY=0 兜底：浮层回到页面内。
    qputenv("EMBY_PC_PLAYER_OVERLAY", QByteArrayLiteral("0"));
    {
        ui::PlayerPage docked;
        docked.resize(800, 600);
        docked.show();
        settle(app, 80);
        check(!docked.usesTopLevelOverlay()
                  && docked.topOverlay()->parentWidget() == &docked
                  && !docked.topOverlay()->isWindow(),
              QStringLiteral("EMBY_PC_PLAYER_OVERLAY=0 回退页面内浮层"));
        docked.setPlayerModeActive(true);
        settle(app, 80);
        check(docked.controlsVisible()
                  && docked.topOverlay()->geometry().top() == 0
                  && docked.bottomOverlay()->geometry().bottom() + 1 == docked.height(),
              QStringLiteral("页面内兜底浮层贴住播放页顶部/底部"));
        docked.setPlayerModeActive(false);
    }
    qunsetenv("EMBY_PC_PLAYER_OVERLAY");

    out << (failures == 0 ? "ALL PASS" : QStringLiteral("FAILURES: %1").arg(failures)) << "\n";
    out.flush();
    return failures == 0 ? 0 : 1;
}

int runWindowStateTests(QApplication &app)
{
    QTextStream out(stdout);
    int failures = 0;
    auto check = [&out, &failures](bool ok, const QString &name) {
        out << (ok ? "PASS  " : "FAIL  ") << name << "\n";
        out.flush();
        if (!ok)
            ++failures;
    };

    QTemporaryDir settingsDir;
    check(settingsDir.isValid(), QStringLiteral("T27 创建窗口状态临时 QSettings"));
    const QString oldOrganization = QCoreApplication::organizationName();
    const QString oldApplication = QCoreApplication::applicationName();
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QCoreApplication::setOrganizationName(QStringLiteral("emby-pc-test"));
    QCoreApplication::setApplicationName(QStringLiteral("ui-window-state-test"));

    QRect savedNormalGeometry;
    {
        ui::MainWindow window(ui::MainWindow::Options{false});
        window.resize(800, 600);
        // offscreen 屏宽可能小于窗口最小宽度；用 x=0 验证保存/恢复，clamp 另测。
        window.move(0, 60);
        window.show();
        settle(app, 120);
        window.close();
        settle(app, 80);
        QSettings settings;
        savedNormalGeometry = settings.value(QStringLiteral("window/normalGeometry")).toRect();
    }
    {
        ui::MainWindow window(ui::MainWindow::Options{false});
        settle(app, 120);
        check(savedNormalGeometry.isValid() && window.geometry() == savedNormalGeometry,
              QStringLiteral("窗口 normalGeometry 保存/恢复（saved=%1,%2 %3x%4 actual=%5,%6 %7x%8）")
                  .arg(savedNormalGeometry.x()).arg(savedNormalGeometry.y())
                  .arg(savedNormalGeometry.width()).arg(savedNormalGeometry.height())
                  .arg(window.x()).arg(window.y())
                  .arg(window.width()).arg(window.height()));
        check(!window.isMaximized(), QStringLiteral("窗口非最大化状态恢复"));
        window.close();
    }

    {
        QSettings settings;
        settings.setValue(QStringLiteral("window/normalGeometry"),
                          QRect(10000, 10000, 700, 500));
        settings.setValue(QStringLiteral("window/maximized"), false);
    }
    {
        ui::MainWindow window(ui::MainWindow::Options{false});
        settle(app, 120);
        QScreen *screen = QGuiApplication::primaryScreen();
        const QRect available = screen ? screen->availableGeometry() : QRect();
        const QRect geometry = window.geometry();
        check(!available.isEmpty() && available.intersects(geometry)
                  && geometry.left() >= available.left()
                  && geometry.top() >= available.top(),
              QStringLiteral("窗口位置 clamp 到当前屏幕 availableGeometry（左上可见）"));
        window.close();
    }

    {
        QSettings settings;
        settings.setValue(QStringLiteral("window/normalGeometry"),
                          QRect(100, 80, 700, 500));
        settings.setValue(QStringLiteral("window/maximized"), true);
    }
    {
        ui::MainWindow window(ui::MainWindow::Options{false});
        settle(app, 120);
        check(window.isMaximized(), QStringLiteral("窗口最大化状态恢复"));
        window.close();
    }

    QCoreApplication::setOrganizationName(oldOrganization);
    QCoreApplication::setApplicationName(oldApplication);
    return failures;
}

int runSettingsTests(QApplication &app)
{
    QTextStream out(stdout);
    int failures = 0;
    auto check = [&out, &failures](bool ok, const QString &name) {
        out << (ok ? "PASS  " : "FAIL  ") << name << "\n";
        out.flush();
        if (!ok)
            ++failures;
    };

    QTemporaryDir settingsDir;
    check(settingsDir.isValid(), QStringLiteral("T28 创建设置临时 QSettings"));
    const QString oldOrganization = QCoreApplication::organizationName();
    const QString oldApplication = QCoreApplication::applicationName();
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QCoreApplication::setOrganizationName(QStringLiteral("emby-pc-test"));
    QCoreApplication::setApplicationName(QStringLiteral("ui-settings-test"));

    {
        ui::SettingsPage page;
        page.show();
        settle(app, 80);
        auto *subtitle = page.findChild<QComboBox *>(QStringLiteral("SettingsSubtitleLanguageCombo"));
        auto *autoPlay = page.findChild<QCheckBox *>(QStringLiteral("SettingsAutoPlayNextCheck"));
        auto *remember = page.findChild<QCheckBox *>(QStringLiteral("SettingsRememberVolumeCheck"));
        auto *delay = page.findChild<QComboBox *>(QStringLiteral("SettingsAutoHideDelayCombo"));
        auto *cacheLimit = page.findChild<QSpinBox *>(QStringLiteral("SettingsCacheLimitSpin"));
        auto *hardware = page.findChild<QLabel *>(QStringLiteral("SettingsHardwareDecoderValue"));
        check(subtitle && autoPlay && remember && delay && cacheLimit && hardware,
              QStringLiteral("设置页播放/存储控件存在"));
        check(page.subtitleLanguage() == QStringLiteral("chi,zho,zh")
                  && page.autoPlayNext() && page.rememberVolume()
                  && page.autoHideDelayMs() == 2500 && page.cacheImageLimitMB() == 1024,
              QStringLiteral("设置页默认值"));

        int changedCount = 0;
        QObject::connect(&page, &ui::SettingsPage::settingsChanged,
                         [&changedCount]() { ++changedCount; });
        subtitle->setCurrentIndex(subtitle->findData(QStringLiteral("zho,chi,zh")));
        autoPlay->setChecked(false);
        remember->setChecked(false);
        delay->setCurrentIndex(delay->findData(1500));
        cacheLimit->setValue(512);
        settle(app, 80);
        check(changedCount >= 5, QStringLiteral("设置页修改发出 settingsChanged"));

        QSettings settings;
        check(settings.value(QStringLiteral("player/subtitleLanguage")).toString()
                  == QStringLiteral("zho,chi,zh")
                  && !settings.value(QStringLiteral("player/autoPlayNext")).toBool()
                  && !settings.value(QStringLiteral("player/rememberVolume")).toBool()
                  && settings.value(QStringLiteral("player/autoHideDelayMs")).toInt() == 1500
                  && settings.value(QStringLiteral("cache/imageLimitMB")).toInt() == 512,
              QStringLiteral("设置页写入约定 QSettings 键"));

        int clearCount = 0;
        int exportCount = 0;
        QObject::connect(&page, &ui::SettingsPage::clearImageCacheRequested,
                         [&clearCount]() { ++clearCount; });
        QObject::connect(&page, &ui::SettingsPage::exportDiagnosticsRequested,
                         [&exportCount]() { ++exportCount; });
        if (auto *clear = page.findChild<QPushButton *>(QStringLiteral("SettingsClearCacheButton")))
            clear->click();
        if (auto *exportButton = page.findChild<QPushButton *>(QStringLiteral("SettingsExportDiagnosticsButton")))
            exportButton->click();
        check(clearCount == 1 && exportCount == 1,
              QStringLiteral("设置页缓存清理/诊断导出按钮发出信号"));

        page.setHardwareDecoderText(QStringLiteral("d3d11va"));
        page.setLibmpvVersionText(QStringLiteral("mpv 0.38"));
        check(hardware->text() == QStringLiteral("d3d11va")
                  && page.findChild<QLabel *>(QStringLiteral("SettingsLibmpvVersionValue"))->text()
                         == QStringLiteral("mpv 0.38"),
              QStringLiteral("设置页回填硬件解码/libmpv 版本"));
        const auto aboutValues = {
            page.findChild<QLabel *>(QStringLiteral("SettingsAppVersionValue")),
            page.findChild<QLabel *>(QStringLiteral("SettingsQtVersionValue")),
            page.findChild<QLabel *>(QStringLiteral("SettingsBuildTimeValue")),
            page.findChild<QLabel *>(QStringLiteral("SettingsLibmpvVersionValue"))};
        bool aboutNonEmpty = true;
        for (QLabel *label : aboutValues)
            aboutNonEmpty = aboutNonEmpty && label && !label->text().isEmpty();
        check(aboutNonEmpty, QStringLiteral("关于页版本/Qt/构建/libmpv 字段非空"));
        check(page.findChild<QLabel *>(QStringLiteral("SettingsCacheUsageValue"))->text().size() > 0,
              QStringLiteral("存储页显示缓存占用"));
    }

    {
        ui::SettingsPage restored;
        check(restored.subtitleLanguage() == QStringLiteral("zho,chi,zh")
                  && !restored.autoPlayNext() && !restored.rememberVolume()
                  && restored.autoHideDelayMs() == 1500 && restored.cacheImageLimitMB() == 512,
              QStringLiteral("重新构造 SettingsPage 恢复设置值"));
    }

    QCoreApplication::setOrganizationName(oldOrganization);
    QCoreApplication::setApplicationName(oldApplication);
    return failures;
}

int runLibraryLayoutTests(QApplication &app)
{
    QTextStream out(stdout);
    int failures = 0;
    auto check = [&out, &failures](bool ok, const QString &name) {
        out << (ok ? "PASS  " : "FAIL  ") << name << "\n";
        out.flush();
        if (!ok)
            ++failures;
    };

    QTemporaryDir settingsDir;
    check(settingsDir.isValid(), QStringLiteral("T38 创建布局临时 QSettings"));
    const QString oldOrganization = QCoreApplication::organizationName();
    const QString oldApplication = QCoreApplication::applicationName();
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QCoreApplication::setOrganizationName(QStringLiteral("emby-pc-test"));
    QCoreApplication::setApplicationName(QStringLiteral("ui-layout-test"));

    // 1) HomeView 多 section 顺序/空行隐藏。
    ui::EmbyThumbnailCache cache;
    ui::HomeView home(&cache);
    home.resize(1000, 700);
    home.show();
    settle(app, 100);
    QList<ui::HomeSection> sections;
    ui::HomeSection resume;
    resume.kind = ui::HomeSection::Kind::Resume;
    resume.title = QStringLiteral("继续观看");
    ui::MediaItem resumeItem;
    resumeItem.id = QStringLiteral("resume-1");
    resumeItem.title = QStringLiteral("继续观看 1");
    resume.items.append(resumeItem);
    sections.append(resume);
    ui::HomeSection recent;
    recent.kind = ui::HomeSection::Kind::Recent;
    recent.title = QStringLiteral("最近添加");
    ui::MediaItem recentItem;
    recentItem.id = QStringLiteral("recent-1");
    recentItem.title = QStringLiteral("最近添加 1");
    recent.items.append(recentItem);
    sections.append(recent);
    ui::HomeSection emptyLibrary;
    emptyLibrary.kind = ui::HomeSection::Kind::Library;
    emptyLibrary.libraryId = QStringLiteral("lib-empty");
    emptyLibrary.title = QStringLiteral("空库");
    sections.append(emptyLibrary);
    ui::HomeSection library;
    library.kind = ui::HomeSection::Kind::Library;
    library.libraryId = QStringLiteral("lib-movies");
    library.title = QStringLiteral("电影最新");
    ui::MediaItem libraryItem;
    libraryItem.id = QStringLiteral("movie-1");
    libraryItem.title = QStringLiteral("电影 1");
    library.items.append(libraryItem);
    sections.append(library);
    home.setHomeSections(sections);
    settle(app, 120);
    check(home.sectionCount() == 3 && home.visibleSectionCount() == 3
              && home.sectionTitles()
                     == QStringList({QStringLiteral("继续观看"), QStringLiteral("最近添加"),
                                     QStringLiteral("电影最新")}),
          QStringLiteral("HomeView 按顺序渲染 3 个非空 section，空库隐藏"));
    const int homeRowCount = home.findChildren<QWidget *>(QStringLiteral("HomeSectionRow")).size();
    check(homeRowCount == 3,
          QStringLiteral("HomeView 生成 3 个 HomeSectionRow（实际 %1）").arg(homeRowCount));

    // T39：5 section × 18 张卡片的懒加载/构建耗时（offscreen）。
    QList<ui::HomeSection> manySections;
    for (int s = 0; s < 5; ++s) {
        ui::HomeSection section;
        section.kind = ui::HomeSection::Kind::Library;
        section.libraryId = QStringLiteral("lib-%1").arg(s);
        section.title = QStringLiteral("库 %1").arg(s);
        for (int i = 0; i < 18; ++i) {
            ui::MediaItem item;
            item.id = QStringLiteral("perf-%1-%2").arg(s).arg(i);
            item.title = item.id;
            section.items.append(item);
        }
        manySections.append(section);
    }
    QElapsedTimer lazyTimer;
    lazyTimer.start();
    home.setHomeSections(manySections);
    settle(app, 160);
    const int lazyElapsed = int(lazyTimer.elapsed());
    const int builtSections = home.builtSectionCount();
    const int totalSections = home.sectionCount();
    check(builtSections > 0 && builtSections < totalSections,
          QStringLiteral("懒加载只构建首屏 section（built=%1/%2）")
              .arg(builtSections).arg(totalSections));
    check(lazyElapsed < 1500,
          QStringLiteral("5×18 section 首屏构建 %1ms < 1500ms").arg(lazyElapsed));
    home.resize(760, 480);
    settle(app, 100);
    check(home.builtSectionCount() == builtSections,
          QStringLiteral("窗口缩放不重建已构建 section"));
    auto *sectionsScroll = home.findChild<QScrollArea *>(QStringLiteral("HomeSectionsScroll"));
    if (sectionsScroll) {
        sectionsScroll->verticalScrollBar()->setValue(
            sectionsScroll->verticalScrollBar()->maximum());
        settle(app, 180);
    }
    check(sectionsScroll && home.builtSectionCount() > builtSections
              && home.builtSectionCount() <= totalSections,
          QStringLiteral("滚动后增量构建剩余 section（built=%1/%2）")
              .arg(home.builtSectionCount()).arg(totalSections));

    // T40：非继续观看行只做固定预览网格，不横向滚动；标题点击发 sectionActivated。
    const auto previewRows = home.findChildren<QWidget *>(QStringLiteral("HomeSectionPreview"));
    int previewCardCount = 0;
    if (!previewRows.isEmpty())
        previewCardCount = previewRows.first()->findChildren<QWidget *>(
            QStringLiteral("HomeSectionCard")).size();
    check(!previewRows.isEmpty() && previewCardCount >= 6 && previewCardCount <= 8,
          QStringLiteral("最近添加/媒体库行固定 6–8 张预览（实际 %1）").arg(previewCardCount));
    check(home.findChildren<QScrollArea *>(QStringLiteral("HomeSectionScroll")).isEmpty(),
          QStringLiteral("非继续观看 section 无横向滚动条"));
    int activatedKind = -1;
    QString activatedLibraryId;
    QObject::connect(&home, &ui::HomeView::sectionActivated,
                     [&activatedKind, &activatedLibraryId](int kind, const QString &id) {
                         activatedKind = kind;
                         activatedLibraryId = id;
                     });
    const auto sectionTitles = home.findChildren<QLabel *>(QStringLiteral("HomeSectionTitle"));
    if (!sectionTitles.isEmpty()) {
        QLabel *title = sectionTitles.first();
        QMouseEvent click(QEvent::MouseButtonRelease, QPointF(8, 8),
                          title->mapToGlobal(QPoint(8, 8)), Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
        QApplication::sendEvent(title, &click);
        settle(app, 40);
    }
    check(activatedKind == static_cast<int>(ui::HomeSection::Kind::Library)
              && activatedLibraryId == QStringLiteral("lib-0"),
          QStringLiteral("点击 section 标题发出跳转信号"));
    home.close();

    // 2) 设置页媒体库显示/排序写 QSettings。
    ui::SettingsPage page;
    QList<ui::LibrarySection> libraries;
    ui::LibrarySection movies;
    movies.id = QStringLiteral("lib-movies");
    movies.name = QStringLiteral("电影");
    movies.collectionType = QStringLiteral("movies");
    ui::LibrarySection series;
    series.id = QStringLiteral("lib-series");
    series.name = QStringLiteral("剧集");
    series.collectionType = QStringLiteral("tvshows");
    libraries << movies << series;
    page.show();
    settle(app, 80);
    auto navButtons = page.findChildren<QPushButton *>(QStringLiteral("SettingsNavButton"));
    bool playedCategory = false;
    for (auto *nav : navButtons) {
        if (nav->text() == QStringLiteral("播放")) {
            nav->click();
            playedCategory = true;
            break;
        }
    }
    settle(app, 60);
    auto *playbackCombo = page.findChild<QComboBox *>(
        QStringLiteral("SettingsSubtitleLanguageCombo"));
    check(navButtons.size() == 5 && playedCategory && playbackCombo
              && playbackCombo->isVisible(),
          QStringLiteral("T45 设置页左侧 5 个分类+切换播放分组"));
    for (auto *nav : page.findChildren<QPushButton *>(QStringLiteral("SettingsNavButton"))) {
        if (nav->text() == QStringLiteral("媒体库")) {
            nav->click();
            break;
        }
    }
    page.setLibrarySections(libraries);
    settle(app, 80);
    auto rows = page.findChildren<QWidget *>(QStringLiteral("SettingsLibraryRow"));
    check(rows.size() == 2 && page.findChild<QWidget *>(QStringLiteral("SettingsLibraryGroup"))
                                  ->isVisible(),
          QStringLiteral("设置页媒体库分区显示 2 行"));
    int librarySignals = 0;
    QObject::connect(&page, &ui::SettingsPage::libraryDisplaySettingsChanged,
                     [&librarySignals]() { ++librarySignals; });
    if (!rows.isEmpty()) {
        auto *sidebarCheck = rows.first()->findChild<QCheckBox *>(
            QStringLiteral("SettingsSidebarVisibleCheck"));
        auto *homeCheck = rows.first()->findChild<QCheckBox *>(
            QStringLiteral("SettingsHomeVisibleCheck"));
        if (sidebarCheck)
            sidebarCheck->setChecked(false);
        settle(app, 80);
        QSettings settings;
        check(sidebarCheck && homeCheck && !sidebarCheck->isChecked()
                  && !homeCheck->isEnabled()
                  && settings.value(QStringLiteral("sidebar/hiddenLibraries")).toStringList()
                         .contains(QStringLiteral("lib-movies"))
                  && librarySignals >= 1,
              QStringLiteral("设置页取消侧边栏显示写入 QSettings 并禁用首页显示"));
    }
    auto *down = rows.isEmpty()
                     ? nullptr
                     : rows.first()->findChild<QPushButton *>(QStringLiteral("SettingsLibraryDownButton"));
    if (down)
        down->click();
    settle(app, 80);
    QSettings ordered;
    check(ordered.value(QStringLiteral("home/sectionOrder")).toStringList().indexOf(
              QStringLiteral("lib:lib-series"))
              < ordered.value(QStringLiteral("home/sectionOrder")).toStringList().indexOf(
                    QStringLiteral("lib:lib-movies")),
          QStringLiteral("设置页下移调整 home/sectionOrder"));

    // 3) MainWindow QSplitter 宽度恢复与侧边栏数量联动。
    QSettings settingsReset;
    settingsReset.remove(QStringLiteral("sidebar/hiddenLibraries"));
    settingsReset.remove(QStringLiteral("home/hiddenLibraries"));
    settingsReset.remove(QStringLiteral("home/sectionOrder"));
    settingsReset.setValue(QStringLiteral("ui/sidebarWidth"), 300);
    ui::MainWindow window(ui::MainWindow::Options{false});
    window.resize(1200, 700);
    window.show();
    settle(app, 150);
    auto *splitter = window.mainSplitter();
    auto *sidebar = window.findChild<QWidget *>(QStringLiteral("Sidebar"));
    check(splitter && splitter->objectName() == QStringLiteral("MainSplitter")
              && sidebar && sidebar->minimumWidth() == 180 && sidebar->maximumWidth() == 320
              && qAbs(sidebar->width() - 300) <= 8,
          QStringLiteral("MainSplitter 恢复 ui/sidebarWidth=300，范围 180-320"));
    auto *windowSettings = window.settingsPage();
    if (windowSettings) {
        windowSettings->setLibrarySections(window.librarySections());
        settle(app, 80);
        const auto settingsRows = window.findChildren<QWidget *>(
            QStringLiteral("SettingsLibraryRow"));
        auto *sidebarCheck = settingsRows.isEmpty()
                                 ? nullptr
                                 : settingsRows.last()->findChild<QCheckBox *>(
                                       QStringLiteral("SettingsSidebarVisibleCheck"));
        auto *realSidebar = window.findChild<ui::Sidebar *>();
        if (sidebarCheck && realSidebar) {
            const int before = realSidebar->sections().size();
            sidebarCheck->setChecked(false);
            settle(app, 120);
            const int after = realSidebar->sections().size();
            check(after == before - 1,
                  QStringLiteral("设置页隐藏媒体库后侧边栏数量减少（%1→%2 checked=%3）")
                      .arg(before).arg(after).arg(sidebarCheck->isChecked()));
        }
    }
    if (splitter) {
        splitter->setSizes({250, qMax(1, splitter->width() - 250)});
        settle(app, 100);
    }
    window.close();

    ui::MainWindow restoredWindow(ui::MainWindow::Options{false});
    restoredWindow.resize(1200, 700);
    restoredWindow.show();
    settle(app, 150);
    auto *restoredSidebar = restoredWindow.findChild<QWidget *>(QStringLiteral("Sidebar"));
    check(restoredSidebar && qAbs(restoredSidebar->width() - 250) <= 8,
          QStringLiteral("ui/sidebarWidth 关闭保存后新窗口恢复（实际 %1）")
              .arg(restoredSidebar ? restoredSidebar->width() : -1));
    restoredWindow.close();

    QCoreApplication::setOrganizationName(oldOrganization);
    QCoreApplication::setApplicationName(oldApplication);
    return failures;
}

int runAuthTests(QApplication &app)
{
    Q_UNUSED(app);
    QTextStream out(stdout);
    int failures = 0;
    auto check = [&out, &failures](bool ok, const QString &name) {
        out << (ok ? "PASS  " : "FAIL  ") << name << "\n";
        out.flush();
        if (!ok)
            ++failures;
    };

    // 自测默认关闭本地图片缓存，避免上一次 selftest 的磁盘文件影响网络请求计数；
    // 单独的磁盘缓存测试会显式开启并使用临时目录。
    qputenv("EMBY_PC_IMAGE_DISK_CACHE", QByteArrayLiteral("0"));

    // 隔离 QSettings，避免写入真实用户配置。
    QTemporaryDir tempDir;
    check(tempDir.isValid(), QStringLiteral("创建临时 QSettings 目录"));
    const QString oldOrganization = QCoreApplication::organizationName();
    const QString oldApplication = QCoreApplication::applicationName();
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, tempDir.path());
    QCoreApplication::setOrganizationName(QStringLiteral("emby-pc-test"));
    QCoreApplication::setApplicationName(QStringLiteral("ui-auth-test"));

    FakeEmbyServer server;
    check(server.start(), QStringLiteral("假 Emby 服务器启动（127.0.0.1）"));

    // 错误翻译
    emby::ApiError sslError;
    sslError.networkError = QNetworkReply::SslHandshakeFailedError;
    check(ui::friendlyApiError(sslError).contains(QStringLiteral("证书")),
          QStringLiteral("SSL 证书错误有中文提示"));
    emby::ApiError timeoutError;
    timeoutError.networkError = QNetworkReply::TimeoutError;
    check(ui::friendlyApiError(timeoutError).contains(QStringLiteral("超时")),
          QStringLiteral("超时有中文提示"));
    emby::ApiError httpError;
    httpError.httpStatus = 401;
    check(ui::friendlyApiError(httpError).contains(QStringLiteral("401")),
          QStringLiteral("401 有中文提示"));

    QString urlError;
    check(ui::normalizeServerUrl(QStringLiteral("192.168.1.10:8096"), &urlError)
              == QStringLiteral("http://192.168.1.10:8096"),
          QStringLiteral("服务器地址自动补 http://"));
    check(ui::normalizeServerUrl(QStringLiteral("not a url"), &urlError).isEmpty()
              && !urlError.isEmpty(),
          QStringLiteral("非法服务器地址被拒绝"));

    // TokenStore 持久化
    ui::QSettingsTokenStore store;
    store.clear();
    emby::ServerProfile profile;
    profile.url = server.baseUrl();
    profile.name = QStringLiteral("Fake Emby");
    profile.serverId = QStringLiteral("srv-1");
    profile.userId = QStringLiteral("u1");
    profile.userName = QStringLiteral("alice");
    profile.accessToken = QStringLiteral("token-test-123");
    profile.deviceId = QStringLiteral("dev-test");
    store.saveProfile(profile);
    const emby::ServerProfile loaded = store.loadProfile();
    check(loaded.accessToken == profile.accessToken && loaded.userId == QStringLiteral("u1")
              && loaded.deviceId == QStringLiteral("dev-test"),
          QStringLiteral("TokenStore 保存/读取 ServerProfile"));
    check(store.backendName().contains(QStringLiteral("明文")),
          QStringLiteral("TokenStore 明确标注 QSettings 临时明文"));

    // LoginDialog：测试连接 + 登录成功
    emby::EmbyClient client;
    client.setRequestTimeout(4000);
    ui::LoginDialog login(&client);
    login.setInitialValues(server.baseUrl(), QStringLiteral("alice"), QString());
    auto *password = login.findChild<QLineEdit *>(QStringLiteral("PasswordEdit"));
    auto *testButton = login.findChild<QPushButton *>(QStringLiteral("TestConnectionButton"));
    auto *loginButton = login.findChild<QPushButton *>(QStringLiteral("LoginSubmitButton"));
    auto *statusLabel = login.findChild<QLabel *>(QStringLiteral("LoginStatus"));
    check(password && testButton && loginButton && statusLabel,
          QStringLiteral("LoginDialog 控件齐全"));
    check(statusLabel && statusLabel->isHidden(),
          QStringLiteral("T44 登录空状态框不渲染"));

    int loginOkCount = 0;
    emby::ServerProfile loginProfile;
    QObject::connect(&login, &ui::LoginDialog::loginSucceeded,
                     [&loginOkCount, &loginProfile](const emby::ServerProfile &p) {
                         ++loginOkCount;
                         loginProfile = p;
                     });

    if (testButton)
        testButton->click();
    const bool testOk = waitFor([statusLabel]() {
        return statusLabel && statusLabel->text().contains(QStringLiteral("连接成功"));
    });
    check(testOk, QStringLiteral("“测试连接”显示连接成功"));

    if (password)
        password->setText(QStringLiteral("secret"));
    if (loginButton)
        loginButton->click();
    check(waitFor([&loginOkCount]() { return loginOkCount == 1; }),
          QStringLiteral("LoginDialog 登录成功并发出 loginSucceeded"));
    check(loginProfile.userName == QStringLiteral("alice")
              && loginProfile.userId == QStringLiteral("u1")
              && !loginProfile.accessToken.isEmpty(),
          QStringLiteral("登录结果包含用户与 token"));
    check(statusLabel && !statusLabel->text().contains(loginProfile.accessToken),
          QStringLiteral("状态栏不泄露 token"));

    // T20：图片失败不写永久 LRU；TTL 后自动重试，第二次 200 能恢复。
    emby::EmbyClient cacheClient;
    cacheClient.setServer(server.baseUrl(), loginProfile.accessToken);
    ui::EmbyThumbnailCache cache;
    cache.setClient(&cacheClient);
    cache.setRetryTtl(400);
    const QUrl retryUrl = cacheClient.imageUrl(QStringLiteral("zretry"),
                                               QStringLiteral("Primary"),
                                               QStringLiteral("tag-retry"), 0, 300);
    int cacheReadySignals = 0;
    QObject::connect(&cache, &ui::ThumbnailCache::thumbnailReady,
                     [&cacheReadySignals](const QString &, const QSize &) {
                         ++cacheReadySignals;
                     });
    cache.request(retryUrl.toString(), QSize(64, 64));
    check(waitFor([&cache]() { return cache.stats().failures >= 1; }, 4000),
          QStringLiteral("图片首次 500 被记录为失败"));
    check(cache.cachedCount() == 0 && cache.failureCount() == 1,
          QStringLiteral("图片失败不写入永久 LRU，只记录失败状态"));
    check(!cache.get(retryUrl.toString(), QSize(64, 64)).isNull(),
          QStringLiteral("失败期间显示临时占位图（不永久缓存）"));
    check(waitFor([&cache]() { return cache.stats().successes >= 1; }, 6000),
          QStringLiteral("TTL 到期自动重试，第二次 200 成功恢复"));
    check(cache.failureCount() == 0 && cache.cachedCount() == 1
              && cache.stats().retries >= 1 && cacheReadySignals >= 1,
          QStringLiteral("重试成功写入缓存并清理失败记录（stats: %1）")
              .arg(cache.stats().summary()));

    // T23：同一 URL 被 paint 重复 request 100 次，只应发一次网络请求。
    ui::EmbyThumbnailCache dedupCache;
    dedupCache.setClient(&cacheClient);
    const QUrl dedupUrl = cacheClient.imageUrl(QStringLiteral("dedup"),
                                               QStringLiteral("Primary"),
                                               QStringLiteral("dedup-tag"), 0, 300);
    const QSize dedupSize(64, 64);
    for (int i = 0; i < 100; ++i)
        dedupCache.request(dedupUrl.toString(), dedupSize);
    check(waitFor([&dedupCache]() { return dedupCache.stats().successes >= 1; }, 4000),
          QStringLiteral("重复 request 100 次后图片仍能加载"));
    check(server.imageAttempts.value(QUrl(dedupUrl).path()) == 1
              && dedupCache.stats().requests == 1
              && dedupCache.inFlightCount() == 0 && dedupCache.queuedCount() == 0,
          QStringLiteral("in-flight 去重：同一 URL 只发起 1 次请求（stats: %1）")
              .arg(dedupCache.stats().summary()));

    // T23：并发上限 + FIFO 队列；假服务器延迟 60ms 响应，统计峰值并发。
    ui::EmbyThumbnailCache queueCache;
    queueCache.setClient(&cacheClient);
    queueCache.setMaxConcurrent(6);
    const QSize queueSize(64, 64);
    QStringList slowPaths;
    for (int i = 0; i < 12; ++i) {
        const QUrl slowUrl = cacheClient.imageUrl(QStringLiteral("slow%1").arg(i),
                                                  QStringLiteral("Primary"),
                                                  QStringLiteral("slow-tag"), 0, 300);
        slowPaths.append(QUrl(slowUrl).path());
        queueCache.request(slowUrl.toString(), queueSize);
    }
    check(queueCache.inFlightCount() == 6 && queueCache.queuedCount() == 6,
          QStringLiteral("并发上限 6：12 张图 6 个在途 + 6 个排队（stats: %1）")
              .arg(queueCache.stats().summary()));
    const bool queueDrained =
        waitFor([&queueCache]() { return queueCache.stats().successes >= 12; }, 8000);
    check(queueDrained && server.maxActiveImageRequests <= 6
              && queueCache.stats().requests == 12
              && queueCache.inFlightCount() == 0 && queueCache.queuedCount() == 0,
          QStringLiteral("FIFO 队列完成后续跑，服务器峰值并发 %1 <= 6")
              .arg(server.maxActiveImageRequests));
    bool slowOnce = true;
    for (const QString &slowPath : slowPaths)
        slowOnce = slowOnce && server.imageAttempts.value(slowPath) == 1;
    check(slowOnce, QStringLiteral("排队图片每张只请求 1 次"));

    // T25：复用 EmbyClient QNAM + 默认关闭 HTTP/2；env 可开启对比。
    check(queueCache.qnamReused() && !queueCache.http2Enabled(),
          QStringLiteral("图片复用 EmbyClient QNAM，默认关闭 HTTP/2"));
    qputenv("EMBY_PC_IMAGE_HTTP2", QByteArrayLiteral("1"));
    {
        ui::EmbyThumbnailCache h2Cache;
        h2Cache.setClient(&cacheClient);
        check(h2Cache.http2Enabled(), QStringLiteral("EMBY_PC_IMAGE_HTTP2=1 开启 HTTP/2"));
    }
    qunsetenv("EMBY_PC_IMAGE_HTTP2");

    // T25：服务器 500ms 才返回，传输超时 100ms → 失败释放槽位，最多重试 2 次。
    ui::EmbyThumbnailCache timeoutCache;
    timeoutCache.setClient(&cacheClient);
    timeoutCache.setTransferTimeout(100);
    timeoutCache.setRetryTtl(30);
    const QUrl holdUrl = cacheClient.imageUrl(QStringLiteral("hold"),
                                              QStringLiteral("Primary"),
                                              QStringLiteral("hold-tag"), 0, 300);
    timeoutCache.request(holdUrl.toString(), QSize(64, 64));
    check(waitFor([&timeoutCache]() { return timeoutCache.stats().failures >= 3; }, 6000),
          QStringLiteral("图片传输超时按失败处理，槽位释放"));
    check(timeoutCache.stats().requests == 3
              && timeoutCache.stats().retries == 2
              && timeoutCache.stats().timeoutCount >= 1
              && timeoutCache.stats().exhaustedCount == 1
              && timeoutCache.failureCount() == 1
              && timeoutCache.inFlightCount() == 0
              && timeoutCache.queuedCount() == 0,
          QStringLiteral("超时后重试上限 2 次，之后不再发请求（stats: %1）")
              .arg(timeoutCache.stats().summary()));
    settle(app, 300);
    check(timeoutCache.stats().requests == 3,
          QStringLiteral("重试耗尽后保持 requests=3，不再打服务器"));

    // T25：本地磁盘缓存先读盘、成功写盘；第二次启动/新 cache 不再发网络请求。
    QTemporaryDir diskDir;
    check(diskDir.isValid(), QStringLiteral("创建图片磁盘缓存临时目录"));
    const QSize diskSize(64, 64);
    const QUrl diskUrl = cacheClient.imageUrl(QStringLiteral("disk-cache-item"),
                                              QStringLiteral("Primary"),
                                              QStringLiteral("disk-tag"), 0, 300);
    const QString diskPath = QUrl(diskUrl).path();

    ui::EmbyThumbnailCache diskCache;
    diskCache.setClient(&cacheClient);
    diskCache.setDiskCacheEnabled(true);
    diskCache.setDiskCacheDirectory(diskDir.path());
    diskCache.setDiskCacheMaxBytes(4 * 1024 * 1024);
    diskCache.request(diskUrl.toString(), diskSize);
    check(waitFor([&diskCache]() { return diskCache.stats().successes >= 1; }, 4000),
          QStringLiteral("磁盘缓存首次请求成功"));
    check(diskCache.stats().diskWrites >= 1
              && diskCache.stats().diskBytes > 0
              && server.imageAttempts.value(diskPath) == 1,
          QStringLiteral("图片成功写入本地磁盘缓存（%1 bytes）")
              .arg(diskCache.stats().diskBytes));

    ui::EmbyThumbnailCache diskCache2;
    diskCache2.setClient(&cacheClient);
    diskCache2.setDiskCacheEnabled(true);
    diskCache2.setDiskCacheDirectory(diskDir.path());
    diskCache2.request(diskUrl.toString(), diskSize);
    check(waitFor([&diskCache2]() { return diskCache2.stats().diskHits >= 1; }, 4000),
          QStringLiteral("新 cache 优先读本地磁盘缓存"));
    check(diskCache2.stats().requests == 0
              && diskCache2.stats().diskHits >= 1
              && server.imageAttempts.value(diskPath) == 1,
          QStringLiteral("磁盘命中不发网络请求（stats: %1）")
              .arg(diskCache2.stats().summary()));
    check(!diskCache2.get(diskUrl.toString(), diskSize).isNull(),
          QStringLiteral("磁盘缓存可同步读出海报"));

    // 401
    ui::LoginDialog badLogin(&client);
    badLogin.setInitialValues(server.baseUrl(), QStringLiteral("alice"), QString());
    if (auto *p = badLogin.findChild<QLineEdit *>(QStringLiteral("PasswordEdit")))
        p->setText(QStringLiteral("wrong"));
    if (auto *b = badLogin.findChild<QPushButton *>(QStringLiteral("LoginSubmitButton")))
        b->click();
    auto *badStatus = badLogin.findChild<QLabel *>(QStringLiteral("LoginStatus"));
    check(waitFor([badStatus]() {
              return badStatus && badStatus->text().contains(QStringLiteral("用户名或密码错误"));
          }),
          QStringLiteral("401 显示用户名或密码错误"));

    // 连接被拒绝
    ui::LoginDialog refused(&client);
    refused.setInitialValues(QStringLiteral("http://127.0.0.1:1"), QStringLiteral("alice"),
                             QString());
    if (auto *b = refused.findChild<QPushButton *>(QStringLiteral("TestConnectionButton")))
        b->click();
    auto *refusedStatus = refused.findChild<QLabel *>(QStringLiteral("LoginStatus"));
    check(waitFor([refusedStatus]() {
              return refusedStatus
                     && (refusedStatus->text().contains(QStringLiteral("连接被拒绝"))
                         || refusedStatus->text().contains(QStringLiteral("连接失败"))
                         || refusedStatus->text().contains(QStringLiteral("无法连接")));
          }),
          QStringLiteral("连接失败显示中文错误"));

    // 启动自动恢复 token（MainWindow + 外部 TokenStore）
    ui::QSettingsTokenStore restoreStore;
    restoreStore.clear();
    restoreStore.saveProfile(profile);
    {
        ui::MainWindow::Options options;
        options.requireAuthentication = true;
        options.tokenStore = &restoreStore;
        ui::MainWindow window(options);
        int loginStateSignals = 0;
        bool lastLoginState = false;
        QObject::connect(&window, &ui::MainWindow::loginStateChanged,
                         [&loginStateSignals, &lastLoginState](bool loggedIn) {
                             ++loginStateSignals;
                             lastLoginState = loggedIn;
                         });
        window.resize(960, 600);
        window.show();
        check(waitFor([&window]() { return window.hasSession(); }),
              QStringLiteral("启动时自动恢复 token 并验证成功"));
        check(window.embyClient() != nullptr, QStringLiteral("embyClient() 访问器可用"));
        check(loginStateSignals >= 1 && lastLoginState,
              QStringLiteral("登录成功发出 loginStateChanged(true)"));

        // M2-B：真实数据链路。假服务器提供 Views / Items / Resume / 图片请求。
        check(!window.isMockMode(), QStringLiteral("登录后自动切换到真实数据模式"));
        check(waitFor([&window]() { return window.sectionNames().size() == 2; }),
              QStringLiteral("真实 /Views 填充侧边栏媒体库"));
        check(window.findChildren<QPushButton *>(QStringLiteral("SidebarLibraryButton")).size() == 2,
              QStringLiteral("侧边栏生成 2 个动态媒体库按钮"));
        check(waitFor([&window]() { return window.libraryIds().size() >= 2; }),
              QStringLiteral("首页最近添加来自真实 /Items"));
        auto *realHome = window.findChild<ui::HomeView *>(QStringLiteral("HomePage"));
        check(realHome && realHome->sectionCount() >= 2
                  && realHome->sectionTitles().first() == QStringLiteral("继续观看"),
              QStringLiteral("真实首页按 HomeSection 渲染多行（%1 行）")
                  .arg(realHome ? realHome->sectionCount() : 0));

        window.navigateTo(ui::MainWindow::Page::Library);
        auto *realLibrary = window.findChild<ui::LibraryView *>(QStringLiteral("LibraryContent"));
        check(window.currentStackIndex() == 1, QStringLiteral("真实媒体库页 stack=1"));
        const bool firstPageLoaded =
            waitFor([realLibrary]() { return realLibrary && realLibrary->itemCount() >= 2; });
        const int realFirstCount = realLibrary ? realLibrary->itemCount() : -1;
        check(firstPageLoaded,
              QStringLiteral("真实媒体库分页首屏加载 %1 项").arg(realFirstCount));

        window.setSearchText(QStringLiteral("不存在"));
        const bool searchRequested = waitFor([&server]() {
            for (const QString &target : server.requestedTargets) {
                if (target.contains(QStringLiteral("SearchTerm=")))
                    return true;
            }
            return false;
        });
        check(searchRequested, QStringLiteral("搜索请求带 SearchTerm 参数（服务器端搜索）"));
        check(waitFor([realLibrary]() { return realLibrary && realLibrary->itemCount() == 0; }),
              QStringLiteral("真实 SearchTerm 返回空结果时显示空状态"));
        window.setSearchText(QString());
        check(waitFor([realLibrary]() { return realLibrary && realLibrary->itemCount() >= 2; }),
              QStringLiteral("清空搜索后恢复真实媒体库"));

        // T20：海报回退链 Primary -> Series -> Thumb -> Backdrop。
        const QList<ui::MediaItem> mapped =
            realLibrary ? realLibrary->visibleItems() : QList<ui::MediaItem>();
        auto mappedById = [&mapped](const QString &id) -> ui::MediaItem {
            for (const ui::MediaItem &item : mapped) {
                if (item.id == id)
                    return item;
            }
            return {};
        };
        const ui::MediaItem primaryItem = mappedById(QStringLiteral("m1"));
        const ui::MediaItem thumbItem = mappedById(QStringLiteral("m-thumb"));
        const ui::MediaItem boxSetItem = mappedById(QStringLiteral("b1"));
        const ui::MediaItem noneItem = mappedById(QStringLiteral("m-none"));
        check(primaryItem.posterKey.contains(QStringLiteral("/Images/Primary")),
              QStringLiteral("有 PrimaryImageTag 的条目使用 Primary"));
        check(thumbItem.posterKey.contains(QStringLiteral("/Images/Thumb")),
              QStringLiteral("缺 Primary 但有 Thumb 的条目回退 Thumb"));
        check(boxSetItem.posterKey.contains(QStringLiteral("/Images/Thumb")),
              QStringLiteral("BoxSet 缺 Primary 时回退自身 Thumb"));
        check(noneItem.posterKey.isEmpty(),
              QStringLiteral("完全没有图片字段的条目不生成假 URL"));
        check(window.posterStatsSummary().contains(QStringLiteral("no_image"))
                  && window.posterStatsSummary().contains(QStringLiteral("missing_primary")),
              QStringLiteral("MainWindow 暴露 poster stats（%1）")
                  .arg(window.posterStatsSummary().replace(QLatin1Char('\n'),
                                                           QStringLiteral(" / "))));

        const bool detailOpened = window.openDetail(QStringLiteral("s1"));
        auto *realDetail = window.findChild<ui::DetailView *>();
        check(detailOpened && waitFor([realDetail]() {
                  return realDetail && realDetail->currentItem().id == QStringLiteral("s1")
                         && !realDetail->currentItem().seasons.isEmpty();
              }),
              QStringLiteral("真实详情拉取到剧集分季/分集"));
        const int realEpisodeRows =
            realDetail ? realDetail->findChildren<QWidget *>(QStringLiteral("EpisodeRow")).size() : 0;
        check(realEpisodeRows >= 2,
              QStringLiteral("真实详情渲染 %1 个 EpisodeRow").arg(realEpisodeRows));
        QString episodeFallbackKey;
        if (realDetail && !realDetail->currentItem().seasons.isEmpty()) {
            for (const ui::Episode &episode : realDetail->currentItem().seasons.first().episodes) {
                if (episode.number == 1) {
                    episodeFallbackKey = episode.imageKey;
                    break;
                }
            }
        }
        check(episodeFallbackKey.contains(QStringLiteral("/Items/s1/Images/Primary"))
                  && episodeFallbackKey.contains(QStringLiteral("tagser")),
              QStringLiteral("Episode 缺自身图时回退 Series Primary"));
        check(waitFor([&server]() { return server.imageRequests.size() > 0; }),
              QStringLiteral("海报/背景图通过带 Authorization 的图片请求加载"));
        check(server.lastImageAuthorization.contains(QStringLiteral("Token=")),
              QStringLiteral("图片请求使用 Authorization 头（token 不在 URL）"));
        check(!server.lastImageApiKey.isEmpty(),
              QStringLiteral("图片请求同时带 api_key 查询参数（兼容 qEmby/Jellyfin）"));

        window.setMockMode(true);
        check(window.isMockMode()
                  && window.sectionNames()
                         == QStringList({QStringLiteral("电影"), QStringLiteral("剧集")}),
              QStringLiteral("设置页可切回 mock 数据源"));
        window.setMockMode(false);
        check(waitFor([&window]() { return !window.isMockMode() && window.sectionNames().size() == 2; }),
              QStringLiteral("关闭 mock 后恢复真实媒体库"));

        auto *serverValue = window.findChild<QLabel *>(QStringLiteral("SettingsServerValue"));
        auto *userValue = window.findChild<QLabel *>(QStringLiteral("SettingsUserValue"));
        auto *versionValue = window.findChild<QLabel *>(QStringLiteral("SettingsVersionValue"));
        check(serverValue && serverValue->text() == server.baseUrl(),
              QStringLiteral("设置页显示当前服务器"));
        check(userValue && userValue->text() == QStringLiteral("alice"),
              QStringLiteral("设置页显示当前用户"));
        check(versionValue && versionValue->text().contains(QStringLiteral("4.9.0")),
              QStringLiteral("设置页显示服务器版本"));

        window.navigateTo(ui::MainWindow::Page::Settings);
        if (auto *logout = window.findChild<QPushButton *>(QStringLiteral("SettingsLogoutButton"))) {
            logout->click();
        }
        check(waitFor([&window]() { return !window.hasSession(); }),
              QStringLiteral("退出登录清空会话"));
        check(loginStateSignals >= 2 && !lastLoginState,
              QStringLiteral("退出登录发出 loginStateChanged(false)"));
        check(!restoreStore.loadProfile().isValid(),
              QStringLiteral("退出登录清除持久化 token"));
        if (auto *loginDialog = window.findChild<ui::LoginDialog *>())
            loginDialog->reject();
    }

    // 过期 token：自动恢复失败应回登录页并清除 token
    emby::ServerProfile expired = profile;
    expired.accessToken = QStringLiteral("expired-token");
    restoreStore.saveProfile(expired);
    {
        ui::MainWindow::Options options;
        options.requireAuthentication = true;
        options.tokenStore = &restoreStore;
        ui::MainWindow window(options);
        window.show();
        check(waitFor([&window]() { return window.findChild<ui::LoginDialog *>() != nullptr; }),
              QStringLiteral("过期 token 自动恢复失败后弹出登录框"));
        check(!window.hasSession(), QStringLiteral("过期 token 不进入已登录状态"));
        check(!restoreStore.loadProfile().isAuthenticated(),
              QStringLiteral("401 后清除持久化 token"));
        if (auto *loginDialog = window.findChild<ui::LoginDialog *>())
            loginDialog->reject();
    }

    // T25：图片请求按 qEmby/Jellyfin 兼容方式把 api_key 放图片 URL；元数据 URL 仍不得带 token。
    const QString token = profile.accessToken;
    bool tokenInMetadataUrl = false;
    bool tokenInImageUrl = false;
    for (const QString &target : server.requestedTargets) {
        if (!target.contains(token))
            continue;
        if (target.contains(QStringLiteral("/Images/")))
            tokenInImageUrl = true;
        else
            tokenInMetadataUrl = true;
    }
    check(!tokenInMetadataUrl, QStringLiteral("元数据请求 URL 不含 token"));
    check(tokenInImageUrl, QStringLiteral("图片请求使用 api_key 查询参数（token 只在图片 URL）"));

    QCoreApplication::setOrganizationName(oldOrganization);
    QCoreApplication::setApplicationName(oldApplication);
    out << (failures == 0 ? "AUTH ALL PASS" : QStringLiteral("AUTH FAILURES: %1").arg(failures))
        << "\n";
    out.flush();
    return failures;
}

int runLiveLogin(QApplication &app)
{
    Q_UNUSED(app);
    QTextStream out(stdout);
    const QString url = qEnvironmentVariable("EMBY_TEST_URL");
    const QString user = qEnvironmentVariable("EMBY_TEST_USER");
    const QString password = qEnvironmentVariable("EMBY_TEST_PASSWORD");
    if (url.isEmpty() || user.isEmpty()) {
        out << "SKIP live login: set EMBY_TEST_URL / EMBY_TEST_USER / EMBY_TEST_PASSWORD\n";
        out.flush();
        return 0;
    }

    emby::EmbyClient client;
    client.setRequestTimeout(10000);
    ui::LoginDialog dialog(&client);
    dialog.setInitialValues(url, user, QString());
    if (auto *passwordEdit = dialog.findChild<QLineEdit *>(QStringLiteral("PasswordEdit")))
        passwordEdit->setText(password);

    int successCount = 0;
    emby::ServerProfile profile;
    QObject::connect(&dialog, &ui::LoginDialog::loginSucceeded,
                     [&successCount, &profile](const emby::ServerProfile &p) {
                         ++successCount;
                         profile = p;
                     });

    if (auto *testButton = dialog.findChild<QPushButton *>(QStringLiteral("TestConnectionButton")))
        testButton->click();
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("LoginStatus"));
    const bool testOk = waitFor([status]() {
        return status && status->text().contains(QStringLiteral("连接成功"));
    });
    out << (testOk ? "PASS" : "FAIL") << " live test connection\n";

    if (auto *loginButton = dialog.findChild<QPushButton *>(QStringLiteral("LoginSubmitButton")))
        loginButton->click();
    const bool loginOk = waitFor([&successCount]() { return successCount == 1; }, 10000);
    out << (loginOk ? "PASS" : "FAIL") << " live authenticate\n";

    bool viewsDone = false;
    bool viewsOk = false;
    QString viewsError;
    if (loginOk) {
        client.fetchViews([&viewsDone, &viewsOk, &viewsError](const emby::ItemQueryResult &,
                                                               const emby::ApiError &error) {
            viewsDone = true;
            viewsOk = !error.isError();
            if (error.isError())
                viewsError = ui::friendlyApiError(error);
        });
        waitFor([&viewsDone]() { return viewsDone; }, 10000);
        out << (viewsOk ? "PASS" : "FAIL") << " live fetch Views";
        if (!viewsOk)
            out << " (" << viewsError << ")";
        out << "\n";
    }

    bool restoreOk = false;
    if (loginOk) {
        const emby::ServerInfo info = dialog.serverInfo();
        out << "server: " << info.serverName << " " << info.version << "\n";
        out << "profile user: " << profile.userName << "\n";

        // 再验证一次：持久化 -> 新 MainWindow 启动自动恢复 token -> fetchViews。
        QTemporaryDir settingsDir;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        ui::QSettingsTokenStore store;
        store.clear();
        store.saveProfile(profile);
        ui::MainWindow::Options options;
        options.requireAuthentication = true;
        options.tokenStore = &store;
        ui::MainWindow window(options);
        window.show();
        restoreOk = waitFor([&window]() { return window.hasSession(); }, 10000);
        out << (restoreOk ? "PASS" : "FAIL") << " live startup restore token\n";
        if (!restoreOk) {
            if (auto *loginDialog = window.findChild<ui::LoginDialog *>())
                loginDialog->reject();
        }
        window.close();
    }
    out.flush();
    return (testOk && loginOk && viewsOk && restoreOk) ? 0 : 1;
}

int runLiveData(QApplication &app)
{
    QTextStream out(stdout);
    const QString url = qEnvironmentVariable("EMBY_TEST_URL");
    const QString user = qEnvironmentVariable("EMBY_TEST_USER");
    const QString password = qEnvironmentVariable("EMBY_TEST_PASSWORD");
    if (url.isEmpty() || user.isEmpty()) {
        out << "SKIP live data: set EMBY_TEST_URL / EMBY_TEST_USER / EMBY_TEST_PASSWORD\n";
        out.flush();
        return 0;
    }

    // 隔离 QSettings，避免覆盖真实用户 token。
    QTemporaryDir settingsDir;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QCoreApplication::setOrganizationName(QStringLiteral("emby-pc-live-test"));
    QCoreApplication::setApplicationName(QStringLiteral("ui-live-data"));

    int failures = 0;
    auto check = [&out, &failures](bool ok, const QString &name) {
        out << (ok ? "PASS  " : "FAIL  ") << name << "\n";
        out.flush();
        if (!ok)
            ++failures;
    };

    emby::EmbyClient client;
    client.setRequestTimeout(15000);
    ui::LoginDialog dialog(&client);
    dialog.setInitialValues(url, user, QString());
    if (auto *passwordEdit = dialog.findChild<QLineEdit *>(QStringLiteral("PasswordEdit")))
        passwordEdit->setText(password);
    int successCount = 0;
    emby::ServerProfile profile;
    QObject::connect(&dialog, &ui::LoginDialog::loginSucceeded,
                     [&successCount, &profile](const emby::ServerProfile &p) {
                         ++successCount;
                         profile = p;
                     });
    if (auto *loginButton = dialog.findChild<QPushButton *>(QStringLiteral("LoginSubmitButton")))
        loginButton->click();
    check(waitFor([&successCount]() { return successCount == 1; }, 15000),
          QStringLiteral("真实服务器登录成功"));

    ui::QSettingsTokenStore store;
    store.clear();
    store.saveProfile(profile);

    ui::MainWindow::Options options;
    options.requireAuthentication = true;
    options.tokenStore = &store;
    ui::MainWindow window(options);
    window.resize(1280, 760);
    window.show();
    check(waitFor([&window]() { return window.hasSession() && !window.isMockMode(); }, 15000),
          QStringLiteral("MainWindow 自动恢复 token 并进入真实数据模式"));

    const bool viewsLoaded =
        waitFor([&window]() { return !window.sectionNames().isEmpty(); }, 15000);
    out << "INFO  真实 Views：" << window.sectionNames().join(QStringLiteral(" / ")) << "\n";
    check(viewsLoaded, QStringLiteral("侧边栏加载真实 /Views（%1 个媒体库）")
                           .arg(window.sectionNames().size()));

    const bool homeLoaded = waitFor([&window]() { return window.libraryIds().size() > 0; }, 15000);
    out << "INFO  首页最近添加 " << window.libraryIds().size() << " 项\n";
    check(homeLoaded, QStringLiteral("首页最近添加加载真实 Items"));

    // 真实图片端点：Authorization + 解码 + 缓存写入（T20）。
    QString posterKey;
    for (const ui::MediaItem &item : window.visibleItems()) {
        if (!item.posterKey.isEmpty()) {
            posterKey = item.posterKey;
            break;
        }
    }
    check(!posterKey.isEmpty(), QStringLiteral("真实条目生成海报 URL"));
    if (!posterKey.isEmpty()) {
        ui::EmbyThumbnailCache posterCache;
        posterCache.setClient(window.embyClient());
        posterCache.setRetryTtl(500);
        const QSize posterSize(160, 240);
        posterCache.request(posterKey, posterSize);
        const bool posterDownloaded =
            waitFor([&posterCache]() { return posterCache.stats().successes >= 1; }, 15000);
        check(posterDownloaded,
              QStringLiteral("真实海报下载成功（%1）").arg(posterCache.stats().summary()));
        check(!posterCache.get(posterKey, posterSize).isNull()
                  && posterCache.cachedCount() == 1,
              QStringLiteral("真实海报写入 LRU 并可同步读取"));
    }

    // 媒体库首屏 + 滚动分页
    window.navigateTo(ui::MainWindow::Page::Library);
    auto *library = window.findChild<ui::LibraryView *>(QStringLiteral("LibraryContent"));
    const bool firstPageOk = waitFor([library]() { return library && library->itemCount() > 0; }, 15000);
    check(firstPageOk, QStringLiteral("媒体库首屏加载"));
    if (library) {
        const int initialCount = library->itemCount();
        const int total = library->totalCount();
        out << "INFO  首屏 " << initialCount << " / 总数 " << total << "（Limit=60）"
            << " hasMore=" << library->hasMore() << "\n";
        if (library->hasMore() || total > initialCount) {
            settle(app, 300);
            if (auto *grid = library->findChild<QListView *>(QStringLiteral("PosterGrid")))
                grid->verticalScrollBar()->setValue(grid->verticalScrollBar()->maximum());
            const bool paged = waitFor([library, initialCount]() {
                return library->itemCount() > initialCount;
            }, 15000);
            check(paged, QStringLiteral("滚动到底部触发分页加载（%1 -> %2）")
                             .arg(initialCount)
                             .arg(library->itemCount()));
        } else {
            out << "INFO  该媒体库不足一页，跳过分页断言\n";
        }

        const QList<ui::MediaItem> visible = library->visibleItems();
        check(!visible.isEmpty(), QStringLiteral("媒体库返回可见 item"));
        if (!visible.isEmpty()) {
            window.openDetail(visible.first().id);
            auto *detail = window.findChild<ui::DetailView *>();
            const bool detailOk = waitFor([detail]() {
                return detail && !detail->currentItem().id.isEmpty()
                       && !detail->currentItem().title.isEmpty();
            }, 15000);
            out << "INFO  详情：" << (detail ? detail->currentItem().title : QString()) << "\n";
            check(detailOk, QStringLiteral("真实详情加载"));
        }
    }

    // 剧集详情：季/集
    window.navigateTo(ui::MainWindow::Page::Series);
    const bool seriesSection = waitFor([&window]() { return window.currentSectionIndex() >= 0; }, 8000);
    auto *series = window.findChild<ui::LibraryView *>(QStringLiteral("LibraryContent"));
    const bool seriesLoaded =
        seriesSection && waitFor([series]() { return series && series->itemCount() > 0; }, 15000);
    check(seriesLoaded, QStringLiteral("剧集媒体库加载"));
    if (seriesLoaded) {
        const QList<ui::MediaItem> visible = series->visibleItems();
        if (!visible.isEmpty()) {
            window.openDetail(visible.first().id);
            auto *detail = window.findChild<ui::DetailView *>();
            const bool seasonsOk = waitFor([detail]() {
                return detail && !detail->currentItem().seasons.isEmpty()
                       && !detail->currentItem().seasons.first().episodes.isEmpty();
            }, 15000);
            out << "INFO  剧集季数 " << (detail ? detail->currentItem().seasons.size() : 0)
                << "，首季集数 "
                << (detail && !detail->currentItem().seasons.isEmpty()
                        ? detail->currentItem().seasons.first().episodes.size()
                        : 0)
                << "\n";
            check(seasonsOk, QStringLiteral("剧集详情加载真实季/集"));
        }
    }

    out << (failures == 0 ? "LIVE DATA ALL PASS"
                          : QStringLiteral("LIVE DATA FAILURES: %1").arg(failures))
        << "\n";
    out.flush();
    window.close();
    return failures == 0 ? 0 : 1;
}

int runLivePosterGaps(QApplication &app)
{
    Q_UNUSED(app);
    QTextStream out(stdout);
    const QString url = qEnvironmentVariable("EMBY_TEST_URL");
    const QString user = qEnvironmentVariable("EMBY_TEST_USER");
    const QString password = qEnvironmentVariable("EMBY_TEST_PASSWORD");
    if (url.isEmpty() || user.isEmpty()) {
        out << "SKIP live poster gaps: set EMBY_TEST_URL / EMBY_TEST_USER / EMBY_TEST_PASSWORD\n";
        out.flush();
        return 0;
    }

    emby::EmbyClient loginClient;
    loginClient.setRequestTimeout(15000);
    ui::LoginDialog dialog(&loginClient);
    dialog.setInitialValues(url, user, QString());
    if (auto *passwordEdit = dialog.findChild<QLineEdit *>(QStringLiteral("PasswordEdit")))
        passwordEdit->setText(password);
    int successCount = 0;
    emby::ServerProfile profile;
    QObject::connect(&dialog, &ui::LoginDialog::loginSucceeded,
                     [&successCount, &profile](const emby::ServerProfile &p) {
                         ++successCount;
                         profile = p;
                     });
    if (auto *loginButton = dialog.findChild<QPushButton *>(QStringLiteral("LoginSubmitButton")))
        loginButton->click();
    if (!waitFor([&successCount]() { return successCount == 1; }, 15000)) {
        out << "FAIL live poster gaps: login failed\n";
        out.flush();
        return 1;
    }

    emby::EmbyClient client;
    client.setProfile(profile);
    client.setRequestTimeout(15000);

    struct Sample
    {
        QString type;
        int total = 0;
        int primary = 0;
        int thumb = 0;
        int backdrop = 0;
        int seriesPrimary = 0;
        int anyImage = 0;
        int none = 0;
        QStringList missingPrimary;
        QStringList missingAll;
    };

    const QStringList types = {QStringLiteral("Movie"), QStringLiteral("Series"),
                               QStringLiteral("BoxSet")};
    QList<Sample> samples;
    for (const QString &type : types) {
        Sample sample;
        sample.type = type;
        samples.append(sample);
    }

    int pending = samples.size();
    bool failed = false;
    QString failText;
    for (int i = 0; i < samples.size(); ++i) {
        Sample &sample = samples[i];
        emby::ItemQuery query;
        query.recursive = true;
        query.includeItemTypes = {sample.type};
        query.limit = 100;
        query.sortBy = {QStringLiteral("SortName")};
        query.fields = {QStringLiteral("ImageTags"), QStringLiteral("BackdropImageTags"),
                        QStringLiteral("PrimaryImageAspectRatio")};
        client.fetchItems(query, [&sample, &pending, &failed, &failText](
                                     const emby::ItemQueryResult &result,
                                     const emby::ApiError &error) {
            if (error.isError()) {
                failed = true;
                failText = error.toString();
                --pending;
                return;
            }
            sample.total = result.items.size();
            for (const emby::MediaItem &item : result.items) {
                const bool hasPrimary = !item.primaryImageTag.isEmpty();
                const bool hasThumb = !item.thumbImageTag.isEmpty();
                const bool hasBackdrop = !item.backdropImageTags.isEmpty();
                if (hasPrimary)
                    ++sample.primary;
                if (hasThumb)
                    ++sample.thumb;
                if (hasBackdrop)
                    ++sample.backdrop;
                if (!item.seriesPrimaryImageTag.isEmpty())
                    ++sample.seriesPrimary;
                if (hasPrimary || hasThumb || hasBackdrop)
                    ++sample.anyImage;
                else
                    ++sample.none;
                if (!hasPrimary)
                    sample.missingPrimary.append(
                        QStringLiteral("%1 (%2)").arg(item.name.isEmpty() ? item.id : item.name,
                                                     item.id));
                if (!hasPrimary && !hasThumb && !hasBackdrop)
                    sample.missingAll.append(
                        QStringLiteral("%1 (%2)").arg(item.name.isEmpty() ? item.id : item.name,
                                                     item.id));
            }
            --pending;
        });
    }
    waitFor([&pending]() { return pending == 0; }, 20000);
    if (failed) {
        out << "FAIL live poster gaps: " << failText << "\n";
        out.flush();
        return 1;
    }

    out << "## Emby poster gap sample (up to 100 per type)\n\n";
    out << "| Type | Total | Primary tag | Thumb tag | Backdrop tag | Series Primary | Any image | No image |\n";
    out << "|---|---:|---:|---:|---:|---:|---:|---:|\n";
    for (const Sample &sample : samples) {
        const auto pct = [](int value, int total) {
            if (total <= 0)
                return QStringLiteral("-");
            return QStringLiteral("%1%").arg(100.0 * value / total, 0, 'f', 1);
        };
        out << "| " << sample.type << " | " << sample.total << " | "
            << sample.primary << " (" << pct(sample.primary, sample.total) << ") | "
            << sample.thumb << " (" << pct(sample.thumb, sample.total) << ") | "
            << sample.backdrop << " (" << pct(sample.backdrop, sample.total) << ") | "
            << sample.seriesPrimary << " (" << pct(sample.seriesPrimary, sample.total) << ") | "
            << sample.anyImage << " (" << pct(sample.anyImage, sample.total) << ") | "
            << sample.none << " (" << pct(sample.none, sample.total) << ") |\n";
    }
    for (const Sample &sample : samples) {
        if (!sample.missingAll.isEmpty())
            out << "\nNo image at all — " << sample.type << ": "
                << sample.missingAll.join(QStringLiteral("; ")) << "\n";
        else if (!sample.missingPrimary.isEmpty())
            out << "\nMissing Primary (has fallback) — " << sample.type << ": "
                << sample.missingPrimary.join(QStringLiteral("; ")) << "\n";
    }
    out.flush();
    return 0;
}

int runScreenshots(QApplication &app, const QString &directory)
{
    Q_UNUSED(app);
    QDir dir(directory);
    if (!dir.exists() && !QDir().mkpath(directory)) {
        QTextStream(stderr) << "cannot create screenshot directory: " << directory << "\n";
        return 1;
    }

    ui::MainWindow window(ui::MainWindow::Options{false});
    window.resize(1280, 760);
    window.show();
    settle(app, 220);

    auto save = [&window, &dir](const QString &name) {
        QPixmap pixmap = window.grab();
        // T18：顶层半透明 overlay 不在 window.grab() 里，截图时按全局坐标合成。
        if (auto *player = window.findChild<ui::PlayerPage *>()) {
            if (player->isPlayerModeActive()) {
                QPainter painter(&pixmap);
                for (QWidget *overlay : {player->topOverlay(), player->bottomOverlay()}) {
                    if (!overlay || !overlay->isVisible())
                        continue;
                    const QPoint offset = overlay->mapToGlobal(QPoint(0, 0))
                                          - window.mapToGlobal(QPoint(0, 0));
                    painter.drawPixmap(offset, overlay->grab());
                }
            }
        }
        const QString path = dir.filePath(name + QStringLiteral(".png"));
        const bool ok = pixmap.save(path);
        QTextStream(stdout) << (ok ? "saved " : "FAILED ") << path << "\n";
    };

    window.navigateTo(ui::MainWindow::Page::Home);
    settle(app, 200);
    save(QStringLiteral("01-home"));

    window.navigateTo(ui::MainWindow::Page::Movies);
    settle(app, 200);
    save(QStringLiteral("02-movies"));

    const QString firstId = window.libraryIds().first();
    window.openDetail(firstId);
    settle(app, 240);
    save(QStringLiteral("03-detail"));

    if (auto *scroll = window.findChild<QScrollArea *>(QStringLiteral("DetailScroll"))) {
        scroll->verticalScrollBar()->setValue(300);
        settle(app, 200);
        save(QStringLiteral("04-detail-episodes"));
    }

    window.openPlayerDemo(firstId, 1, 2);
    auto *player = window.findChild<ui::PlayerPage *>();
    if (player) {
        player->setOverlayAutoHide(false);
        player->setControlsVisible(true);
    }
    settle(app, 240);
    save(QStringLiteral("05-player"));
    save(QStringLiteral("12-player-mode-1280x760"));

    // 自动隐藏后的画面（纯视频，无浮层）作为 T18 回归截图。
    if (player) {
        player->setControlsVisible(false);
        settle(app, 120);
    }
    save(QStringLiteral("14-player-mode-autohide"));
    if (player)
        player->setControlsVisible(true);

    window.resize(960, 600);
    settle(app, 240);
    window.navigateTo(ui::MainWindow::Page::Home);
    settle(app, 240);
    save(QStringLiteral("06-home-960x600"));

    window.resize(1280, 760);
    settle(app, 200);
    window.navigateTo(ui::MainWindow::Page::Settings);
    settle(app, 200);
    save(QStringLiteral("07-settings"));

    window.resize(1920, 1080);
    settle(app, 240);
    window.navigateTo(ui::MainWindow::Page::Home);
    settle(app, 260);
    save(QStringLiteral("08-home-1920x1080"));

    window.resize(960, 600);
    settle(app, 220);
    window.openDetail(firstId);
    settle(app, 220);
    save(QStringLiteral("09-detail-960x600"));

    window.openPlayerDemo(firstId, 1, 2);
    settle(app, 220);
    save(QStringLiteral("10-player-960x600"));
    save(QStringLiteral("13-player-mode-960x600"));

    emby::EmbyClient client;
    ui::LoginDialog login(&client);
    login.setInitialValues(QStringLiteral("http://192.168.1.10:8096"), QStringLiteral("demo"), QString());
    login.resize(560, login.sizeHint().height());
    login.show();
    settle(app, 180);
    const QString loginPath = dir.filePath(QStringLiteral("11-login-dialog.png"));
    if (login.grab().save(loginPath))
        QTextStream(stdout) << "saved " << loginPath << "\n";
    login.close();

    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("emby-pc-ui-demo"));
    app.setOrganizationName(QStringLiteral("emby-pc"));
    ui::Theme::apply(app);

    const QStringList args = QApplication::arguments();
    if (args.contains(QStringLiteral("--selftest"))) {
        int failures = runSelfTest(app);
        failures += runAuthTests(app);
        failures += runWindowStateTests(app);
        failures += runSettingsTests(app);
        failures += runLibraryLayoutTests(app);
        return failures == 0 ? 0 : 1;
    }

    if (args.contains(QStringLiteral("--live-login")))
        return runLiveLogin(app);

    if (args.contains(QStringLiteral("--live-data")))
        return runLiveData(app);

    if (args.contains(QStringLiteral("--live-poster-gaps")))
        return runLivePosterGaps(app);

    const int screenshotIndex = args.indexOf(QStringLiteral("--screenshot"));
    if (screenshotIndex >= 0) {
        const QString directory = (screenshotIndex + 1 < args.size() && !args.at(screenshotIndex + 1).startsWith(QLatin1Char('-')))
                                      ? args.at(screenshotIndex + 1)
                                      : QStringLiteral("preview");
        return runScreenshots(app, directory);
    }

    ui::MainWindow window(ui::MainWindow::Options{false});
    window.show();
    return app.exec();
}