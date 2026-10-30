#include "app/controller.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QtTest>
#include <cstdio>
#ifdef Q_OS_LINUX
#include <csignal>
#include <unistd.h>
#endif

namespace {
QString backendBinary() {
    return qEnvironmentVariable("SAWFISH_TEST_BACKEND",
                                QCoreApplication::applicationDirPath() + "/rostra-daemon");
}
bool processAlive(qint64 pid) {
    QFile state(QString("/proc/%1/stat").arg(pid));
    if (!state.open(QIODevice::ReadOnly))
        return false;
    const auto value = state.readAll();
    const auto end = value.lastIndexOf(')');
    return end >= 0 && value.mid(end + 2, 1) != "Z";
}
} // namespace

class LifecycleTest : public QObject {
    Q_OBJECT
  private slots:
    void ownedCoreStartsAndQuits() {
        QTemporaryDir directory;
        Controller controller(directory.path(), backendBinary(), {});
        QVERIFY(controller.managedBackend());
        controller.initialize();
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), Controller::Idle, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(controller.workerStatus(), "Ready", 10000);
        const auto pid = controller.coreProcessId();
        const auto socket = controller.socket();
        QVERIFY(pid > 0);
        QVERIFY(processAlive(pid));
        QTcpServer reserve;
        QVERIFY(reserve.listen(QHostAddress::LocalHost, 0));
        const auto port = reserve.serverPort();
        reserve.close();
        QFile profile(directory.filePath("profile.json"));
        QVERIFY(profile.open(QIODevice::WriteOnly));
        profile.write(
            QString(
                R"({"inbounds":[{"type":"mixed","listen":"127.0.0.1","listen_port":%1}],"outbounds":[{"type":"direct"}]})")
                .arg(port)
                .toUtf8());
        profile.close();
        QVERIFY(controller.importProfile(profile.fileName()));
        controller.connectService();
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), Controller::Running, 10000);
        QTRY_VERIFY(!controller.busy());
#ifdef Q_OS_LINUX
        QTRY_VERIFY(controller.trafficSample().cpuAvailable);
        QVERIFY(controller.trafficSample().cpuPercent >= 0);
#endif
        QTRY_VERIFY(controller.trafficSample().memory > 0);
        QSignalSpy stopped(&controller, &Controller::shutdownFinished);
        controller.shutdown();
        controller.retryNow();
        controller.connectService();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.size(), 1, 10000);
        QCOMPARE(controller.coreProcessId(), 0);
        QVERIFY(!controller.trafficSample().cpuAvailable);
        QCOMPARE(controller.trafficSample().memory, 0);
        QVERIFY(!processAlive(pid));
        QVERIFY(!QFileInfo::exists(socket));
        QVERIFY(reserve.listen(QHostAddress::LocalHost, port));
        Controller reopened(directory.path(), backendBinary(), {});
        reopened.initialize();
        QTRY_COMPARE_WITH_TIMEOUT(reopened.state(), Controller::Idle, 10000);
    }
    void quitDuringStartup() {
        QTemporaryDir directory;
        Controller controller(directory.path(), backendBinary(), {});
        QSignalSpy stopped(&controller, &Controller::shutdownFinished);
        controller.initialize();
        controller.shutdown();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.size(), 1, 10000);
        QCOMPARE(controller.coreProcessId(), 0);
        QVERIFY(!controller.online());
    }
    void destructionReapsOwnedCore() {
        QTemporaryDir directory;
        auto controller =
            std::make_unique<Controller>(directory.path(), backendBinary(), QString{});
        controller->initialize();
        QTRY_VERIFY_WITH_TIMEOUT(controller->online(), 10000);
        const auto pid = controller->coreProcessId();
        controller.reset();
        QVERIFY(!processAlive(pid));
    }
    void duplicateOwnerCannotLaunchCore() {
        QTemporaryDir directory;
        DaemonProcess first(directory.path()), second(directory.path());
        first.start(backendBinary());
        QTRY_VERIFY(first.processId() > 0);
        const auto pid = first.processId();
        QSignalSpy failed(&second, &DaemonProcess::unavailable);
        second.start(backendBinary());
        QCOMPARE(failed.size(), 1);
        QVERIFY(failed.first()[0].toString().contains("Another Sawfish"));
        QCOMPARE(second.processId(), 0);
        QVERIFY(processAlive(pid));
    }
    void unresponsiveCoreIsReapedOnStop() {
        QTemporaryDir directory;
        DaemonProcess core(directory.path());
        core.start(QCoreApplication::applicationFilePath());
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(directory.filePath("unresponsive.ready")), 5000);
        const auto pid = core.processId();
        QVERIFY(pid > 0);
        QSignalSpy stopped(&core, &DaemonProcess::stopped);
        core.stop();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.size(), 1, 7000);
        QVERIFY(!processAlive(pid));
    }
    void failedExecutableIsReported() {
        QTemporaryDir directory;
        Controller controller(directory.path(), "/missing/rostra-daemon", {});
        controller.initialize();
        QVERIFY(controller.messageIsError());
        QVERIFY(controller.message().contains("Rostra is missing"));
        QCOMPARE(controller.coreProcessId(), 0);
        QSignalSpy stopped(&controller, &Controller::shutdownFinished);
        controller.shutdown();
        QTRY_COMPARE(stopped.size(), 1);
    }
    void coreCrashRecoversWhileSawfishLives() {
        QTemporaryDir directory;
        Controller controller(directory.path(), backendBinary(), {});
        controller.initialize();
        QTRY_VERIFY_WITH_TIMEOUT(controller.online(), 10000);
        const auto pid = controller.coreProcessId();
        const auto oldSocket = controller.socket();
#ifdef Q_OS_LINUX
        QCOMPARE(::kill(pid_t(pid), SIGKILL), 0);
#endif
        QTRY_VERIFY(!controller.online());
        QTRY_VERIFY_WITH_TIMEOUT(controller.online() && controller.coreProcessId() != pid, 10000);
        QVERIFY(controller.coreProcessId() > 0);
        QVERIFY(controller.socket() != oldSocket);
        QVERIFY(!QFileInfo::exists(oldSocket));
    }
    void parentDeathCannotLeaveRostraRunning() {
        QTemporaryDir directory;
        QProcess host;
        const auto cleanup = qScopeGuard([&] {
            if (host.state() != QProcess::NotRunning) {
                host.kill();
                host.waitForFinished(5000);
            }
        });
        host.start(QCoreApplication::applicationFilePath(),
                   {"--core-host", backendBinary(), directory.path()});
        QVERIFY(host.waitForStarted(5000));
        QByteArray output;
        QTRY_VERIFY_WITH_TIMEOUT((output += host.readAllStandardOutput()).contains('\n'), 15000);
        const auto value = QJsonDocument::fromJson(output.trimmed()).object();
        const qint64 pid = value.value("pid").toVariant().toLongLong();
        const auto socket = value.value("socket").toString();
        QVERIFY(pid > 0);
        QVERIFY(processAlive(pid));
        host.kill(); // SIGKILL deliberately bypasses all Sawfish destructors and Quit handlers.
        QVERIFY(host.waitForFinished(5000));
        QTRY_VERIFY_WITH_TIMEOUT(!processAlive(pid), 5000);
        // The Go process leader can be a zombie before its other threads finish
        // releasing descriptors. Check the observable endpoint with a deadline.
        auto socketClosed = [&] {
            QLocalSocket probe;
            probe.connectToServer(socket);
            return !probe.waitForConnected(200);
        };
        QTRY_VERIFY_WITH_TIMEOUT(socketClosed(), 5000);
        const auto runtime = QFileInfo(socket).absolutePath();
        QVERIFY(runtime.startsWith(QDir::tempPath() + "/sawfish-core-"));
        QDir(runtime).removeRecursively();
    }
};

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == "run") {
#ifdef Q_OS_LINUX
        ::signal(SIGTERM, SIG_IGN);
#endif
        QFile ready("unresponsive.ready");
        if (!ready.open(QIODevice::WriteOnly))
            return 1;
        ready.close();
        return app.exec();
    }
    if (argc == 4 && QString::fromLocal8Bit(argv[1]) == "--core-host") {
        Controller controller(QString::fromLocal8Bit(argv[3]), QString::fromLocal8Bit(argv[2]), {});
        bool announced = false;
        QObject::connect(&controller, &Controller::changed, &app, [&] {
            if (announced || !controller.online() || controller.workerStatus() != "Ready")
                return;
            announced = true;
            const auto data = QJsonDocument(QJsonObject{{"pid", controller.coreProcessId()},
                                                        {"socket", controller.socket()}})
                                  .toJson(QJsonDocument::Compact);
            std::printf("%s\n", data.constData());
            std::fflush(stdout);
        });
        controller.initialize();
        return app.exec();
    }
#ifndef Q_OS_LINUX
    return 77;
#endif
    if (!QFileInfo(backendBinary()).isExecutable())
        return 77;
    QTemporaryDir directory;
    QLocalServer local;
    QTcpServer tcp;
    if (!local.listen(directory.filePath("probe.sock")) || !tcp.listen(QHostAddress::LocalHost, 0))
        return 77;
    local.close();
    tcp.close();
    LifecycleTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_lifecycle.moc"
