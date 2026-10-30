#pragma once
#include "icons.h"
#include <QAbstractButton>
#include <QComboBox>
#include <QMenu>
#include <QToolButton>

// Library SVG icons use the current palette for normal, checked and disabled states.
class SymbolButton : public QToolButton {
  public:
    enum Symbol {
        Plus = Icons::Plus,
        More = Icons::More,
        Close = Icons::Close,
        Pen = Icons::Pen,
        Save = Icons::Save,
        Refresh = Icons::Refresh,
        Pause = Icons::Pause,
        Play = Icons::Play,
        Settings = Icons::Settings,
        Copy = Icons::Copy,
        Trash = Icons::Trash
    };
    explicit SymbolButton(Symbol symbol, QWidget* parent = nullptr);
    void setSymbol(Symbol symbol);

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    Symbol m_symbol;
};

class ConnectionSwitch : public QAbstractButton {
  public:
    explicit ConnectionSwitch(QWidget* parent = nullptr);
    QSize sizeHint() const override {
        return {92, 48};
    }

  protected:
    void paintEvent(QPaintEvent*) override;
};

class RoundedMenu : public QMenu {
  public:
    explicit RoundedMenu(QWidget* parent = nullptr);
};

// Draw only the arrow ourselves, so a platform style cannot add a square button.
class ThemedComboBox : public QComboBox {
  public:
    explicit ThemedComboBox(QWidget* parent = nullptr);

  protected:
    void paintEvent(QPaintEvent* event) override;
};
