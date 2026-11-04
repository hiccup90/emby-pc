#pragma once

#include "uitypes.h"

namespace ui {

// 生成确定性的 mock 媒体库（默认 200 部，含电影与剧集、季/集结构、观看进度）。
// 同一个 id 每次生成的内容一致，便于 UI 截图与回归对比。
QList<MediaItem> makeMockLibrary(int count = 200);

// 侧边栏/顶栏用的 mock 身份信息
QString mockServerName();
QString mockUserName();
QString mockUserInitial();

} // namespace ui
