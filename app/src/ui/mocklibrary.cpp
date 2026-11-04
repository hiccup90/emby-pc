#include "mocklibrary.h"

#include <QStringList>

namespace ui {
namespace {

const QStringList kTitles = {
    QStringLiteral("星际穿越"), QStringLiteral("银翼杀手 2049"), QStringLiteral("沙丘"),
    QStringLiteral("降临"), QStringLiteral("盗梦空间"), QStringLiteral("普罗米修斯"),
    QStringLiteral("地心引力"), QStringLiteral("火星救援"), QStringLiteral("月球"),
    QStringLiteral("致命魔术"), QStringLiteral("记忆碎片"), QStringLiteral("敦刻尔克"),
    QStringLiteral("信条"), QStringLiteral("奥本海默"), QStringLiteral("热街"),
    QStringLiteral("猎杀星期一"), QStringLiteral("深空回响"), QStringLiteral("寂静之地"),
    QStringLiteral("异星觉醒"), QStringLiteral("时空线索"), QStringLiteral("午夜档案"),
    QStringLiteral("第七封印"), QStringLiteral("雾中风景"), QStringLiteral("夏日终曲"),
    QStringLiteral("北方航线"), QStringLiteral("海边的曼彻斯特"), QStringLiteral("夜行者"),
    QStringLiteral("灯塔"), QStringLiteral("机械师"), QStringLiteral("罗盘"),
    QStringLiteral("白熊"), QStringLiteral("失控玩家"), QStringLiteral("头号玩家"),
    QStringLiteral("移动迷宫"), QStringLiteral("安德的游戏"), QStringLiteral("银河护卫队"),
    QStringLiteral("绝地救援"), QStringLiteral("冰川时代"), QStringLiteral("深海回声"),
    QStringLiteral("孤岛惊魂"),
};

const QStringList kGenres = {
    QStringLiteral("科幻"), QStringLiteral("剧情"), QStringLiteral("悬疑"),
    QStringLiteral("犯罪"), QStringLiteral("冒险"), QStringLiteral("动画"),
    QStringLiteral("纪录片"), QStringLiteral("惊悚"),
};

const QStringList kEpisodeTitles = {
    QStringLiteral("初次接触"), QStringLiteral("暗流"), QStringLiteral("回声"),
    QStringLiteral("临界点"), QStringLiteral("裂隙"), QStringLiteral("归途"),
    QStringLiteral("信号"), QStringLiteral("静默"), QStringLiteral("余烬"),
    QStringLiteral("破晓"), QStringLiteral("长夜"), QStringLiteral("远方"),
};

QString makeOverview(const QString &title, int year, const QString &genre, int seed)
{
    static const QStringList templates = {
        QStringLiteral("《%1》是一部 %2 年的%3作品。故事从一次看似普通的相遇开始，随着线索逐渐拼合，"
                       "主角不得不面对被刻意掩埋的过去。节奏克制，镜头语言冷峻，适合喜欢慢热叙事的观众。"),
        QStringLiteral("在《%1》中，%2 年的这座城市正经历一场无声的变革。%3的外壳下藏着关于选择与代价的追问，"
                       "每一次决定都会把人物推向更深的漩涡。"),
        QStringLiteral("《%1》（%2）以%3为底色，讲述一群普通人在极端环境下的相互试探与彼此救赎。"
                       "影片用大量留白制造张力，结尾的十分钟尤其值得回味。"),
        QStringLiteral("这是一部 %2 年出品的%3片《%1》。叙事在两条时间线之间来回切换，"
                       "细节埋得很深；第一次观看时建议关掉弹幕，专心跟着镜头走。"),
    };
    return templates.at(seed % templates.size()).arg(title).arg(year).arg(genre);
}

} // namespace

QList<MediaItem> makeMockLibrary(int count)
{
    QList<MediaItem> items;
    items.reserve(count);

    for (int i = 0; i < count; ++i) {
        MediaItem item;
        item.id = QStringLiteral("mock-%1").arg(i + 1, 4, 10, QLatin1Char('0'));
        item.type = (i % 3 == 0) ? QStringLiteral("Series") : QStringLiteral("Movie");

        QString title = kTitles.at(i % kTitles.size());
        if (i >= kTitles.size())
            title += QStringLiteral(" · 卷 %1").arg(i / kTitles.size() + 1);
        item.title = title;
        item.originalTitle = title;

        item.year = 1998 + (i * 7) % 27;
        const QString genreA = kGenres.at((i * 3) % kGenres.size());
        const QString genreB = kGenres.at((i + 2) % kGenres.size());
        item.genres = (genreA == genreB) ? QStringList{genreA} : QStringList{genreA, genreB};
        item.overview = makeOverview(title, item.year, genreA, i);
        item.communityRating = 6.0 + ((i * 17) % 40) / 10.0;
        item.officialRating = (i % 4 == 0) ? QStringLiteral("PG-13") : QStringLiteral("TV-MA");
        item.runtimeMinutes = item.isSeries() ? 45 : 90 + (i * 13) % 70;
        item.posterKey = QStringLiteral("poster:mock/%1").arg(item.id);
        item.backdropKey = QStringLiteral("backdrop:mock/%1").arg(item.id);

        if (i % 6 == 0)
            item.progress = 0.08 + ((i * 23) % 70) / 100.0;
        if (i % 17 == 0) {
            item.progress = 1.0;
            item.played = true;
        }
        if (item.isSeries() && item.progress >= 0.99)
            item.progress = 0.0;

        if (item.isSeries()) {
            const int seasonCount = 1 + (i % 3);
            for (int s = 1; s <= seasonCount; ++s) {
                Season season;
                season.number = s;
                season.name = QStringLiteral("第 %1 季").arg(s);
                const int episodeCount = 6 + ((i + s) % 4);
                for (int e = 1; e <= episodeCount; ++e) {
                    Episode ep;
                    ep.number = e;
                    ep.title = kEpisodeTitles.at((i + s + e) % kEpisodeTitles.size());
                    ep.runtimeMinutes = 40 + ((i + s * 7 + e * 3) % 20);
                    ep.overview = QStringLiteral("第 %1 季第 %2 集。%3")
                                      .arg(s)
                                      .arg(e)
                                      .arg(makeOverview(ep.title, item.year, genreA, i + s + e));
                    if (s == 1 && e == 1 && item.progress > 0.0)
                        ep.progress = item.progress;
                    else if (s == 1 && e == 1 && i % 9 == 0)
                        ep.progress = 0.35;
                    season.episodes.append(ep);
                }
                item.seasons.append(season);
            }
        }

        items.append(item);
    }

    return items;
}

QString mockServerName()
{
    return QStringLiteral("HomeLab · Emby");
}

QString mockUserName()
{
    return QStringLiteral("demo");
}

QString mockUserInitial()
{
    return QStringLiteral("D");
}

} // namespace ui
