#pragma once
#include <QHash>
#include <QList>
#include <QMetaType>
#include <QStringList>

namespace rostra_rpc {
class ConnectionEvents;
}
struct ConnectionInfo {
    QString id, inbound, inboundType, network, source, destination, domain, protocol;
    QString user, outbound, outboundType, rule, processPath, userName;
    QStringList chain, packages;
    qint64 createdAt = 0, closedAt = 0, upload = 0, download = 0;
    qint64 uploadRate = 0, downloadRate = 0;
    quint32 processId = 0;
    bool active() const {
        return closedAt == 0;
    }
};
using ConnectionList = QList<ConnectionInfo>;
Q_DECLARE_METATYPE(ConnectionList)

// Apply every delta on the receiving thread before coalescing GUI snapshots.
class ConnectionHistory {
  public:
    static constexpr int MaxEntries = 10000;
    void apply(const rostra_rpc::ConnectionEvents& events);
    ConnectionList snapshot() const;

  private:
    QHash<QString, ConnectionInfo> m_entries;
    QStringList m_order;
};
