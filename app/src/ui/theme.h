#pragma once

#include <QString>

class QApplication;

namespace ui {

// 深色主题：Fusion + 调色板 + style/dark.qss（编译进 Qt 资源）。
// MainWindow 构造时会自动调用一次，独立 demo 里也可显式调用。
class Theme
{
public:
    static void apply(QApplication &app);
    static QString styleSheet();
};

} // namespace ui
