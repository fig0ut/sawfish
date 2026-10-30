#pragma once
#include <QWidget>
class Controller;
class SettingPage : public QWidget {
    Q_OBJECT
  public:
    explicit SettingPage(Controller* controller, QWidget* parent = nullptr);
};
