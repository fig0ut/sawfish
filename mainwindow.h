#pragma once
#include <QMainWindow>
#include <QTimer>
#include <functional>
class Controller;
class QCloseEvent;
class QSystemTrayIcon;
class QAction;
class MainWindow : public QMainWindow {
    Q_OBJECT
  public:
    explicit MainWindow(Controller* controller, QWidget* parent = nullptr,
                        std::function<bool()> trayAvailable = {});
    void restoreWindow();
    void requestQuit();
  signals:
    void quitReady();

  protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* object, QEvent* event) override;

  private:
    void updateIcon();
    Controller* m_controller;
    QSystemTrayIcon* m_tray = nullptr;
    QAction *m_connectionAction, *m_quitAction;
    QTimer m_trayCheck;
    std::function<bool()> m_trayAvailable;
    bool m_quitting = false;
    int m_iconState = -1;
    qint64 m_iconPalette = -1;
};
