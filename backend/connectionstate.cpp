#include "connectionstate.h"
#include "daemon/started_service.pb.h"
#include <QDateTime>
#include <limits>

void ConnectionHistory::apply(const rostra_rpc::ConnectionEvents& events) {
    if (events.reset()) {
        m_entries.clear();
        m_order.clear();
    }
    auto text = [](const std::string& value) { return QString::fromStdString(value).left(2048); };
    for (const auto& event : events.events()) {
        const auto id = text(event.id());
        if (id.isEmpty())
            continue;
        if (event.has_connection()) {
            const auto& value = event.connection();
            ConnectionInfo entry;
            entry.id = id;
            entry.inbound = text(value.inbound());
            entry.inboundType = text(value.inboundtype());
            entry.network = text(value.network());
            entry.source = text(value.source());
            entry.destination = text(value.destination());
            entry.domain = text(value.domain());
            entry.protocol = text(value.protocol());
            entry.user = text(value.user());
            entry.outbound = text(value.outbound());
            entry.outboundType = text(value.outboundtype());
            entry.rule = text(value.rule());
            entry.createdAt = value.createdat();
            entry.closedAt = qMax<qint64>(0, value.closedat());
            entry.upload = qMax<qint64>(0, value.uplinktotal());
            entry.download = qMax<qint64>(0, value.downlinktotal());
            for (const auto& link : value.chainlist()) {
                if (entry.chain.size() == 32)
                    break;
                entry.chain.append(text(link));
            }
            if (value.has_processinfo()) {
                const auto& process = value.processinfo();
                entry.processId = process.processid();
                entry.processPath = text(process.processpath());
                entry.userName = text(process.username());
                for (const auto& name : process.packagenames()) {
                    if (entry.packages.size() == 32)
                        break;
                    entry.packages.append(text(name));
                }
            }
            if (!m_entries.contains(id))
                m_order.append(id);
            m_entries.insert(id, entry);
        }
        auto it = m_entries.find(id);
        if (it == m_entries.end())
            continue;
        if (event.type() == rostra_rpc::CONNECTION_EVENT_UPDATE && it->active()) {
            it->uploadRate = qMax<qint64>(0, event.uplinkdelta());
            it->downloadRate = qMax<qint64>(0, event.downlinkdelta());
            auto add = [](qint64 total, qint64 delta) {
                return total + qMin(delta, std::numeric_limits<qint64>::max() - total);
            };
            it->upload = add(it->upload, it->uploadRate);
            it->download = add(it->download, it->downloadRate);
        } else if (event.type() == rostra_rpc::CONNECTION_EVENT_CLOSED) {
            it->closedAt =
                event.closedat() > 0 ? event.closedat() : QDateTime::currentMSecsSinceEpoch();
            it->uploadRate = it->downloadRate = 0;
        }
    }
    // Retain active connections preferentially, with a strict memory bound.
    int excess = m_order.size() - MaxEntries;
    if (excess > 0) {
        m_order.removeIf([&](const QString& id) {
            if (excess > 0 && !m_entries[id].active()) {
                m_entries.remove(id);
                --excess;
                return true;
            }
            return false;
        });
        while (excess-- > 0)
            m_entries.remove(m_order.takeFirst());
    }
}

ConnectionList ConnectionHistory::snapshot() const {
    ConnectionList result;
    result.reserve(m_order.size());
    for (const auto& id : m_order)
        result.append(m_entries.value(id));
    return result;
}
