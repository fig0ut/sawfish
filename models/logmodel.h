#pragma once
#include "backend/rpcclient.h"
#include <QAbstractListModel>
#include <QSortFilterProxyModel>

class LogModel : public QAbstractListModel {
    Q_OBJECT
  public:
    static constexpr int MaxLines = 3000;
    static constexpr int MinimumRetention = 100, MaximumRetention = 100000;
    enum { LevelRole = Qt::UserRole + 1, MessageRole };
    explicit LogModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    void append(LogBatch lines, bool reset = false);
    void appendApplication(const QString& message, bool error = false);
    void clear();
    void setPaused(bool paused);
    int retentionLimit() const {
        return m_retention;
    }
    void setRetentionLimit(int lines);
    static QString levelName(int level);
  signals:
    void retentionChanged(int lines);

  private:
    void bound(LogBatch& lines) const;
    LogBatch m_lines, m_pending;
    bool m_paused = false, m_pendingReset = false;
    int m_retention = MaxLines;
};
class LogFilter : public QSortFilterProxyModel {
    Q_OBJECT
  public:
    explicit LogFilter(QObject* parent = nullptr);
    void setMaximumLevel(int level);

  protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override;

  private:
    int m_maxLevel = 6;
};
