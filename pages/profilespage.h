#pragma once
#include <QWidget>
#include <memory>
class Controller;
class ProfilePage : public QWidget {
    Q_OBJECT
  public:
    explicit ProfilePage(Controller* controller, QWidget* parent = nullptr);
    ~ProfilePage() override;

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    struct Ui;
    std::unique_ptr<Ui> m_ui;
};
