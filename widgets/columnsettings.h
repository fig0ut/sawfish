#pragma once
#include <QDialog>

class QHeaderView;
class ColumnSettings : public QDialog {
    Q_OBJECT
  public:
    ColumnSettings(QHeaderView* header, const QByteArray& defaults, const QList<int>& defaultOrder,
                   QWidget* parent = nullptr);
  signals:
    void applied();
};
