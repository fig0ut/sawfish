#include "connectionmodel.h"
#include "format.h"
#include <QDateTime>
#include <QFileInfo>
#include <QSet>

int ConnectionModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : m_entries.size();
}
int ConnectionModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}
QVariant ConnectionModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    const QStringList names = {tr("Host"),       tr("Process"),     tr("Network"),
                               tr("Source"),     tr("Destination"), tr("Route / rule"),
                               tr("Upload/s"),   tr("Download/s"),  tr("Uploaded"),
                               tr("Downloaded"), tr("Started"),     tr("State")};
    return names.value(section);
}
QVariant ConnectionModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= m_entries.size())
        return {};
    const auto& c = m_entries[index.row()];
    if (role == IdRole)
        return c.id;
    if (role == ActiveRole)
        return c.active();
    if (role == SearchRole)
        return QStringList{c.domain,
                           c.destination,
                           c.source,
                           c.processPath,
                           c.network,
                           c.protocol,
                           c.inbound,
                           c.outbound,
                           c.rule,
                           c.chain.join(' '),
                           c.packages.join(' '),
                           c.userName,
                           c.id}
            .join(' ');
    if (role == Qt::TextAlignmentRole && index.column() >= UploadRate && index.column() <= Download)
        return int(Qt::AlignRight | Qt::AlignVCenter);
    if (role != Qt::DisplayRole && role != Qt::ToolTipRole && role != SortRole)
        return {};
    if (index.column() >= UploadRate && index.column() <= Download) {
        const qint64 values[] = {c.uploadRate, c.downloadRate, c.upload, c.download};
        const auto value = values[index.column() - UploadRate];
        if (role == SortRole)
            return value;
        return formatBytes(value) + (index.column() <= DownloadRate ? "/s" : "");
    }
    switch (index.column()) {
    case Host:
        return c.domain.isEmpty() ? c.destination : c.domain;
    case Process:
        return role == Qt::ToolTipRole
                   ? c.processPath
                   : (c.processPath.isEmpty() ? c.packages.join(", ")
                                              : QFileInfo(c.processPath).fileName());
    case Network:
        return c.network.toUpper() + (c.protocol.isEmpty() ? "" : " / " + c.protocol);
    case Source:
        return c.source;
    case Destination:
        return c.destination;
    case Route:
        return (c.chain.isEmpty() ? c.outbound : c.chain.join(" → ")) +
               (c.rule.isEmpty() ? "" : " · " + c.rule);
    case Started:
        return role == SortRole ? QVariant(c.createdAt)
                                : QVariant(QDateTime::fromMSecsSinceEpoch(c.createdAt)
                                               .toString("yyyy-MM-dd HH:mm:ss"));
    case State:
        return c.active() ? tr("Active") : tr("Closed");
    default:
        return {};
    }
}
void ConnectionModel::replace(ConnectionList entries) {
    QSet<QString> retained;
    for (const auto& c : entries)
        retained.insert(c.id);
    for (int row = m_entries.size() - 1; row >= 0; --row) {
        if (retained.contains(m_entries[row].id))
            continue;
        beginRemoveRows({}, row, row);
        m_entries.removeAt(row);
        endRemoveRows();
    }
    QHash<QString, int> rows;
    for (int row = 0; row < m_entries.size(); ++row)
        rows.insert(m_entries[row].id, row);
    ConnectionList added;
    for (auto& c : entries) {
        if (rows.contains(c.id))
            m_entries[rows[c.id]] = std::move(c);
        else
            added.append(std::move(c));
    }
    if (!m_entries.isEmpty())
        emit dataChanged(index(0, 0), index(m_entries.size() - 1, ColumnCount - 1));
    if (!added.isEmpty()) {
        beginInsertRows({}, m_entries.size(), m_entries.size() + added.size() - 1);
        m_entries.append(added);
        endInsertRows();
    }
}
const ConnectionInfo* ConnectionModel::find(const QString& id) const {
    for (const auto& c : m_entries)
        if (c.id == id)
            return &c;
    return nullptr;
}
int ConnectionModel::activeCount() const {
    return std::count_if(m_entries.begin(), m_entries.end(),
                         [](const auto& c) { return c.active(); });
}
ConnectionFilter::ConnectionFilter(QObject* parent) : QSortFilterProxyModel(parent) {
    setFilterRole(ConnectionModel::SearchRole);
    setFilterCaseSensitivity(Qt::CaseInsensitive);
    setSortRole(ConnectionModel::SortRole);
    setSortCaseSensitivity(Qt::CaseInsensitive);
}
void ConnectionFilter::setView(View view) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
    m_view = view;
    endFilterChange(Direction::Rows);
#else
    m_view = view;
    invalidateFilter();
#endif
}
bool ConnectionFilter::filterAcceptsRow(int row, const QModelIndex& parent) const {
    const bool active =
        sourceModel()->index(row, 0, parent).data(ConnectionModel::ActiveRole).toBool();
    return (m_view == All || active == (m_view == Active)) &&
           QSortFilterProxyModel::filterAcceptsRow(row, parent);
}
