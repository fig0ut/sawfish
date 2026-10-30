#pragma once
#include <QWidget>
class Controller;
class SymbolButton;
class HomePage : public QWidget {
    Q_OBJECT
  public:
    explicit HomePage(Controller* controller, QWidget* parent = nullptr);
    void finishEditing();

  private:
    SymbolButton* m_edit;
};
