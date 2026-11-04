#include "theme.h"

#include <QApplication>
#include <QFile>
#include <QFont>
#include <QPalette>
#include <QResource>
#include <QStyle>
#include <QStyleFactory>

// 静态库里的 Qt 资源需要显式初始化，否则链接器可能丢掉 qrc 目标文件。
static void embyUiEnsureResources()
{
    Q_INIT_RESOURCE(ui_style);
}

namespace ui {

QString Theme::styleSheet()
{
    embyUiEnsureResources();
    QFile file(QStringLiteral(":/ui/dark.qss"));
    if (file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString::fromUtf8(file.readAll());

#ifdef EMBY_UI_SOURCE_DIR
    QFile fallback(QStringLiteral(EMBY_UI_SOURCE_DIR "/style/dark.qss"));
    if (fallback.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString::fromUtf8(fallback.readAll());
#endif
    return {};
}

void Theme::apply(QApplication &app)
{
    static bool applied = false;
    if (applied)
        return;
    applied = true;

    if (QStyle *fusion = QStyleFactory::create(QStringLiteral("Fusion")))
        app.setStyle(fusion);

    QPalette palette;
    palette.setColor(QPalette::Window, QColor(0x0B, 0x12, 0x20));
    palette.setColor(QPalette::WindowText, QColor(0xE2, 0xE8, 0xF0));
    palette.setColor(QPalette::Base, QColor(0x11, 0x1C, 0x2E));
    palette.setColor(QPalette::AlternateBase, QColor(0x0F, 0x17, 0x2A));
    palette.setColor(QPalette::Text, QColor(0xE2, 0xE8, 0xF0));
    palette.setColor(QPalette::Button, QColor(0x1E, 0x29, 0x3B));
    palette.setColor(QPalette::ButtonText, QColor(0xE2, 0xE8, 0xF0));
    palette.setColor(QPalette::Highlight, QColor(0x25, 0x63, 0xEB));
    palette.setColor(QPalette::HighlightedText, QColor(0xFF, 0xFF, 0xFF));
    palette.setColor(QPalette::ToolTipBase, QColor(0x1E, 0x29, 0x3B));
    palette.setColor(QPalette::ToolTipText, QColor(0xE2, 0xE8, 0xF0));
    palette.setColor(QPalette::PlaceholderText, QColor(0x64, 0x74, 0x8B));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(0x64, 0x74, 0x8B));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x64, 0x74, 0x8B));
    app.setPalette(palette);

    QFont font = app.font();
    font.setFamilies({QStringLiteral("Microsoft YaHei UI"), QStringLiteral("Noto Sans CJK SC"),
                      QStringLiteral("Segoe UI"), QStringLiteral("Noto Sans")});
    font.setPointSizeF(9.5);
    app.setFont(font);

    app.setStyleSheet(styleSheet());
}

} // namespace ui