#include "icons.h"
#include <QFile>
#include <QPainter>
#include <QPixmapCache>
#include <QSvgRenderer>
#include <QtMath>

// Explicit initialization also links resources when sawfish_widgets is static.
static void initializeIcons() {
    Q_INIT_RESOURCE(sawfish_icons);
}

void Icons::paint(QPainter& painter, Name name, const QRectF& bounds, const QColor& color) {
    static const bool initialized = [] {
        initializeIcons();
        return true;
    }();
    Q_UNUSED(initialized);
    static const char* const files[] = {"plus",          "ellipsis-vertical",
                                        "x-mark",        "pencil-square",
                                        "check",         "arrow-path",
                                        "pause",         "play",
                                        "cog-6-tooth",   "document-duplicate",
                                        "trash",         "chevron-down",
                                        "chevron-right", "check"};
    if (bounds.isEmpty())
        return;
    const qreal scale = painter.device()->devicePixelRatioF();
    const QSize pixels(qCeil(bounds.width() * scale), qCeil(bounds.height() * scale));
    const auto key = QStringLiteral("sawfish-heroicons/%1/%2/%3x%4/%5")
                         .arg(int(name))
                         .arg(color.rgba())
                         .arg(pixels.width())
                         .arg(pixels.height())
                         .arg(scale);
    QPixmap pixmap;
    if (!QPixmapCache::find(key, &pixmap)) {
        QFile source(QStringLiteral(":/icons/heroicons/%1.svg").arg(files[name]));
        if (!source.open(QIODevice::ReadOnly))
            return;
        auto svg = source.readAll();
        svg.replace("currentColor", color.name(QColor::HexRgb).toUtf8());
        QSvgRenderer renderer(svg);
        if (!renderer.isValid())
            return;
        pixmap = QPixmap(pixels);
        pixmap.setDevicePixelRatio(scale);
        pixmap.fill(Qt::transparent);
        QPainter iconPainter(&pixmap);
        iconPainter.setRenderHint(QPainter::Antialiasing);
        iconPainter.setOpacity(color.alphaF());
        renderer.render(&iconPainter, QRectF(QPointF(0, 0), bounds.size()));
        iconPainter.end();
        QPixmapCache::insert(key, pixmap);
    }
    painter.drawPixmap(bounds, pixmap, QRectF(QPointF(0, 0), pixmap.size()));
}
