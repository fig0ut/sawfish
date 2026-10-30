#pragma once
#include "backend/groupstate.h"
#include <QAbstractItemModel>

class GroupModel : public QAbstractItemModel {
    Q_OBJECT
  public:
    enum Role { GroupTagRole = Qt::UserRole, OutboundTagRole, SelectableRole, SelectedRole };
    explicit GroupModel(QObject* parent = nullptr) : QAbstractItemModel(parent) {}
    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& index) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& = {}) const override {
        return 4;
    }
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    void replace(OutboundGroups groups);
    const OutboundGroup* find(const QString& tag) const;
    QModelIndex findIndex(const QString& group, const QString& outbound = {}) const;

  private:
    OutboundGroups m_groups;
};
