#include "app/controller.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLocalServer>
#include <QProcess>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

class BackendTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_directory;
    QProcess m_daemon;
    QString m_binary, m_socket, m_profile;
    quint16 m_proxyPort = 0;
    std::unique_ptr<Controller> m_controller;
    bool launchDaemon() {
        m_daemon.setProgram(m_binary);
        m_daemon.setArguments({"run", "-D", m_directory.filePath("daemon"), "--socket", m_socket});
        m_daemon.start();
        return m_daemon.waitForStarted(5000);
    }
  private slots:
    void initTestCase() {
        m_binary = qEnvironmentVariable("SAWFISH_TEST_BACKEND",
                                        QCoreApplication::applicationDirPath() + "/rostra-daemon");
        QVERIFY2(QFileInfo(m_binary).isExecutable(),
                 "Build rostra-daemon first or set SAWFISH_TEST_BACKEND.");
        m_socket = m_directory.filePath("daemon.sock");
        QTcpServer reserve;
        QVERIFY(reserve.listen(QHostAddress::LocalHost, 0));
        m_proxyPort = reserve.serverPort();
        reserve.close();
        m_profile = m_directory.filePath("direct.json");
        QFile profile(m_profile);
        QVERIFY(profile.open(QIODevice::WriteOnly));
        profile.write(
            QString(
                R"({"log":{"level":"debug"},"inbounds":[{"type":"mixed","tag":"mixed-in","listen":"127.0.0.1","listen_port":%1}],"outbounds":[{"type":"selector","tag":"proxy","outbounds":["direct-a","direct-b"],"default":"direct-a"},{"type":"direct","tag":"direct-a"},{"type":"direct","tag":"direct-b"}],"route":{"final":"proxy"}})")
                .arg(m_proxyPort)
                .toUtf8());
        profile.close();
        QVERIFY(launchDaemon());
        m_controller =
            std::make_unique<Controller>(m_directory.filePath("client"), m_binary, m_socket);
        m_controller->initialize();
        QTRY_VERIFY_WITH_TIMEOUT(m_controller->online(), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(m_controller->state(), Controller::Idle, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(m_controller->workerStatus(), QString("Ready"), 10000);
    }
    void validateStartAndProxyTraffic() {
        QVERIFY(m_controller->importProfile(m_profile));
        QVERIFY(m_controller->canConnect());
        m_controller->connectService();
        QTRY_VERIFY_WITH_TIMEOUT(!m_controller->busy(), 10000);
        QVERIFY2(!m_controller->messageIsError(), qPrintable(m_controller->message()));
        QTRY_COMPARE_WITH_TIMEOUT(m_controller->state(), Controller::Running, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(m_controller->logModel()->rowCount() > 0, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(m_controller->groupsReady(), 5000);
        const auto* group = m_controller->groupModel()->find("proxy");
        QVERIFY(group);
        QCOMPARE(group->selected, "direct-a");
        QCOMPARE(group->members.size(), 2);
        exerciseProxyTraffic();
    }
    void switchOutboundWithoutRestarting() {
        QVERIFY(m_controller->canSelectOutbound("proxy", "direct-b"));
        m_controller->selectOutbound("proxy", "direct-b");
        QTRY_VERIFY_WITH_TIMEOUT(!m_controller->busy(), 5000);
        QVERIFY2(!m_controller->messageIsError(), qPrintable(m_controller->message()));
        QTRY_COMPARE_WITH_TIMEOUT(m_controller->groupModel()->find("proxy")->selected, "direct-b",
                                  5000);
        QCOMPARE(m_controller->state(), Controller::Running);
        m_controller->logModel()->clear();
        exerciseProxyTraffic();
        auto usedSelectedOutbound = [this] {
            auto* logs = m_controller->logModel();
            for (int row = 0; row < logs->rowCount(); ++row)
                if (logs->index(row)
                        .data(LogModel::MessageRole)
                        .toString()
                        .contains("outbound/direct[direct-b]"))
                    return true;
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(usedSelectedOutbound(), 5000);
    }

  private:
    void exerciseProxyTraffic() {
        QTcpServer echo;
        QVERIFY(echo.listen(QHostAddress::LocalHost, 0));
        connect(&echo, &QTcpServer::newConnection, &echo, [&echo] {
            auto* peer = echo.nextPendingConnection();
            connect(peer, &QTcpSocket::readyRead, peer, [peer] { peer->write(peer->readAll()); });
        });
        QTcpSocket client;
        client.connectToHost(QHostAddress::LocalHost, m_proxyPort);
        QTRY_COMPARE_WITH_TIMEOUT(client.state(), QAbstractSocket::ConnectedState, 5000);
        client.write(QString("CONNECT 127.0.0.1:%1 HTTP/1.1\r\nHost: 127.0.0.1:%1\r\n\r\n")
                         .arg(echo.serverPort())
                         .toUtf8());
        QByteArray response;
        QTRY_VERIFY_WITH_TIMEOUT((response += client.readAll()).contains("\r\n\r\n"), 5000);
        QVERIFY2(response.contains("200"), response.constData());
        client.write("sawfish-through-rostra");
        QByteArray payload;
        QTRY_VERIFY_WITH_TIMEOUT((payload += client.readAll()).contains("sawfish-through-rostra"),
                                 5000);
        QTRY_VERIFY_WITH_TIMEOUT(m_controller->trafficSample().available, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(m_controller->trafficSample().uploadTotal > 0, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(m_controller->connectionsReady(), 5000);
        QString tracked;
        auto findConnection = [&] {
            for (const auto& entry : m_controller->connectionModel()->snapshot())
                if (entry.active() &&
                    entry.destination.endsWith(":" + QString::number(echo.serverPort()))) {
                    tracked = entry.id;
                    return true;
                }
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(findConnection(), 5000);
        QTRY_VERIFY(m_controller->canCloseConnections());
        m_controller->closeConnection(tracked);
        QTRY_COMPARE_WITH_TIMEOUT(client.state(), QAbstractSocket::UnconnectedState, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!m_controller->connectionModel()->find(tracked)->active(), 5000);
        QVERIFY(m_controller->connectionModel()->find(tracked)->upload > 0);
    }
  private slots:
    void profileSwitchRestartsRunningCore() {
        const auto first = m_controller->selectedId();
        const auto originalPort = m_proxyPort;
        QTcpServer reserve;
        QVERIFY(reserve.listen(QHostAddress::LocalHost, 0));
        const auto alternatePort = reserve.serverPort();
        reserve.close();
        QFile source(m_profile);
        QVERIFY(source.open(QIODevice::ReadOnly));
        auto content = source.readAll();
        content.replace(QByteArray::number(originalPort), QByteArray::number(alternatePort));
        QVERIFY(QDir().mkpath(m_directory.filePath("alternate")));
        QFile alternate(m_directory.filePath("alternate/direct.json"));
        QVERIFY(alternate.open(QIODevice::WriteOnly));
        alternate.write(content);
        alternate.close();
        QVERIFY(m_controller->importProfile(alternate.fileName()));
        const auto second = m_controller->selectedId();
        QVERIFY(first != second);
        QTRY_VERIFY_WITH_TIMEOUT(!m_controller->busy(), 10000);
        QVERIFY2(!m_controller->messageIsError(), qPrintable(m_controller->message()));
        QTRY_COMPARE(m_controller->state(), Controller::Running);
        QCOMPARE(m_controller->activeId(), second);
        QCOMPARE(m_controller->selectedProfile()->name, "direct.json (1)");
        QTRY_VERIFY(m_controller->groupsReady() && m_controller->groupModel()->find("proxy"));
        // A reload preserves selector choice but replaces the inbound listener.
        QCOMPARE(m_controller->groupModel()->find("proxy")->selected, "direct-b");
        QVERIFY(reserve.listen(QHostAddress::LocalHost, originalPort));
        reserve.close();
        m_proxyPort = alternatePort;
        exerciseProxyTraffic();
        m_controller->selectProfile(first);
        QTRY_VERIFY_WITH_TIMEOUT(!m_controller->busy(), 10000);
        QCOMPARE(m_controller->activeId(), first);
        QTRY_COMPARE(m_controller->state(), Controller::Running);
        QVERIFY(reserve.listen(QHostAddress::LocalHost, alternatePort));
        reserve.close();
        m_proxyPort = originalPort;
        exerciseProxyTraffic();
    }
    void invalidValidationKeepsRunningSession() {
        QFile file(m_directory.filePath("invalid.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{not valid json}");
        file.close();
        QVERIFY(m_controller->importProfile(file.fileName()));
        QVERIFY(!m_controller->canConnect());
        QTRY_VERIFY_WITH_TIMEOUT(!m_controller->busy(), 5000);
        QVERIFY(m_controller->messageIsError());
        QCOMPARE(m_controller->selectedProfile()->validation, Profile::Invalid);
        QCOMPARE(m_controller->state(), Controller::Running);
    }
    void reopeningAttachesAndDisconnects() {
        m_controller.reset();
        m_controller =
            std::make_unique<Controller>(m_directory.filePath("client"), m_binary, m_socket);
        m_controller->initialize();
        QTRY_COMPARE_WITH_TIMEOUT(m_controller->state(), Controller::Running, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(m_controller->groupsReady(), 5000);
        QVERIFY(m_controller->groupModel()->find("proxy"));
        QCOMPARE(m_controller->groupModel()->find("proxy")->selected, "direct-b");
        QCOMPARE(m_controller->profiles()->rowCount(), 3);
        QCOMPARE(m_controller->selectedProfile()->name, "invalid.json");
        QCOMPARE(m_controller->activeName(), "Existing daemon session");
        QTRY_VERIFY_WITH_TIMEOUT(m_controller->canDisconnect(), 5000);
        m_controller->disconnectService();
        QTRY_VERIFY_WITH_TIMEOUT(!m_controller->busy(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(m_controller->state(), Controller::Idle, 5000);
        QCOMPARE(m_controller->groupModel()->rowCount(), 0);
        QVERIFY(!m_controller->groupsReady());
    }
    void reconnectAfterDaemonRestart() {
        m_daemon.terminate();
        QVERIFY(m_daemon.waitForFinished(5000));
        QTRY_VERIFY_WITH_TIMEOUT(!m_controller->online(), 5000);
        QVERIFY(launchDaemon());
        QTRY_VERIFY_WITH_TIMEOUT(m_controller->online(), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(m_controller->state(), Controller::Idle, 5000);
    }
    void cleanupTestCase() {
        m_controller.reset();
        m_daemon.terminate();
        if (!m_daemon.waitForFinished(5000)) {
            m_daemon.kill();
            m_daemon.waitForFinished(1000);
        }
    }
};
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto binary = qEnvironmentVariable(
        "SAWFISH_TEST_BACKEND", QCoreApplication::applicationDirPath() + "/rostra-daemon");
    if (!QFileInfo(binary).isExecutable()) {
        qWarning(
            "Backend integration tests skipped: build rostra-daemon or set SAWFISH_TEST_BACKEND.");
        return 77;
    }
    QTemporaryDir probeDirectory;
    QLocalServer localProbe;
    QTcpServer tcpProbe;
    if (!localProbe.listen(probeDirectory.filePath("probe.sock")) ||
        !tcpProbe.listen(QHostAddress::LocalHost, 0)) {
        qWarning("Backend integration tests skipped: local Unix and TCP socket binding is "
                 "unavailable in this environment.");
        return 77;
    }
    localProbe.close();
    tcpProbe.close();
    BackendTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_backend.moc"
