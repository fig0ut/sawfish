#include "groupmodel.h"
#include <QDateTime>

QModelIndex GroupModel::index(int row, int column, const QModelIndex& parent) const {
    if (!hasIndex(row, column, parent))
        return {};
    return createIndex(row, column, parent.isValid() ? quintptr(parent.row() + 1) : quintptr(0));
}

QModelIndex GroupModel::parent(const QModelIndex& child) const {
    if (!child.isValid() || child.internalId() == 0)
        return {};
    return createIndex(int(child.internalId() - 1), 0, quintptr(0));
}

int GroupModel::rowCount(const QModelIndex& parent) const {
    if (!parent.isValid())
        return int(m_groups.size());
    if (parent.column() != 0 || parent.internalId() != 0 || parent.row() >= m_groups.size())
        return 0;
    return int(m_groups[parent.row()].members.size());
}

QVariant GroupModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid())
        return {};
    const bool member = index.internalId() != 0;
    const int groupRow = member ? int(index.internalId() - 1) : index.row();
    if (groupRow < 0 || groupRow >= m_groups.size())
        return {};
    const auto& group = m_groups[groupRow];
    if (member && (index.row() < 0 || index.row() >= group.members.size()))
        return {};
    const auto* item = member ? &group.members[index.row()] : nullptr;
    const bool selected = item && group.type != "loadbalance" && item->tag == group.selected;
    switch (role) {
    case GroupTagRole:
        return group.tag;
    case OutboundTagRole:
        return item ? item->tag : QString{};
    case SelectableRole:
        return group.selectable && group.type != "loadbalance";
    case SelectedRole:
        return selected;
    case Qt::AccessibleTextRole:
        return item ? item->tag : group.tag;
    case Qt::AccessibleDescriptionRole:
        if (item)
            return selected ? tr("%1, in use; press T to test delay").arg(item->type)
                   : group.selectable
                       ? tr("%1, press Enter to use or T to test delay").arg(item->type)
                       : tr("%1, managed automatically; press T to test delay").arg(item->type);
        return group.type;
    case Qt::ToolTipRole:
        if (item && index.column() == 3 && item->testTime > 0)
            return tr("Last test: %1")
                .arg(QDateTime::fromSecsSinceEpoch(item->testTime).toLocalTime().toString());
        return item ? item->tag : group.tag;
    case Qt::DisplayRole:
        switch (index.column()) {
        case 0:
            return item ? item->tag : group.tag;
        case 1:
            return item ? item->type : group.type;
        case 2:
            if (item)
                return selected ? tr("Selected") : QString{};
            if (group.type == "loadbalance")
                return tr("Per connection");
            if (!group.selectable)
                return group.selected.isEmpty() ? tr("Automatic")
                                                : tr("Automatic: %1").arg(group.selected);
            return group.selected.isEmpty() ? tr("No selection") : group.selected;
        case 3:
            if (!item)
                return {};
            if (item->testTime <= 0)
                return tr("—");
            return item->delay > 0 ? tr("%1 ms").arg(item->delay) : tr("Unavailable");
        }
    }
    return {};
}

QVariant GroupModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case 0:
        return tr("Group / outbound");
    case 1:
        return tr("Type");
    case 2:
        return tr("Selection");
    case 3:
        return tr("Last latency");
    }
    return {};
}

void GroupModel::replace(OutboundGroups groups) {
    beginResetModel();
    m_groups = std::move(groups);
    endResetModel();
}

const OutboundGroup* GroupModel::find(const QString& tag) const {
    for (const auto& group : m_groups)
        if (group.tag == tag)
            return &group;
    return nullptr;
}

QModelIndex GroupModel::findIndex(const QString& group, const QString& outbound) const {
    for (int row = 0; row < m_groups.size(); ++row) {
        if (m_groups[row].tag != group)
            continue;
        const auto root = index(row, 0);
        if (outbound.isEmpty())
            return root;
        for (int item = 0; item < m_groups[row].members.size(); ++item)
            if (m_groups[row].members[item].tag == outbound)
                return index(item, 0, root);
    }
    return {};
}
