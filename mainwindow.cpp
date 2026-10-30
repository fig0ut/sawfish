#include "mainwindow.h"
#include "app/controller.h"
#include "pages/connectionspage.h"
#include "pages/homepage.h"
#include "pages/logspage.h"
#include "pages/profilespage.h"
#include "pages/proxiespage.h"
#include "pages/settingspage.h"
#include "widgets/applicationicon.h"
#include "widgets/controls.h"
#include "widgets/toast.h"
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QStackedWidget>
#include <QSystemTrayIcon>
#include <QToolButton>
#include <QVBoxLayout>

MainWindow::MainWindow(Controller* controller, QWidget* parent, std::function<bool()> trayAvailable)
    : QMainWindow(parent), m_controller(controller),
      m_trayAvailable(trayAvailable ? std::move(trayAvailable)
                                    : QSystemTrayIcon::isSystemTrayAvailable) {
    qApp->installEventFilter(this);
    setWindowTitle(tr("Sawfish — Rostra client"));
    updateIcon();
    auto* trayMenu = new RoundedMenu(this);
    auto* showAction = trayMenu->addAction(tr("Open Sawfish"));
    showAction->setObjectName("showWindowAction");
    connect(showAction, &QAction::triggered, this, &MainWindow::restoreWindow);
    m_connectionAction = trayMenu->addAction(tr("Start"));
    m_connectionAction->setObjectName("trayConnectionAction");
    connect(m_connectionAction, &QAction::triggered, controller, [controller] {
        if (controller->state() == Controller::Running ||
            controller->state() == Controller::Stopping)
            controller->disconnectService();
        else
            controller->connectService();
    });
    trayMenu->addSeparator();
    m_quitAction = trayMenu->addAction(tr("Quit Sawfish"));
    m_quitAction->setObjectName("quitAction");
    m_quitAction->setShortcut(QKeySequence::Quit);
    connect(m_quitAction, &QAction::triggered, this, &MainWindow::requestQuit);
    addAction(m_quitAction);
    m_tray = new QSystemTrayIcon(windowIcon(), this);
    m_tray->setContextMenu(trayMenu);
    m_tray->setToolTip(tr("Sawfish"));
    connect(m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
                    restoreWindow();
            });
    if (QSystemTrayIcon::isSystemTrayAvailable())
        m_tray->show();
    m_trayCheck.setInterval(2000);
    connect(&m_trayCheck, &QTimer::timeout, this, [this] {
        if (m_quitting)
            return;
        if (QSystemTrayIcon::isSystemTrayAvailable()) {
            if (!m_tray->isVisible())
                m_tray->show();
        } else if (!m_trayAvailable() && !isVisible()) {
            restoreWindow();
            m_controller->notify(
                tr("System tray unavailable. Sawfish remains accessible in the taskbar."));
        }
    });
    m_trayCheck.start();
    connect(controller, &Controller::shutdownFinished, this, [this] {
        m_trayCheck.stop();
        m_tray->hide();
        hide();
        emit quitReady();
    });
    resize(960, 700);
    setMinimumSize(800, 600);
    auto* central = new QWidget(this);
    auto* layout = new QHBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* sidebar = new QWidget;
    sidebar->setObjectName("sidebar");
    sidebar->setFixedWidth(190);
    auto* sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(16, 24, 16, 16);
    auto* brand = new QLabel(tr("Sawfish"));
    auto font = brand->font();
    font.setPointSize(20);
    font.setBold(true);
    brand->setFont(font);
    sideLayout->addWidget(brand);
    sideLayout->addWidget(new QLabel(tr("GUI for Rostra")));
    sideLayout->addSpacing(24);
    auto* navigation = new QListWidget;
    navigation->setObjectName("navigation");
    navigation->addItems(
        {tr("Home"), tr("Profiles"), tr("Proxies"), tr("Connections"), tr("Logs"), tr("Settings")});
    sideLayout->addWidget(navigation);
    auto* pages = new QStackedWidget;
    auto* home = new HomePage(controller);
    pages->addWidget(home);
    pages->addWidget(new ProfilePage(controller));
    pages->addWidget(new ProxiesPage(controller));
    pages->addWidget(new ConnectionsPage(controller));
    pages->addWidget(new LogPage(controller));
    pages->addWidget(new SettingPage(controller));
    connect(navigation, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);
    connect(pages, &QStackedWidget::currentChanged, home, [home, pages] {
        if (pages->currentWidget() != home)
            home->finishEditing();
    });
    navigation->setCurrentRow(0);
    layout->addWidget(sidebar);
    layout->addWidget(pages, 1);
    setCentralWidget(central);
    auto* toast = new Toast(central);
    connect(controller, &Controller::notification, toast, &Toast::present);
    auto refresh = [this, controller] {
        const bool active = controller->state() == Controller::Running ||
                            controller->state() == Controller::Stopping;
        m_tray->setToolTip(tr("Sawfish — %1").arg(controller->stateText()));
        m_connectionAction->setText(active ? tr("Stop") : tr("Start"));
        m_connectionAction->setEnabled(active ? controller->canDisconnect()
                                              : controller->canConnect());
        m_quitAction->setEnabled(!controller->shuttingDown());
        updateIcon();
    };
    connect(controller, &Controller::changed, this, refresh);
    refresh();
    setStyleSheet(R"(
        #sidebar { background: palette(alternate-base); }
        #navigation { background: transparent; border: none; outline: none; }
        #navigation::item { padding: 12px 10px; margin: 2px 0; border-radius: 5px; }
        #navigation::item:selected { background: palette(highlight); color: palette(highlighted-text); }
        QPushButton, QToolButton { padding: 7px 12px; color: palette(button-text);
            background: palette(button); border: 1px solid palette(mid); border-radius: 6px; }
        QPushButton:hover, QToolButton:hover { background: palette(midlight); }
        QPushButton:pressed, QToolButton:pressed, QPushButton:checked, QToolButton:checked {
            background: palette(highlight); color: palette(highlighted-text); }
        QPushButton:disabled, QToolButton:disabled { color: palette(mid); }
        QPushButton:focus, QToolButton:focus, QLineEdit:focus, QPlainTextEdit:focus, QComboBox:focus {
            border: 1px solid palette(highlight); }
        QLineEdit, QPlainTextEdit, QComboBox { color: palette(text); background: palette(base);
            selection-background-color: palette(highlight); selection-color: palette(highlighted-text);
            border: 1px solid palette(mid); border-radius: 5px; padding: 6px; }
        QScrollBar:vertical { width: 10px; margin: 0; border: none; background: transparent; }
        QScrollBar:horizontal { height: 10px; margin: 0; border: none; background: transparent; }
        QScrollBar::handle { background: palette(mid); border: none; border-radius: 3px; }
        QScrollBar::handle:vertical { min-height: 28px; margin: 2px; }
        QScrollBar::handle:horizontal { min-width: 28px; margin: 2px; }
        QScrollBar::handle:hover, QScrollBar::handle:pressed { background: palette(highlight); }
        QScrollBar::handle:disabled { background: transparent; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; border: none; }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; border: none; }
        QScrollBar::add-page, QScrollBar::sub-page { background: transparent; border: none; }
        QScrollBar::up-arrow, QScrollBar::down-arrow,
        QScrollBar::left-arrow, QScrollBar::right-arrow { image: none; width: 0; height: 0; }
        QAbstractScrollArea::corner { background: transparent; border: none; }
    )");
}

void MainWindow::closeEvent(QCloseEvent* event) {
    event->ignore();
    if (m_quitting)
        return;
    if (m_trayAvailable()) {
        hide();
    } else {
        showMinimized();
        m_controller->notify(tr(
            "System tray unavailable. Sawfish is minimized; use Quit Sawfish or Ctrl+Q to exit."));
    }
}

void MainWindow::restoreWindow() {
    if (isMinimized())
        setWindowState(windowState() & ~Qt::WindowMinimized);
    show();
    activateWindow();
}

void MainWindow::requestQuit() {
    if (m_quitting)
        return;
    m_quitting = true;
    m_trayCheck.stop();
    centralWidget()->setEnabled(false);
    m_controller->shutdown();
}

void MainWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::PaletteChange)
        updateIcon();
}

void MainWindow::updateIcon() {
    const auto state = m_controller->connectionChanging() ? ConnectionIconState::Working
                       : m_controller->online() && m_controller->state() == Controller::Running
                           ? ConnectionIconState::Running
                       : !m_controller->online() || m_controller->state() == Controller::Failed
                           ? ConnectionIconState::Unavailable
                           : ConnectionIconState::Idle;
    if (m_iconState == int(state) && m_iconPalette == palette().cacheKey())
        return;
    m_iconState = int(state);
    m_iconPalette = palette().cacheKey();
    setProperty("connectionIconState", m_iconState);
    setWindowIcon(applicationIcon(palette(), state));
    if (m_tray)
        m_tray->setIcon(windowIcon());
}

bool MainWindow::eventFilter(QObject* object, QEvent* event) {
    if (object == qApp && event->type() == QEvent::ApplicationPaletteChange) {
        // Qt resolves stylesheet palette colors during polishing. Reapply after
        // QApplication has finished propagating its new palette to the widgets.
        QTimer::singleShot(0, this, [this] {
            const auto sheet = styleSheet();
            setStyleSheet({});
            setPalette(QApplication::palette());
            setStyleSheet(sheet);
        });
    }
    return QMainWindow::eventFilter(object, event);
}
