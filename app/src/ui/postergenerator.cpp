#include "postergenerator.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QFontMetrics>

#include <cmath>

namespace ui {
namespace {

struct Palette
{
    QColor top;
    QColor bottom;
    QColor accent;
};

const Palette kPalettes[] = {
    {QColor(0x0F, 0x20, 0x27), QColor(0x2C, 0x53, 0x64), QColor(0x4F, 0xD1, 0xC5)},
    {QColor(0x1A, 0x10, 0x2E), QColor(0x4C, 0x1D, 0x95), QColor(0xF4, 0x72, 0xB6)},
    {QColor(0x1B, 0x1B, 0x1B), QColor(0x3D, 0x2B, 0x1F), QColor(0xF5, 0x9E, 0x0B)},
    {QColor(0x0B, 0x1F, 0x3A), QColor(0x1E, 0x3A, 0x8A), QColor(0x60, 0xA5, 0xFA)},
    {QColor(0x13, 0x2A, 0x1F), QColor(0x2D, 0x6A, 0x4F), QColor(0x34, 0xD3, 0x99)},
    {QColor(0x2A, 0x12, 0x12), QColor(0x7F, 0x1D, 0x1D), QColor(0xFB, 0x92, 0x3C)},
    {QColor(0x14, 0x16, 0x2B), QColor(0x3A, 0x2F, 0x6B), QColor(0xA7, 0x8B, 0xFA)},
    {QColor(0x0D, 0x1B, 0x24), QColor(0x24, 0x4B, 0x5F), QColor(0x38, 0xBD, 0xF8)},
};

Palette paletteFor(const QString &key)
{
    const uint h = qHash(key);
    return kPalettes[h % (sizeof(kPalettes) / sizeof(kPalettes[0]))];
}

quint32 seedFor(const QString &key)
{
    return qHash(key);
}

void drawMountains(QPainter &p, const QRectF &r, const QColor &tint, quint32 seed)
{
    for (int layer = 0; layer < 3; ++layer) {
        QColor c = tint;
        c.setAlpha(26 + layer * 16);
        QPainterPath path;
        path.moveTo(r.left(), r.bottom());
        const qreal baseY = r.bottom() - r.height() * (0.10 + layer * 0.08);
        path.lineTo(r.left(), baseY);
        const int peaks = 3 + layer;
        for (int i = 0; i < peaks; ++i) {
            const qreal peakX = r.left() + r.width() * (i + 0.55) / peaks;
            const qreal peakY = baseY - r.height() * (0.10 + ((seed >> ((layer * 5 + i) % 24)) & 0x7) * 0.016);
            const qreal valleyX = r.left() + r.width() * (i + 1.0) / peaks;
            path.lineTo(peakX, peakY);
            path.lineTo(valleyX, baseY - r.height() * 0.015);
        }
        path.lineTo(r.right(), r.bottom());
        path.closeSubpath();
        p.fillPath(path, c);
    }
}

void drawRings(QPainter &p, const QRectF &r, const QColor &tint, quint32 seed)
{
    p.setBrush(Qt::NoBrush);
    const QPointF center(r.left() + r.width() * (0.62 + (seed % 10) * 0.01),
                         r.top() + r.height() * (0.34 + (seed % 7) * 0.01));
    const qreal maxR = qMax(r.width(), r.height()) * 0.7;
    for (int i = 0; i < 7; ++i) {
        QColor c = tint;
        c.setAlpha(26 + i * 7);
        QPen pen(c);
        pen.setWidthF(qMax<qreal>(1.0, r.width() * 0.004));
        p.setPen(pen);
        p.drawEllipse(center, maxR * (0.14 + i * 0.11), maxR * (0.14 + i * 0.11));
    }
}

void drawBeams(QPainter &p, const QRectF &r, const QColor &tint, quint32 seed)
{
    for (int i = 0; i < 4; ++i) {
        QColor c = tint;
        c.setAlpha(20 + i * 8);
        QPainterPath path;
        const qreal x = r.left() - r.width() * 0.2 + r.width() * (0.1 + i * 0.16 + (seed % 5) * 0.01);
        path.moveTo(x, r.top());
        path.lineTo(x + r.width() * 0.18, r.top());
        path.lineTo(x + r.width() * 0.62, r.bottom());
        path.lineTo(x + r.width() * 0.44, r.bottom());
        path.closeSubpath();
        p.fillPath(path, c);
    }
}

void drawGrid(QPainter &p, const QRectF &r, const QColor &tint, quint32 seed)
{
    QColor c = tint;
    c.setAlpha(30);
    QPen pen(c);
    pen.setWidthF(qMax<qreal>(1.0, r.width() * 0.0035));
    p.setPen(pen);
    const qreal horizon = r.top() + r.height() * 0.52;
    const qreal vanishX = r.left() + r.width() * (0.4 + (seed % 20) * 0.01);
    for (int i = -6; i <= 6; ++i)
        p.drawLine(QPointF(vanishX, horizon), QPointF(vanishX + i * r.width() * 0.16, r.bottom()));
    for (int i = 1; i < 9; ++i) {
        const qreal t = static_cast<qreal>(i * i) / 81.0;
        const qreal y = horizon + (r.bottom() - horizon) * t;
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
    }
}

void drawGrain(QPainter &p, const QRectF &r, quint32 seed)
{
    p.setPen(Qt::NoPen);
    quint32 state = seed | 1u;
    const int count = qBound(80, int(r.width() * r.height() / 9000.0), 260);
    for (int i = 0; i < count; ++i) {
        state = state * 1664525u + 1013904223u;
        const qreal x = r.left() + (state % 10000) / 10000.0 * r.width();
        state = state * 1664525u + 1013904223u;
        const qreal y = r.top() + (state % 10000) / 10000.0 * r.height();
        state = state * 1664525u + 1013904223u;
        const int alpha = 6 + int((state >> 8) % 12);
        p.setBrush(QColor(255, 255, 255, alpha));
        p.drawEllipse(QPointF(x, y), 0.7, 0.7);
    }
}

void paintBase(QPainter &p, const QRectF &r, const QString &key)
{
    const Palette pal = paletteFor(key);
    QLinearGradient bg(r.topLeft(), r.bottomRight());
    bg.setColorAt(0.0, pal.top);
    bg.setColorAt(0.55, pal.bottom);
    bg.setColorAt(1.0, pal.top.darker(140));
    p.fillRect(r, bg);

    QRadialGradient glow(QPointF(r.left() + r.width() * 0.72, r.top() + r.height() * 0.18),
                         qMax(r.width(), r.height()) * 0.75);
    QColor glowColor = pal.accent;
    glowColor.setAlpha(95);
    glow.setColorAt(0.0, glowColor);
    glowColor.setAlpha(0);
    glow.setColorAt(1.0, glowColor);
    p.fillRect(r, glow);

    const quint32 seed = seedFor(key);
    switch (seed % 4) {
    case 0: drawMountains(p, r, pal.accent, seed); break;
    case 1: drawRings(p, r, pal.accent, seed); break;
    case 2: drawBeams(p, r, pal.accent, seed); break;
    default: drawGrid(p, r, pal.accent, seed); break;
    }

    drawGrain(p, r, seed);

    QRadialGradient vignette(QPointF(r.center().x(), r.center().y() * 0.95),
                             qMax(r.width(), r.height()) * 0.72);
    vignette.setColorAt(0.55, QColor(0, 0, 0, 0));
    vignette.setColorAt(1.0, QColor(0, 0, 0, 120));
    p.fillRect(r, vignette);
}

void drawPosterTitle(QPainter &p, const QRectF &r, const QString &title)
{
    if (title.isEmpty())
        return;

    QLinearGradient shade(r.left(), r.bottom() - r.height() * 0.42, r.left(), r.bottom());
    shade.setColorAt(0.0, QColor(0, 0, 0, 0));
    shade.setColorAt(1.0, QColor(0, 0, 0, 185));
    p.fillRect(QRectF(r.left(), r.bottom() - r.height() * 0.42, r.width(), r.height() * 0.42), shade);

    QFont font;
    font.setFamilies({QStringLiteral("Microsoft YaHei UI"), QStringLiteral("Noto Sans CJK SC"),
                      QStringLiteral("Noto Serif CJK SC"), QStringLiteral("Segoe UI")});
    font.setPixelSize(qMax(9, qRound(r.width() * 0.082)));
    font.setWeight(QFont::DemiBold);
    p.setFont(font);

    const QRectF textRect = r.adjusted(r.width() * 0.08, 0, -r.width() * 0.08, -r.height() * 0.065);
    const QRectF titleRect(textRect.left(), textRect.bottom() - r.height() * 0.32,
                           textRect.width(), r.height() * 0.32);

    p.setPen(QColor(0, 0, 0, 160));
    p.drawText(titleRect.translated(0, 1), Qt::AlignLeft | Qt::AlignBottom | Qt::TextWordWrap, title);
    p.setPen(QColor(248, 250, 252, 235));
    p.drawText(titleRect, Qt::AlignLeft | Qt::AlignBottom | Qt::TextWordWrap, title);
}

} // namespace

QPixmap makePosterArt(const QString &key, const QSize &size, const QString &title)
{
    if (size.isEmpty())
        return {};
    QPixmap pm(size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF r(0, 0, size.width(), size.height());
    paintBase(p, r, key);
    drawPosterTitle(p, r, title);
    p.end();
    return pm;
}

QPixmap makeBackdropArt(const QString &key, const QSize &size, const QString &title)
{
    if (size.isEmpty())
        return {};
    QPixmap pm(size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF r(0, 0, size.width(), size.height());
    paintBase(p, r, key);
    if (!title.isEmpty())
        drawPosterTitle(p, r, title);
    p.end();
    return pm;
}

} // namespace ui
