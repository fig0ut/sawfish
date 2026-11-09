#include "app/controller.h"
#include "backend/coreupdater.h"
#include "backend/proxysettings.h"
#include "mainwindow.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QMessageBox>
#include <QStandardPaths>

int main(int argc, char* argv[]) {
#ifdef Q_OS_LINUX
    if (argc > 1 && QByteArray(argv[1]) == "--system-proxy-helper") {
        QCoreApplication helper(argc, argv);
        return runSystemProxyHelper();
    }
#endif
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName("Sawfish");
    QCoreApplication::setApplicationName("Sawfish");
    QGuiApplication::setDesktopFileName("io.github.fig0ut.sawfish");
    QCoreApplication::setApplicationVersion(QStringLiteral(SAWFISH_VERSION));
    QCommandLineParser parser;
    parser.setApplicationDescription("Desktop client for Rostra");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption(
        {"backend", "Rostra desktop daemon executable (also supplies the worker).", "path"});
    parser.addOption({"data-dir", "Profile and preference directory.", "path"});
    parser.process(app);
    const auto directory = parser.isSet("data-dir")
                               ? parser.value("data-dir")
                               : QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!QDir().mkpath(directory)) {
        QMessageBox::critical(nullptr, "Sawfish", "Cannot create the application data directory.");
        return 1;
    }
    QLockFile lock(directory + "/sawfish.lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock()) {
        QMessageBox::information(
            nullptr, "Sawfish",
            "Sawfish is already using this data directory, or it is not writable.");
        return 1;
    }
    auto executable = parser.value("backend");
    QSettings settings(directory + "/settings.ini", QSettings::IniFormat);
    CoreUpdater::recoverInterrupted(settings);
    if (executable.isEmpty()) {
        executable = settings.value("backend/executable").toString();
        if (executable.isEmpty()) {
            const auto name = QStringLiteral("rostra-daemon")
#ifdef Q_OS_WIN
                              + ".exe"
#endif
                ;
            const auto bundled = QCoreApplication::applicationDirPath() + "/" + name;
            executable =
                QFileInfo(bundled).isExecutable() ? bundled : QStandardPaths::findExecutable(name);
        }
    }
    Controller controller(directory, executable, {});
    controller.initializeSystemProxy();
    MainWindow window(&controller);
    QObject::connect(&window, &MainWindow::quitReady, &app, &QCoreApplication::quit);
    window.show();
    QTimer::singleShot(0, &controller, &Controller::initialize);
    return app.exec();
}
