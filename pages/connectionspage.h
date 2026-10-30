#pragma once
#include <QWidget>
#include <memory>
class Controller;
class ConnectionsPage : public QWidget {
    Q_OBJECT
  public:
    explicit ConnectionsPage(Controller* controller, QWidget* parent = nullptr);
    ~ConnectionsPage() override;

  protected:
    void hideEvent(QHideEvent* event) override;

  private:
    struct Ui;
    std::unique_ptr<Ui> m_ui;
};
