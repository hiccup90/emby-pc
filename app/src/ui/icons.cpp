#include "icons.h"

#include <QPainter>
#include <QPainterPath>

#include <cmath>

namespace ui {
namespace {

void paintGlyph(QPainter &p, Icon kind, const QColor &color)
{
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(color, 1.9, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    switch (kind) {
    case Icon::Home: {
        p.drawPolyline(QPolygonF({QPointF(3.5, 11), QPointF(12, 4.2), QPointF(20.5, 11)}));
        QPainterPath body;
        body.moveTo(5.8, 9.6);
        body.lineTo(5.8, 19.8);
        body.lineTo(18.2, 19.8);
        body.lineTo(18.2, 9.6);
        p.drawPath(body);
        p.drawRect(QRectF(10.2, 14.5, 3.6, 5.3));
        break;
    }
    case Icon::Movie: {
        p.drawRoundedRect(QRectF(3.5, 5, 17, 14), 2, 2);
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        for (int y : {7, 15})
            for (int x : {5, 18})
                p.drawRoundedRect(QRectF(x - 0.9, y - 1, 1.8, 2), 0.6, 0.6);
        break;
    }
    case Icon::Tv: {
        p.drawRoundedRect(QRectF(3, 5, 18, 12), 2, 2);
        p.drawLine(QPointF(12, 17), QPointF(12, 20));
        p.drawLine(QPointF(8.5, 20), QPointF(15.5, 20));
        break;
    }
    case Icon::Settings: {
        p.drawEllipse(QPointF(12, 12), 3.1, 3.1);
        constexpr double kPi = 3.14159265358979323846;
        for (int i = 0; i < 8; ++i) {
            const double angle = i * kPi / 4.0;
            const QPointF a(12 + std::cos(angle) * 6.2, 12 + std::sin(angle) * 6.2);
            const QPointF b(12 + std::cos(angle) * 8.2, 12 + std::sin(angle) * 8.2);
            p.drawLine(a, b);
        }
        break;
    }
    case Icon::Search: {
        p.drawEllipse(QPointF(10.5, 10.5), 6.0, 6.0);
        p.drawLine(QPointF(15.0, 15.0), QPointF(20.0, 20.0));
        break;
    }
    case Icon::User: {
        p.drawEllipse(QPointF(12, 8), 3.6, 3.6);
        QPainterPath shoulders;
        shoulders.moveTo(5.3, 20.2);
        shoulders.cubicTo(5.8, 15.6, 8.6, 13.9, 12, 13.9);
        shoulders.cubicTo(15.4, 13.9, 18.2, 15.6, 18.7, 20.2);
        p.drawPath(shoulders);
        break;
    }
    case Icon::Play: {
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        QPainterPath tri;
        tri.moveTo(8, 5);
        tri.lineTo(19, 12);
        tri.lineTo(8, 19);
        tri.closeSubpath();
        p.drawPath(tri);
        break;
    }
    case Icon::Pause: {
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(QRectF(7, 5, 3.4, 14), 1, 1);
        p.drawRoundedRect(QRectF(13.6, 5, 3.4, 14), 1, 1);
        break;
    }
    case Icon::Prev: {
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(QRectF(4.6, 5, 2.2, 14), 1, 1);
        QPainterPath tri;
        tri.moveTo(18.5, 5);
        tri.lineTo(7.5, 12);
        tri.lineTo(18.5, 19);
        tri.closeSubpath();
        p.drawPath(tri);
        break;
    }
    case Icon::Next: {
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(QRectF(17.2, 5, 2.2, 14), 1, 1);
        QPainterPath tri;
        tri.moveTo(5.5, 5);
        tri.lineTo(16.5, 12);
        tri.lineTo(5.5, 19);
        tri.closeSubpath();
        p.drawPath(tri);
        break;
    }
    case Icon::Back: {
        p.drawLine(QPointF(19.5, 12), QPointF(5.5, 12));
        p.drawPolyline(QPolygonF({QPointF(11.5, 6), QPointF(5.5, 12), QPointF(11.5, 18)}));
        break;
    }
    case Icon::Subtitle: {
        p.drawRoundedRect(QRectF(3, 5.5, 18, 13), 2, 2);
        p.drawLine(QPointF(6.2, 11), QPointF(11.2, 11));
        p.drawLine(QPointF(13.2, 11), QPointF(17.8, 11));
        p.drawLine(QPointF(6.2, 15), QPointF(9.6, 15));
        p.drawLine(QPointF(11.6, 15), QPointF(17.8, 15));
        break;
    }
    case Icon::Audio:
    case Icon::Volume: {
        QPainterPath speaker;
        speaker.moveTo(4, 9.8);
        speaker.lineTo(7.6, 9.8);
        speaker.lineTo(11.6, 6.2);
        speaker.lineTo(11.6, 17.8);
        speaker.lineTo(7.6, 14.2);
        speaker.lineTo(4, 14.2);
        speaker.closeSubpath();
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawPath(speaker);
        p.setBrush(Qt::NoBrush);
        p.setPen(pen);
        p.drawArc(QRectF(11.8, 8.2, 6.4, 7.6), -55 * 16, 110 * 16);
        if (kind == Icon::Audio)
            p.drawArc(QRectF(11.8, 5.6, 9.6, 12.8), -50 * 16, 100 * 16);
        break;
    }
    case Icon::Stats: {
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(QRectF(4.6, 13.2, 3.2, 6.2), 1, 1);
        p.drawRoundedRect(QRectF(10.4, 9.0, 3.2, 10.4), 1, 1);
        p.drawRoundedRect(QRectF(16.2, 4.8, 3.2, 14.6), 1, 1);
        break;
    }
    case Icon::Fullscreen: {
        p.drawPolyline(QPolygonF({QPointF(4, 9), QPointF(4, 4), QPointF(9, 4)}));
        p.drawPolyline(QPolygonF({QPointF(15, 4), QPointF(20, 4), QPointF(20, 9)}));
        p.drawPolyline(QPolygonF({QPointF(20, 15), QPointF(20, 20), QPointF(15, 20)}));
        p.drawPolyline(QPolygonF({QPointF(9, 20), QPointF(4, 20), QPointF(4, 15)}));
        break;
    }
    case Icon::ExitFullscreen: {
        p.drawPolyline(QPolygonF({QPointF(9, 4), QPointF(9, 9), QPointF(4, 9)}));
        p.drawPolyline(QPolygonF({QPointF(15, 4), QPointF(15, 9), QPointF(20, 9)}));
        p.drawPolyline(QPolygonF({QPointF(20, 15), QPointF(15, 15), QPointF(15, 20)}));
        p.drawPolyline(QPolygonF({QPointF(4, 15), QPointF(9, 15), QPointF(9, 20)}));
        break;
    }
    case Icon::Heart: {
        QPainterPath heart;
        heart.moveTo(12, 20);
        heart.cubicTo(8, 16.4, 4, 13.4, 4, 9.6);
        heart.cubicTo(4, 6.6, 6.4, 4.6, 9, 4.6);
        heart.cubicTo(10.5, 4.6, 11.5, 5.3, 12, 6.1);
        heart.cubicTo(12.5, 5.3, 13.5, 4.6, 15, 4.6);
        heart.cubicTo(17.6, 4.6, 20, 6.6, 20, 9.6);
        heart.cubicTo(20, 13.4, 16, 16.4, 12, 20);
        p.drawPath(heart);
        break;
    }
    case Icon::More: {
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(6, 12), 1.7, 1.7);
        p.drawEllipse(QPointF(12, 12), 1.7, 1.7);
        p.drawEllipse(QPointF(18, 12), 1.7, 1.7);
        break;
    }
    case Icon::Grid: {
        p.drawRoundedRect(QRectF(4, 4, 7, 7), 1.4, 1.4);
        p.drawRoundedRect(QRectF(13, 4, 7, 7), 1.4, 1.4);
        p.drawRoundedRect(QRectF(4, 13, 7, 7), 1.4, 1.4);
        p.drawRoundedRect(QRectF(13, 13, 7, 7), 1.4, 1.4);
        break;
    }
    case Icon::Check: {
        p.drawPolyline(QPolygonF({QPointF(4.8, 12.4), QPointF(9.8, 17.4), QPointF(19.2, 6.8)}));
        break;
    }
    case Icon::Clock: {
        p.drawEllipse(QPointF(12, 12), 8.2, 8.2);
        p.drawLine(QPointF(12, 7.2), QPointF(12, 12));
        p.drawLine(QPointF(12, 12), QPointF(15.6, 14.2));
        break;
    }
    case Icon::Server: {
        p.drawRoundedRect(QRectF(4, 5, 16, 5.2), 1.4, 1.4);
        p.drawRoundedRect(QRectF(4, 13.8, 16, 5.2), 1.4, 1.4);
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(7, 7.6), 0.85, 0.85);
        p.drawEllipse(QPointF(7, 16.4), 0.85, 0.85);
        break;
    }
    }
}

} // namespace

QPixmap iconPixmap(Icon kind, const QColor &color, int size)
{
    const qreal dpr = 2.0;
    QPixmap pm(qRound(size * dpr), qRound(size * dpr));
    pm.fill(Qt::transparent);
    pm.setDevicePixelRatio(dpr);

    QPainter p(&pm);
    p.scale(size / 24.0, size / 24.0);
    paintGlyph(p, kind, color);
    p.end();
    return pm;
}

QIcon icon(Icon kind, const QColor &color, int size)
{
    return QIcon(iconPixmap(kind, color, size));
}

} // namespace ui
