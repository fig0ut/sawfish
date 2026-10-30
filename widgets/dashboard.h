#pragma once
#include <QVariantList>
#include <QWidget>
#include <memory>

class Controller;
class DashboardCard;
class Dashboard : public QWidget {
    Q_OBJECT
  public:
    // Keep persisted kind IDs stable when adding widgets.
    enum Kind { Upload, Download, Connections, TotalTraffic, Cpu, Memory, Traffic };
    explicit Dashboard(Controller* controller, QWidget* parent = nullptr);
    ~Dashboard() override;
    void setEditing(bool editing);
    bool editing() const;
    bool contains(Kind kind) const;
    void addWidget(Kind kind);
    void removeWidget(Kind kind);
    QVariantList savedLayout() const;
    static QString title(Kind kind);
  signals:
    void layoutChanged();

  protected:
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* object, QEvent* event) override;

  private:
    friend class DashboardCard;
    struct Ui;
    std::unique_ptr<Ui> m_ui;
    void arrange();
    void commit(DashboardCard* card, const QRect& geometry);
    void persist();
};
