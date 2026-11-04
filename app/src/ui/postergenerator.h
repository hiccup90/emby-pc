#pragma once

#include <QPixmap>
#include <QSize>
#include <QString>

namespace ui {

// 离线海报/背景图生成器：用缓存 key 的哈希稳定地生成不同配色的抽象海报，
// 目的是在没有网络的 skeleton 阶段也能验证海报网格的布局、比例与缓存路径。
// 后续接入 Emby 图片接口后，由 ThumbnailCache 的网络实现替换掉这里即可。
QPixmap makePosterArt(const QString &key, const QSize &size, const QString &title = QString());
QPixmap makeBackdropArt(const QString &key, const QSize &size, const QString &title = QString());

} // namespace ui
