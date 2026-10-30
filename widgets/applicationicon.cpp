#include "applicationicon.h"
#include <QPainter>
#include <QPainterPath>

QIcon applicationIcon(const QPalette& palette, ConnectionIconState state) {
    QIcon icon;
    // Render each common tray size directly so the silhouette and status mark
    // stay sharp on small panels and on high-DPI displays.
    for (const int size : {16, 20, 22, 24, 32, 48, 64, 128}) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter p(&pixmap);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(size / 64.0, size / 64.0);
        const bool running = state == ConnectionIconState::Running;
        const auto background = palette.color(running ? QPalette::Highlight : QPalette::Button);
        const auto foreground =
            palette.color(running ? QPalette::HighlightedText : QPalette::ButtonText);
        p.setPen(QPen(palette.color(running ? QPalette::Highlight : QPalette::Mid), 2));
        p.setBrush(background);
        p.drawRoundedRect(QRectF(3, 3, 58, 58), 15, 15);
        p.setPen(Qt::NoPen);
        p.setBrush(foreground);
        p.drawEllipse(QRectF(12, 19, 32, 23));
        p.drawPolygon(QPolygonF({QPointF(40, 30), QPointF(53, 20), QPointF(53, 41)}));
        p.setBrush(background);
        p.drawEllipse(QPointF(21, 27), 2.5, 2.5);
        if (running) {
            p.setPen(QPen(foreground, 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            //p.drawPolyline(QPolygonF({QPointF(27, 49), QPointF(32, 54), QPointF(43, 45)}));
            QPainterPath path;
            path.moveTo(16, 52);

            path.cubicTo(20, 44, 28, 44, 32, 50);
            path.cubicTo(36, 56, 44, 56, 48, 48);

            p.drawPath(path);
        } else if (state == ConnectionIconState::Working) {
            p.setBrush(palette.color(QPalette::Highlight));
            for (int x : {23, 32, 41})
                p.drawEllipse(QPointF(x, 51), 2.5, 2.5);
        } else if (state == ConnectionIconState::Unavailable) {
            p.setPen(QPen(foreground, 3, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(27, 46), QPointF(37, 54));
            p.drawLine(QPointF(37, 46), QPointF(27, 54));
        } else {
            p.setPen(QPen(foreground, 3, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(26, 50), QPointF(38, 50));
        }
        p.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}
