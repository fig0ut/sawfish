#pragma once
#include <QColor>
#include <QRectF>

class QPainter;

namespace Icons {
enum Name {
    Plus,
    More,
    Close,
    Pen,
    Save,
    Refresh,
    Pause,
    Play,
    Settings,
    Copy,
    Trash,
    ChevronDown,
    ChevronRight,
    Check
};
// Bundled Heroicons SVGs, tinted from the calling widget's current palette.
void paint(QPainter& painter, Name name, const QRectF& bounds, const QColor& color);
} // namespace Icons
