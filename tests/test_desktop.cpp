#include "backend/ownedprocess.h"
#include "backend/rpcclient.h"
#include "backendpath.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <cstdio>
#ifdef Q_OS_LINUX
#include <signal.h>
#endif

namespace {
QString backend() {
    return desktopBackendPath();
}
bool alive(qint64 pid) {
#ifdef Q_OS_WIN
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
    if (!process)
        return false;
    const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return running;
#else
    QFile state(QString("/proc/%1/stat").arg(pid));
    if (!state.open(QIODevice::ReadOnly))
        return false;
    auto data = state.readAll();
    return data.mid(data.lastIndexOf(')') + 2, 1) != "Z";
#endif
}
} // namespace

class DesktopTest : public QObject {
    Q_OBJECT
  private slots:
    void structuredVersion() {
        QProcess process;
        process.start(backend(), {"version"});
        QVERIFY(process.waitForFinished(10000));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 0);
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(process.readAllStandardOutput(), &error);
        QCOMPARE(error.error, QJsonParseError::NoError);
        QVERIFY(document.isObject());
        const auto info = document.object();
        QVERIFY(!info.value("version").toString().isEmpty());
        QCOMPARE(info.value("api_version").toInt(), 4);
        QCOMPARE(info.size(), 2);
    }
    void configurationPreflight_data() {
        QTest::addColumn<QByteArray>("config");
        QTest::addColumn<bool>("valid");
        QTest::newRow("valid") << QByteArray("{\"outbounds\":[{\"type\":\"direct\"}]}") << true;
        QTest::newRow("invalid-json") << QByteArray("invalid") << false;
        QTest::newRow("invalid-outbound")
            << QByteArray("{\"outbounds\":[{\"type\":\"unknown-type\"}]}") << false;
        QTest::newRow("oversized") << QByteArray(4 * 1024 * 1024 + 1, ' ') << false;
    }
    void configurationPreflight() {
        QFETCH(QByteArray, config);
        QFETCH(bool, valid);
        QProcess process;
        process.start(backend(), {"check"});
        QVERIFY(process.waitForStarted());
        process.write(config);
        process.closeWriteChannel();
        QVERIFY(process.waitForFinished(10000));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode() == 0, valid);
    }
    void invalidOwnershipOptions_data() {
        QTest::addColumn<QStringList>("arguments");
        QTest::addColumn<QByteArray>("message");
        QTest::newRow("run-missing-parent")
            << QStringList{"run", "--owned"} << QByteArray("requires --socket and --parent-pid");
        QTest::newRow("run-parent-without-owned")
            << QStringList{"run", "--parent-pid", "1"} << QByteArray("requires --owned");
        QTest::newRow("run-tcp") << QStringList{"run", "--owned", "--listen", "127.0.0.1:0"}
                                 << QByteArray("--listen cannot be used with --owned");
        QTest::newRow("worker-relay")
            << QStringList{"worker",
                           "--owned",
                           "--socket",
                           "unused.sock",
                           "--parent-pid",
                           "1",
                           "--daemon-relay-socket",
                           "unused-relay"}
            << QByteArray("--daemon-relay-socket cannot be used with --owned");
        for (const auto& command : {"run", "worker"}) {
            QTest::newRow(command)
                << QStringList{command, "--owned", "--socket", "unused.sock", "--parent-pid", "1"}
                << QByteArray("not started by its declared parent");
        }
    }
    void invalidOwnershipOptions() {
        QFETCH(QStringList, arguments);
        QFETCH(QByteArray, message);
        QProcess process;
        process.start(backend(), arguments);
        QVERIFY(process.waitForFinished(10000));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QVERIFY(process.exitCode() != 0);
        const auto errors = process.readAllStandardError();
        QVERIFY2(errors.contains(message), errors.constData());
    }
    void grpcAndValidationOverUnicodeSocket() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto socket = directory.filePath(QString::fromUtf8("café.sock"));
        QProcess core;
        bindProcessToParent(core);
        core.start(backend(),
                   {"run", "--owned", "-D", directory.filePath("state"), "--socket", socket,
                    "--parent-pid", QString::number(QCoreApplication::applicationPid())});
        QVERIFY(core.waitForStarted());
        QByteArray output;
        QTRY_VERIFY_WITH_TIMEOUT((output += core.readAllStandardOutput()).contains("READY"), 15000);
        RpcClient rpc;
        QSignalSpy ready(&rpc, &RpcClient::daemonReady), status(&rpc, &RpcClient::serviceStatus);
        rpc.connectDaemon(socket);
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 10000);
        QTRY_VERIFY(!status.isEmpty());
        QCOMPARE(status.last().first().toInt(), 0);
        QProcess worker;
        bindProcessToParent(worker);
        const auto workerSocket = directory.filePath("worker.sock");
        worker.start(backend(), {"worker", "--owned", "--socket", workerSocket, "--parent-pid",
                                 QString::number(QCoreApplication::applicationPid())});
        QVERIFY(worker.waitForStarted());
        output.clear();
        QTRY_VERIFY_WITH_TIMEOUT((output += worker.readAllStandardOutput()).contains("READY"),
                                 15000);
        rpc.setWorkerSocket(workerSocket);
        QSignalSpy checked(&rpc, &RpcClient::operationFinished);
        rpc.checkConfig(1, "{}");
        QTRY_COMPARE(checked.size(), 1);
        QVERIFY(checked.first()[1].toBool());
        rpc.checkConfig(2, "invalid");
        QTRY_COMPARE(checked.size(), 2);
        QVERIFY(!checked.last()[1].toBool());
        rpc.setWorkerSocket({});
        rpc.disconnectDaemon();
        worker.closeWriteChannel();
        core.closeWriteChannel();
        QVERIFY(worker.waitForFinished(5000));
        QVERIFY(core.waitForFinished(5000));
        QCOMPARE(core.exitCode(), 0);
        QCOMPARE(worker.exitCode(), 0);
    }
    void parentDeathKillsOwnedProcess_data() {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("bound");
        QTest::newRow("core") << QString("core") << true;
        QTest::newRow("worker") << QString("worker") << true;
        QTest::newRow("core-without-os-binding") << QString("core") << false;
        QTest::newRow("worker-without-os-binding") << QString("worker") << false;
        QTest::newRow("uncooperative-child") << QString("stubborn") << true;
    }
    void parentDeathKillsOwnedProcess() {
        QFETCH(QString, mode);
        QFETCH(bool, bound);
        QTemporaryDir directory;
        QProcess host;
        host.start(QCoreApplication::applicationFilePath(),
                   {"--host", backend(), directory.path(), mode, bound ? "bound" : "unbound"});
        QVERIFY(host.waitForStarted());
        QByteArray output;
        QTRY_VERIFY_WITH_TIMEOUT((output += host.readAllStandardOutput()).contains('\n'), 15000);
        const auto pid = output.trimmed().toLongLong();
        QVERIFY(pid > 0);
        QVERIFY(alive(pid));
        host.kill();
        QVERIFY(host.waitForFinished());
        QTRY_VERIFY_WITH_TIMEOUT(!alive(pid), 5000);
    }
};

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.value(1) == "--hang") {
        std::puts("READY");
        std::fflush(stdout);
        return app.exec();
    }
    if (args.value(1) == "--host") {
        QProcess child;
        if (args.value(5) != "unbound")
            bindProcessToParent(child);
        if (args.value(4) == "stubborn")
            child.start(app.applicationFilePath(), {"--hang"});
        else if (args.value(4) == "worker")
            child.start(args.value(2),
                        {"worker", "--owned", "--socket", args.value(3) + "/worker.sock",
                         "--parent-pid", QString::number(app.applicationPid())});
        else
            child.start(args.value(2), {"run", "--owned", "-D", args.value(3) + "/state",
                                        "--socket", args.value(3) + "/control.sock", "--parent-pid",
                                        QString::number(app.applicationPid())});
        if (!child.waitForStarted())
            return 1;
        QByteArray output;
        QElapsedTimer timer;
        timer.start();
        while (!output.contains("READY") && timer.elapsed() < 10000) {
            child.waitForReadyRead(100);
            output += child.readAllStandardOutput();
        }
        if (!output.contains("READY"))
            return 2;
        std::printf("%lld\n", static_cast<long long>(child.processId()));
        std::fflush(stdout);
        return app.exec();
    }
    DesktopTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_desktop.moc"
