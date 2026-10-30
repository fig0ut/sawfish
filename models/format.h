#pragma once
#include <QStringList>

inline QString formatBytes(double value) {
    static const QStringList units = {"B", "KiB", "MiB", "GiB", "TiB"};
    int unit = 0;
    while (value >= 1024 && unit < units.size() - 1) {
        value /= 1024;
        ++unit;
    }
    return QStringLiteral("%1 %2").arg(value, 0, 'f', unit ? 1 : 0).arg(units[unit]);
}
