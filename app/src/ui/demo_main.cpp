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
#include "libraryview.h"
#include "logindialog.h"
#include "mainwindow.h"
#include "playerpage.h"
#include "theme.h"
#include "tokenstore.h"

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
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

    void respond(QTcpSocket *socket, const QString &method, const QString &target,
                 const QMap<QString, QString> &headers, const QByteArray &body)
    {
        int status = 200;
        QByteArray reason = "OK";
        QByteArray responseBody = "{}";
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
                       && path.endsWith(QLatin1String("/Images/Primary"))) {
                imageRequests.append(target);
                lastImageAuthorization = authorization;
                status = 404;
                reason = "Not Found";
                responseBody = R"({"Message":"no image"})";
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
                        R"({"Items":[{"Id":"e1","Name":"试播集","Type":"Episode","IndexNumber":1,"ParentIndexNumber":1,"SeriesId":"s1","RunTimeTicks":27000000000,"UserData":{"Played":false,"PlaybackPositionTicks":0}},{"Id":"e2","Name":"第二集","Type":"Episode","IndexNumber":2,"ParentIndexNumber":1,"SeriesId":"s1","RunTimeTicks":27000000000,"UserData":{"Played":false,"PlaybackPositionTicks":0}}],"TotalRecordCount":2})";
                } else if (searchTerm == QLatin1String("不存在")) {
                    responseBody = R"({"Items":[],"TotalRecordCount":0})";
                } else if (parentId == QLatin1String("v-tv")) {
                    responseBody =
                        R"({"Items":[{"Id":"s1","Name":"假剧集","Type":"Series","ProductionYear":2024,"CommunityRating":8.5,"ImageTags":{"Primary":"tags1"},"BackdropImageTags":["bds1"],"UserData":{"Played":false,"PlaybackPositionTicks":0}}],"TotalRecordCount":1})";
                } else {
                    responseBody =
                        R"({"Items":[{"Id":"m1","Name":"假电影","Type":"Movie","ProductionYear":2023,"CommunityRating":7.8,"RunTimeTicks":54000000000,"ImageTags":{"Primary":"tagm1"},"BackdropImageTags":["bdm1"],"Genres":["科幻"],"Overview":"测试用简介","UserData":{"Played":false,"PlaybackPositionTicks":0}},{"Id":"s1","Name":"假剧集","Type":"Series","ProductionYear":2024,"CommunityRating":8.5,"ImageTags":{"Primary":"tags1"},"BackdropImageTags":["bds1"],"UserData":{"Played":false,"PlaybackPositionTicks":0}}],"TotalRecordCount":2})";
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

        QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + " " + reason + "\r\n";
        response += "Content-Type: application/json\r\n";
        response += "Content-Length: " + QByteArray::number(responseBody.size()) + "\r\n";
        response += "Connection: close\r\n\r\n";
        response += responseBody;
        socket->write(response);
        socket->flush();
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

    check(window.isMockMode() && window.sectionNames()
              == QStringList({QStringLiteral("电影"), QStringLiteral("剧集")}),
          QStringLiteral("mock 模式侧边栏媒体库来自 2 个 section"));
    check(window.findChild<QWidget *>(QStringLiteral("ResumeSection")) != nullptr
              && window.findChildren<QWidget *>(QStringLiteral("ResumeCard")).size() > 0,
          QStringLiteral("首页继续观看横向列表已渲染 Resume 卡片"));

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

    // 播放页与信号
    window.openPlayerDemo(firstId, 1, 2);
    check(window.currentStackIndex() == 4, QStringLiteral("播放页 stack=4"));
    auto *player = window.findChild<ui::PlayerPage *>();
    check(player != nullptr && player->isPlaying(), QStringLiteral("播放页处于播放状态（mock）"));
    if (auto *subtitleButton = player->findChild<QPushButton *>(QStringLiteral("SubtitleButton")))
        check(subtitleButton->text().contains(QStringLiteral("中文")),
              QStringLiteral("字幕按钮反映当前轨道：%1").arg(subtitleButton->text()));

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

    // 播放页返回
    player->findChild<QPushButton *>(QStringLiteral("PlayerBackButton"))->click();
    check(window.currentStackIndex() == 3, QStringLiteral("播放页返回详情页（stack=3）"));

    // 最小窗口尺寸下布局不崩
    window.resize(960, 600);
    settle(app, 120);
    check(window.minimumWidth() == 960 && window.minimumHeight() == 600,
          QStringLiteral("最小尺寸 960x600"));

    out << (failures == 0 ? "ALL PASS" : QStringLiteral("FAILURES: %1").arg(failures)) << "\n";
    out.flush();
    return failures == 0 ? 0 : 1;
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
        check(waitFor([&server]() { return server.imageRequests.size() > 0; }),
              QStringLiteral("海报/背景图通过带 Authorization 的图片请求加载"));
        check(server.lastImageAuthorization.contains(QStringLiteral("Token=")),
              QStringLiteral("图片请求使用 Authorization 头（token 不在 URL）"));

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

    // token 不拼进任何请求 URL
    const QString token = profile.accessToken;
    bool tokenInUrl = false;
    for (const QString &target : server.requestedTargets) {
        if (target.contains(token))
            tokenInUrl = true;
    }
    check(!tokenInUrl, QStringLiteral("token 未拼进任何请求 URL"));

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

        const QStringList ids = library->visibleItemIds();
        check(!ids.isEmpty(), QStringLiteral("媒体库返回可见 item"));
        if (!ids.isEmpty()) {
            window.openDetail(ids.first());
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
        const QStringList ids = series->visibleItemIds();
        if (!ids.isEmpty()) {
            window.openDetail(ids.first());
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
        const QString path = dir.filePath(name + QStringLiteral(".png"));
        const bool ok = window.grab().save(path);
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
    settle(app, 240);
    save(QStringLiteral("05-player"));

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
        return failures == 0 ? 0 : 1;
    }

    if (args.contains(QStringLiteral("--live-login")))
        return runLiveLogin(app);

    if (args.contains(QStringLiteral("--live-data")))
        return runLiveData(app);

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