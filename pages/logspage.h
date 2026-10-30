#pragma once
#include <QWidget>
class Controller;
class LogPage : public QWidget {
    Q_OBJECT
  public:
    explicit LogPage(Controller* controller, QWidget* parent = nullptr);
};
