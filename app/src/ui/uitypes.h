#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace ui {

// 与 Emby API DTO 对应的最小媒体子集。当前由 mocklibrary 填充，
// 后续由 app/src/emby 的映射层填充（字段保持一一对应，避免 UI 改动）。
struct Episode
{
    int number = 0;
    QString title;
    QString overview;
    int runtimeMinutes = 0;
    bool played = false;
    double progress = 0.0; // 0.0 ~ 1.0
    QString imageKey;      // 真实图片 URL；为空时 DetailView 用离线占位图
};

struct Season
{
    int number = 0;
    QString name; // "第 1 季"
    QList<Episode> episodes;
};

struct MediaItem
{
    QString id;
    QString type; // "Movie" / "Series"
    QString title;
    QString originalTitle;
    int year = 0;
    QString officialRating; // "PG-13"
    QStringList genres;
    QString overview;
    QString tagline;
    double communityRating = 0.0;
    int runtimeMinutes = 0;
    QString posterKey;   // 缩略图缓存 key，例如 "poster:mock/0001"
    QString backdropKey; // 例如 "backdrop:mock/0001"
    bool played = false;
    double progress = 0.0; // 0.0 ~ 1.0
    QList<Season> seasons; // 剧集才有

    bool isSeries() const { return type == QLatin1String("Series"); }
    QString genreText() const { return genres.join(QStringLiteral(" / ")); }
};

// mpv 的轨道信息（字幕/音轨菜单共用）
struct TrackInfo
{
    QString id;
    QString language; // "chi" / "eng"
    QString title;    // "中文（简体）"
    QString codec;    // "ass" / "aac"
    bool selected = false;
};

} // namespace ui