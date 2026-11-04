#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>

namespace ui {

// 线性矢量图标集：不依赖 Qt 主题/系统图标，深色主题下颜色可控，
// 也避免 Windows/Linux 字体图标缺字。绘制在 24x24 逻辑坐标内。
enum class Icon {
    Home,
    Movie,
    Tv,
    Settings,
    Search,
    User,
    Play,
    Pause,
    Prev,
    Next,
    Back,
    Subtitle,
    Audio,
    Stats,
    Fullscreen,
    ExitFullscreen,
    Volume,
    Heart,
    More,
    Grid,
    Check,
    Clock,
    Server,
};

QPixmap iconPixmap(Icon icon, const QColor &color, int size = 20);
QIcon icon(Icon icon, const QColor &color = QColor(0xCB, 0xD5, 0xE1), int size = 20);

} // namespace ui
