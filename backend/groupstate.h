#pragma once
#include <QList>
#include <QMetaType>
#include <QString>

struct OutboundMember {
    QString tag, type;
    qint64 testTime = 0;
    int delay = 0;
};
struct OutboundGroup {
    QString tag, type, selected;
    bool selectable = false;
    QList<OutboundMember> members;
};
using OutboundGroups = QList<OutboundGroup>;
Q_DECLARE_METATYPE(OutboundGroups)
