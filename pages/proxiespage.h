#pragma once
#include <QWidget>
#include <memory>
class Controller;
class ProxiesPage : public QWidget {
    Q_OBJECT
  public:
    explicit ProxiesPage(Controller* controller, QWidget* parent = nullptr);
    ~ProxiesPage() override;

  private:
    struct Ui;
    std::unique_ptr<Ui> m_ui;
};
