#pragma once
#include "backend/connectionstate.h"
#include <QAbstractTableModel>
#include <QSortFilterProxyModel>

class ConnectionModel : public QAbstractTableModel {
    Q_OBJECT
  public:
    enum Column {
        Host,
        Process,
        Network,
        Source,
        Destination,
        Route,
        UploadRate,
        DownloadRate,
        Upload,
        Download,
        Started,
        State,
        ColumnCount
    };
    enum { IdRole = Qt::UserRole + 1, SortRole, SearchRole, ActiveRole };
    using QAbstractTableModel::QAbstractTableModel;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    void replace(ConnectionList entries);
    ConnectionList snapshot() const {
        return m_entries;
    }
    const ConnectionInfo* find(const QString& id) const;
    int activeCount() const;

  private:
    ConnectionList m_entries;
};

class ConnectionFilter : public QSortFilterProxyModel {
    Q_OBJECT
  public:
    enum View { Active, Closed, All };
    explicit ConnectionFilter(QObject* parent = nullptr);
    void setView(View view);

  protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override;

  private:
    View m_view = Active;
};
