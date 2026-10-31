#include "app/controller.h"
// Exercise the Win32 macro collision on every platform. The fake service and
// real client must link and exchange StartService RPCs with generated code.
#ifndef StartService
#define StartService StartServiceW
#endif
// Deliberately bypass the wrapper: generated headers must also be safe when
// included directly by generated implementation files.
#include "experimental/boxdd/desktop_service.grpc.pb.h"
#include "fakebackend.h"
#include <QFile>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>

class RpcTest : public QObject {
    Q_OBJECT
    FakeState state;
    FakeDesktop desktop{state};
    FakeManaged managed{state};
    FakeApplication application{state};
    FakeStarted started{state};
    std::unique_ptr<grpc::Server> server;
    RpcClient::ChannelFactory factory() {
        return [this](const QString&) { return server->InProcessChannel({}); };
    }
    QString writeProfile(QTemporaryDir& dir, const QByteArray& data) {
        QFile file(dir.filePath("config.json"));
        if (!file.open(QIODevice::WriteOnly))
            return {};
        file.write(data);
        return file.fileName();
    }
  private slots:
    void initTestCase() {
        grpc::ServerBuilder builder;
        builder.RegisterService(&desktop);
        builder.RegisterService(&managed);
        builder.RegisterService(&application);
        builder.RegisterService(&started);
        server = builder.BuildAndStart();
        QVERIFY(server);
    }
    void init() {
        state.api = 4;
        state.claims = 0;
        state.starts = 0;
        state.stops = 0;
        state.checks = 0;
        state.otherOwner = false;
        state.unavailable = false;
        state.failStreams = false;
        state.uncertainStart = false;
        state.holdCheck = false;
        state.selections = 0;
        state.delayTests = 0;
        state.rejectDelay = false;
        state.holdDelay = false;
        state.groupSubscriptions = 0;
        state.rejectSelection = false;
        state.uncertainSelection = false;
        state.holdSelection = false;
        state.failGroups = false;
        state.unsupportedGroups = false;
        state.connectionSubscriptions = 0;
        state.connectionCloses = 0;
        state.allConnectionCloses = 0;
        state.failConnections = false;
        state.unsupportedConnections = false;
        state.rejectClose = false;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.connectionBatches.clear();
            state.activeConnections.clear();
        }
        state.setGroups(sampleGroups());
        state.setState(0);
    }
    void commandsStreamsAndCancellation() {
        auto rpc = std::make_unique<RpcClient>(nullptr, factory());
        QSignalSpy ready(rpc.get(), &RpcClient::daemonReady);
        QSignalSpy status(rpc.get(), &RpcClient::serviceStatus);
        QSignalSpy logs(rpc.get(), &RpcClient::logs);
        QSignalSpy finished(rpc.get(), &RpcClient::operationFinished);
        rpc->connectDaemon("/daemon.sock");
        rpc->setWorkerSocket("/worker.sock");
        QTRY_COMPARE(ready.size(), 1);
        QTRY_VERIFY(!status.isEmpty());
        QTRY_VERIFY(!logs.isEmpty());
        QVERIFY(logs.first()[1].toBool());
        QCOMPARE(qvariant_cast<LogBatch>(logs.first()[0]).first().message, "daemon log");
        rpc->checkConfig(1, "invalid");
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!finished[0][1].toBool());
        QVERIFY(!finished[0][3].toBool());
        rpc->checkConfig(2, "{}");
        QTRY_COMPARE(finished.size(), 2);
        QVERIFY(finished[1][1].toBool());
        rpc->startService(3, "{}");
        QTRY_COMPARE(finished.size(), 3);
        QTRY_COMPARE(status.last()[0].toInt(), 2);
        rpc->stopService(4);
        QTRY_COMPARE(finished.size(), 4);
        QTRY_COMPARE(status.last()[0].toInt(), 0);
        QElapsedTimer shutdown;
        shutdown.start();
        rpc.reset();
        QVERIFY(shutdown.elapsed() < 2000);
        QTRY_COMPARE(state.activeStreams.load(), 0);
    }
    void connectionsStreamRecoveryAndClosure() {
        state.setState(2);
        state.sendConnections(sampleConnections());
        QTemporaryDir directory;
        Controller controller(directory.filePath("client"), "/missing/worker", "/daemon.sock",
                              nullptr, factory());
        controller.initialize();
        QTRY_VERIFY(controller.connectionsReady());
        auto* model = controller.connectionModel();
        QTRY_COMPARE(model->rowCount(), 3);
        QCOMPARE(model->activeCount(), 2);
        rostra_rpc::ConnectionEvents update;
        auto* event = update.add_events();
        event->set_id("browser");
        event->set_type(rostra_rpc::CONNECTION_EVENT_UPDATE);
        event->set_downlinkdelta(512);
        state.sendConnections(update);
        state.sendConnections(update);
        QTRY_COMPARE(model->find("browser")->download, 5120);
        state.rejectClose = true;
        QSignalSpy errors(&controller, &Controller::notification);
        controller.closeConnection("browser");
        QTRY_COMPARE(errors.size(), 1);
        QVERIFY(errors.first()[0].toString().contains("close denied"));
        QVERIFY(model->find("browser")->active());
        state.rejectClose = false;
        controller.closeConnection("browser");
        QTRY_VERIFY(!model->find("browser")->active());
        QTRY_VERIFY(controller.canCloseConnections());
        controller.closeAllConnections();
        QTRY_COMPARE(model->activeCount(), 0);
        state.failConnections = true;
        QTRY_VERIFY(!controller.connectionsReady());
        QVERIFY(controller.online()); // A tracking failure does not stop the core.
        state.failConnections = false;
        QTRY_VERIFY_WITH_TIMEOUT(controller.connectionsReady(), 4000);
        QCOMPARE(model->rowCount(), 3);
        state.setState(0);
        QTRY_COMPARE(model->rowCount(), 0);
        QVERIFY(!controller.canCloseConnections());
        state.unsupportedConnections = true;
        state.setState(2);
        QTRY_VERIFY(controller.connectionsError().contains("unsupported"));
        QVERIFY(controller.online());
    }
    void workerRestartIsAsynchronous() {
        WorkerProcess worker;
        QSignalSpy ready(&worker, &WorkerProcess::ready);
        QSignalSpy unavailable(&worker, &WorkerProcess::unavailable);
        worker.start(QCoreApplication::applicationFilePath());
        QTRY_COMPARE(ready.size(), 1);
        QElapsedTimer elapsed;
        elapsed.start();
        worker.start({});
        QVERIFY(elapsed.elapsed() < 200);
        QTRY_COMPARE(unavailable.size(), 1);
        QVERIFY(!worker.isReady());
        worker.start(QCoreApplication::applicationFilePath());
        QTRY_COMPARE(ready.size(), 2);
        worker.start(QCoreApplication::applicationFilePath());
        QTRY_COMPARE(ready.size(), 3);
        QVERIFY(worker.isReady());
    }
    void ownershipAndVersionFailures() {
        RpcClient rpc(nullptr, factory());
        QSignalSpy lost(&rpc, &RpcClient::daemonUnavailable);
        state.otherOwner = true;
        rpc.connectDaemon("/daemon.sock");
        QTRY_COMPARE(lost.size(), 1);
        QVERIFY(!lost.last()[1].toBool());
        QCOMPARE(state.claims.load(), 0);
        state.otherOwner = false;
        state.api = 99;
        rpc.connectDaemon("/daemon.sock");
        QTRY_COMPARE(lost.size(), 2);
        QVERIFY(!lost.last()[1].toBool());
        QVERIFY(lost.last()[0].toString().contains("99"));
        QCOMPARE(state.activeStreams.load(), 0);
    }
    void controllerValidatesBeforeStarting() {
        QTemporaryDir dir;
        Controller controller(dir.filePath("client"), QCoreApplication::applicationFilePath(),
                              "/daemon.sock", nullptr, factory());
        controller.initialize();
        QTRY_COMPARE(controller.workerStatus(), "Ready");
        QTRY_COMPARE(controller.state(), Controller::Idle);
        QVERIFY(controller.importProfile(writeProfile(dir, "invalid")));
        controller.connectService();
        QTRY_VERIFY(!controller.busy());
        QVERIFY(controller.messageIsError());
        QCOMPARE(state.starts.load(), 0);
        QVERIFY(controller.importProfile(writeProfile(dir, "{\n // original text\n}")));
        controller.connectService();
        QTRY_VERIFY(!controller.busy());
        QTRY_COMPARE(controller.state(), Controller::Running);
        QCOMPARE(state.starts.load(), 1);
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            QCOMPARE(state.lastConfig, "{\n // original text\n}");
        }
        QVERIFY(!controller.canConnect());
        controller.disconnectService();
        QTRY_VERIFY(!controller.busy());
        QTRY_COMPARE(controller.state(), Controller::Idle);
    }
    void lostValidationIntentIsNotReplayed() {
        QTemporaryDir dir;
        Controller controller(dir.filePath("client"), QCoreApplication::applicationFilePath(),
                              "/daemon.sock", nullptr, factory());
        controller.initialize();
        QTRY_COMPARE(controller.workerStatus(), "Ready");
        QTRY_COMPARE(controller.state(), Controller::Idle);
        QVERIFY(controller.importProfile(writeProfile(dir, "{}")));
        state.holdCheck = true;
        controller.connectService();
        QTRY_COMPARE(state.checks.load(), 1);
        state.failStreams = true;
        state.unavailable = true;
        QTRY_VERIFY(!controller.online());
        QTRY_VERIFY(!controller.busy());
        state.holdCheck = false;
        QTest::qWait(100);
        state.failStreams = false;
        state.unavailable = false;
        controller.retry();
        QTRY_COMPARE(controller.state(), Controller::Idle);
        QCOMPARE(state.starts.load(), 0);
    }
    void standaloneValidationWorksWithoutDaemon() {
        state.unavailable = true;
        QTemporaryDir dir;
        Controller controller(dir.filePath("client"), QCoreApplication::applicationFilePath(),
                              "/daemon.sock", nullptr, factory());
        controller.initialize();
        QTRY_COMPARE(controller.workerStatus(), "Ready");
        QVERIFY(controller.importProfile(writeProfile(dir, "{}")));
        state.holdCheck = true;
        controller.validateProfile();
        QTRY_COMPARE(state.checks.load(), 1);
        QTest::qWait(1200);
        QVERIFY(controller.busy());
        state.holdCheck = false;
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(controller.message(), "Configuration is valid.");
        QCOMPARE(state.starts.load(), 0);
    }
    void uncertainStartIsReconciledWithoutReplay() {
        state.uncertainStart = true;
        QTemporaryDir dir;
        Controller controller(dir.filePath("client"), QCoreApplication::applicationFilePath(),
                              "/daemon.sock", nullptr, factory());
        controller.initialize();
        QTRY_COMPARE(controller.workerStatus(), "Ready");
        QTRY_COMPARE(controller.state(), Controller::Idle);
        QVERIFY(controller.importProfile(writeProfile(dir, "{}")));
        controller.connectService();
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), Controller::Running, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(controller.online() && !controller.busy(), 5000);
        QCOMPARE(state.starts.load(), 1);
    }
    void automaticChecksIgnoreStaleEditsAndDeletedProfiles() {
        QTemporaryDir dir;
        Controller controller(dir.filePath("client"), QCoreApplication::applicationFilePath(),
                              "/daemon.sock", nullptr, factory());
        controller.initialize();
        QTRY_COMPARE(controller.workerStatus(), "Ready");
        QTRY_COMPARE(controller.state(), Controller::Idle);
        state.holdCheck = true;
        QVERIFY(controller.importProfile(writeProfile(dir, "invalid")));
        const auto id = controller.selectedId();
        QTRY_COMPARE(state.checks.load(), 1);
        QVERIFY(!controller.busy()); // Background checks do not lock profile editing.
        QCOMPARE(controller.profiles()->find(id)->validation, Profile::Checking);
        QVERIFY(controller.saveProfile(id, "Edited", "https://example.org/profile.json", "{}"));
        state.holdCheck = false;
        QTRY_COMPARE(controller.profiles()->find(id)->validation, Profile::Valid);
        QCOMPARE(state.checks.load(), 2);
        QCOMPARE(controller.state(), Controller::Idle);
        QCOMPARE(state.starts.load(), 0);
        state.holdCheck = true;
        QVERIFY(controller.saveProfile(id, "Edited", {}, "invalid"));
        QTRY_COMPARE(state.checks.load(), 3);
        controller.deleteProfile(id);
        QCOMPARE(controller.profiles()->rowCount(), 0);
        state.holdCheck = false;
        QTest::qWait(100);
        QVERIFY(!controller.profiles()->find(id));
        QVERIFY(controller.selectedId().isEmpty());
        QCOMPARE(state.starts.load(), 0);
    }
    void switchingProfilesRestartsOnlyAfterSuccessfulCheck() {
        QTemporaryDir dir;
        Controller controller(dir.filePath("client"), QCoreApplication::applicationFilePath(),
                              "/daemon.sock", nullptr, factory());
        controller.initialize();
        QTRY_COMPARE(controller.workerStatus(), "Ready");
        QTRY_COMPARE(controller.state(), Controller::Idle);
        QVERIFY(controller.importProfile(writeProfile(dir, "{}")));
        const auto first = controller.selectedId();
        QTRY_COMPARE(controller.selectedProfile()->validation, Profile::Valid);
        QVERIFY(controller.importProfile(writeProfile(dir, "{ }")));
        const auto second = controller.selectedId();
        QTRY_COMPARE(controller.selectedProfile()->validation, Profile::Valid);
        controller.selectProfile(first);
        QCOMPARE(state.starts.load(), 0); // Selection while off leaves the connection off.
        controller.connectService();
        QTRY_VERIFY(!controller.busy());
        QTRY_COMPARE(controller.state(), Controller::Running);
        QCOMPARE(state.starts.load(), 1);
        state.holdCheck = true;
        controller.selectProfile(second);
        QTRY_VERIFY(controller.busy());
        QCOMPARE(state.starts.load(), 1);
        QCOMPARE(controller.activeId(), first);
        state.holdCheck = false;
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(state.starts.load(), 2);
        QCOMPARE(controller.activeId(), second);
        QVERIFY(controller.saveProfile(second, "Changed", {}, "invalid"));
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(controller.selectedProfile()->validation, Profile::Invalid);
        QCOMPARE(state.starts.load(), 2);
        QCOMPARE(controller.state(), Controller::Running);
        QCOMPARE(controller.activeId(), second);
        QVERIFY(controller.saveProfile(second, "Changed", {}, "{  }"));
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(controller.selectedProfile()->validation, Profile::Valid);
        QCOMPARE(state.starts.load(), 3);
        controller.deleteProfile(second);
        QTRY_VERIFY(!controller.busy());
        QTRY_COMPARE(controller.state(), Controller::Idle);
        QVERIFY(!controller.profiles()->find(second));
        QCOMPARE(state.stops.load(), 1);
        QCOMPARE(controller.selectedId(), first);
    }
    void urlImportsHandleRedirectsErrorsAndLimits() {
        QTcpServer http;
        QVERIFY(http.listen(QHostAddress::LocalHost, 0));
        connect(&http, &QTcpServer::newConnection, &http, [&http] {
            auto* peer = http.nextPendingConnection();
            connect(peer, &QTcpSocket::disconnected, peer, &QObject::deleteLater);
            connect(peer, &QTcpSocket::readyRead, peer, [peer] {
                const auto request = peer->readAll();
                if (!request.contains("HTTP/1.1"))
                    return;
                QByteArray response;
                if (request.startsWith("GET /redirect"))
                    response =
                        "HTTP/1.1 302 Found\r\nLocation: /config.json\r\nContent-Length: 0\r\n\r\n";
                else if (request.startsWith("GET /large"))
                    response = "HTTP/1.1 200 OK\r\nContent-Length: 2097153\r\n\r\nx";
                else if (request.startsWith("GET /fail"))
                    response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
                else
                    response = "HTTP/1.1 200 OK\r\nContent-Length: 7\r\n\r\ninvalid";
                peer->write(response);
                peer->disconnectFromHost();
            });
        });
        QTemporaryDir dir;
        Controller controller(dir.filePath("client"), QCoreApplication::applicationFilePath(),
                              "/daemon.sock", nullptr, factory());
        controller.initialize();
        QTRY_COMPARE(controller.workerStatus(), "Ready");
        const auto base = QString("http://127.0.0.1:%1").arg(http.serverPort());
        controller.importProfileUrl(base + "/config.json");
        QTRY_VERIFY(!controller.importing());
        QCOMPARE(controller.profiles()->rowCount(), 1);
        QTRY_COMPARE(controller.selectedProfile()->validation, Profile::Invalid);
        QCOMPARE(controller.selectedProfile()->name, "config.json");
        QCOMPARE(controller.selectedProfile()->url, base + "/config.json");
        controller.importProfileUrl(base + "/config.json");
        QTRY_VERIFY(!controller.importing());
        QCOMPARE(controller.profiles()->rowCount(), 2);
        QCOMPARE(controller.selectedProfile()->name, "config.json (1)");
        controller.importProfileUrl(base + "/redirect");
        QTRY_VERIFY(!controller.importing());
        QCOMPARE(controller.profiles()->rowCount(), 3);
        QCOMPARE(controller.selectedProfile()->content, "invalid");
        controller.importProfileUrl(base + "/fail");
        QTRY_VERIFY(!controller.importing());
        QCOMPARE(controller.profiles()->rowCount(), 3);
        QVERIFY(controller.messageIsError());
        controller.importProfileUrl(base + "/large");
        QTRY_VERIFY(!controller.importing());
        QCOMPARE(controller.profiles()->rowCount(), 3);
        QVERIFY(controller.message().contains("2 MiB"));
        controller.importProfileUrl("file:///tmp/config.json");
        QVERIFY(controller.messageIsError());
        QVERIFY(!controller.importing());
    }
    void updatingRemoteProfilePreservesIdentityAndConcurrentEdits() {
        QTcpServer http;
        QVERIFY(http.listen(QHostAddress::LocalHost, 0));
        QByteArray body = "{}";
        int status = 200, requests = 0;
        bool hold = false;
        QPointer<QTcpSocket> pending;
        auto respond = [&](QTcpSocket* peer) {
            peer->write("HTTP/1.1 " + QByteArray::number(status) + " Result\r\nContent-Length: " +
                        QByteArray::number(body.size()) + "\r\n\r\n" + body);
            peer->disconnectFromHost();
        };
        connect(&http, &QTcpServer::newConnection, &http, [&] {
            auto* peer = http.nextPendingConnection();
            connect(peer, &QTcpSocket::disconnected, peer, &QObject::deleteLater);
            connect(peer, &QTcpSocket::readyRead, peer, [&, peer] {
                peer->readAll();
                ++requests;
                if (hold)
                    pending = peer;
                else
                    respond(peer);
            });
        });
        QTemporaryDir dir;
        Controller controller(dir.filePath("client"), QCoreApplication::applicationFilePath(),
                              "/daemon.sock", nullptr, factory());
        controller.initialize();
        QTRY_COMPARE(controller.workerStatus(), "Ready");
        QTRY_COMPARE(controller.state(), Controller::Idle);
        const auto url = QString("http://127.0.0.1:%1/config.json").arg(http.serverPort());
        controller.importProfileUrl(url);
        QTRY_VERIFY(!controller.importing());
        const auto id = controller.selectedId();
        QTRY_COMPARE(controller.selectedProfile()->validation, Profile::Valid);
        QVERIFY(controller.saveProfile(id, "Remote", url, "{}"));
        controller.connectService();
        QTRY_VERIFY(!controller.busy());
        QTRY_COMPARE(controller.state(), Controller::Running);
        body = "{ }";
        controller.updateProfile(id);
        QVERIFY(controller.updatingProfile(id));
        controller.updateProfile(id);
        QTRY_VERIFY(!controller.importing());
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(requests, 2);
        QCOMPARE(controller.profiles()->rowCount(), 1);
        QCOMPARE(controller.selectedId(), id);
        QCOMPARE(controller.selectedProfile()->name, "Remote");
        QCOMPARE(controller.selectedProfile()->content, "{ }");
        QCOMPARE(controller.selectedProfile()->validation, Profile::Valid);
        QCOMPARE(state.starts.load(), 2);
        status = 500;
        controller.updateProfile(id);
        QTRY_VERIFY(!controller.importing());
        QVERIFY(controller.messageIsError());
        QCOMPARE(controller.selectedProfile()->content, "{ }");
        status = 200;
        body = "invalid";
        controller.updateProfile(id);
        QTRY_VERIFY(!controller.importing());
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(controller.selectedProfile()->validation, Profile::Invalid);
        QCOMPARE(state.starts.load(), 2);
        QCOMPARE(controller.state(), Controller::Running);
        hold = true;
        controller.updateProfile(id);
        QTRY_VERIFY(pending);
        QVERIFY(controller.saveProfile(id, "Edited while downloading", url, "{  }"));
        QTRY_VERIFY(!controller.busy());
        respond(pending);
        pending.clear();
        QTRY_VERIFY(!controller.importing());
        QVERIFY(controller.message().contains("changed while downloading"));
        QCOMPARE(controller.selectedProfile()->content, "{  }");
        controller.updateProfile(id);
        QTRY_VERIFY(pending);
        controller.deleteProfile(id);
        QTRY_VERIFY(!controller.busy());
        respond(pending);
        QTRY_VERIFY(!controller.importing());
        QCOMPARE(controller.profiles()->rowCount(), 0);
    }
    void delayTestsWaitForMeasurementsAndDoNotSelect() {
        QTemporaryDir dir;
        Controller controller(dir.filePath("client"), QCoreApplication::applicationFilePath(),
                              "/daemon.sock", nullptr, factory());
        controller.initialize();
        state.setState(2);
        QTRY_VERIFY(controller.groupsReady());
        QVERIFY(controller.canTestDelay("urltest", "direct-b"));
        controller.testDelay("urltest", "direct-b");
        QTRY_COMPARE(state.delayTests.load(), 1);
        QVERIFY(controller.testingDelay("direct-b"));
        QVERIFY(!controller.busy());
        controller.testDelay("proxy", "direct-b");
        QCOMPARE(state.delayTests.load(), 1); // Same outbound shares one test across groups.
        QTest::qWait(100); // The successful RPC acknowledgement is not a latency result.
        QVERIFY(controller.testingDelay("direct-b"));
        auto groups = sampleGroups();
        for (auto& group : *groups.mutable_group()) {
            group.mutable_items(1)->set_urltesttime(1700000001);
            group.mutable_items(1)->set_urltestdelay(42);
        }
        state.setGroups(groups);
        QTRY_VERIFY(!controller.testingDelay("direct-b"));
        QCOMPARE(controller.groupModel()->find("urltest")->members[1].delay, 42);
        QCOMPARE(state.selections.load(), 0);
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            QCOMPARE(state.lastDelayOutbound, "direct-b");
        }
        state.rejectDelay = true;
        controller.testDelay("loadbalance", "direct-a");
        QTRY_COMPARE(state.delayTests.load(), 2);
        QTRY_VERIFY(!controller.testingDelay("direct-a"));
        QVERIFY(controller.delayError("direct-a").contains("outbound was removed"));
        state.rejectDelay = false;
        controller.testDelay("proxy", "direct-a");
        QTRY_COMPARE(state.delayTests.load(), 3);
        QVERIFY(controller.testingDelay("direct-a"));
        state.setState(0);
        QTRY_COMPARE(controller.state(), Controller::Idle);
        QVERIFY(!controller.testingDelay("direct-a"));
        QVERIFY(!controller.canTestDelay("proxy", "direct-a"));
        QCOMPARE(state.selections.load(), 0);
        state.setState(2);
        QTRY_VERIFY(controller.groupsReady());
        controller.testDelay("loadbalance", "direct-b");
        QTRY_COMPARE(state.delayTests.load(), 4);
        QTest::qWait(100);
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            QCOMPARE(state.lastDelayOutbound, "loadbalance");
        }
        // Other groups can have different histories for the same outbound.
        // Their snapshots must not be mistaken for this health-check result.
        groups.mutable_group(0)->mutable_items(1)->set_urltesttime(1700000002);
        groups.mutable_group(0)->mutable_items(1)->set_urltestdelay(13);
        state.setGroups(groups);
        QTRY_COMPARE(controller.groupModel()->find("proxy")->members[1].delay, 13);
        QVERIFY(controller.testingDelay("direct-b"));
        QVERIFY(!controller.canTestDelay("loadbalance", "direct-a"));
        groups.mutable_group(2)->mutable_items(1)->set_urltesttime(1700000003);
        groups.mutable_group(2)->mutable_items(1)->set_urltestdelay(24);
        state.setGroups(groups);
        QTRY_VERIFY(!controller.testingDelay("direct-b"));
        QVERIFY(controller.delayError("direct-b").isEmpty());
        QCOMPARE(controller.groupModel()->find("loadbalance")->members[1].delay, 24);
    }
    void outboundSelectionAndLiveMembership() {
        QTemporaryDir dir;
        Controller controller(dir.filePath("client"), QCoreApplication::applicationFilePath(),
                              "/daemon.sock", nullptr, factory());
        controller.initialize();
        QTRY_COMPARE(controller.state(), Controller::Idle);
        QCOMPARE(state.groupSubscriptions.load(), 0);
        state.setState(2);
        QTRY_VERIFY(controller.groupsReady());
        auto* model = controller.groupModel();
        QCOMPARE(model->rowCount(), 3);
        QCOMPARE(model->find("proxy")->selected, "direct-a");
        QVERIFY(model->find("loadbalance")->selected.isEmpty());
        QVERIFY(!controller.canSelectOutbound("urltest", "direct-b"));
        QVERIFY(!controller.canSelectOutbound("loadbalance", "direct-b"));
        QVERIFY(!controller.canSelectOutbound("proxy", "missing"));
        QVERIFY(!controller.canSelectOutbound("proxy", "direct-a"));
        controller.selectOutbound("urltest", "direct-b");
        QCOMPARE(state.selections.load(), 0);
        state.holdSelection = true;
        controller.selectOutbound("proxy", "direct-b");
        QTRY_COMPARE(state.selections.load(), 1);
        QVERIFY(controller.busy());
        QCOMPARE(model->find("proxy")->selected, "direct-a");
        controller.selectOutbound("proxy", "direct-b");
        QCOMPARE(state.selections.load(), 1);
        state.holdSelection = false;
        QTRY_VERIFY(!controller.busy());
        QTRY_COMPARE(model->find("proxy")->selected, "direct-b");
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            QCOMPARE(state.lastGroup, "proxy");
            QCOMPARE(state.lastOutbound, "direct-b");
        }
        state.rejectSelection = true;
        controller.selectOutbound("proxy", "direct-a");
        QTRY_VERIFY(!controller.busy());
        QVERIFY(controller.messageIsError());
        QVERIFY(controller.message().contains("outbound was removed"));
        QCOMPARE(model->find("proxy")->selected, "direct-b");
        auto updated = sampleGroups();
        updated.mutable_group(0)->mutable_items()->SwapElements(0, 1);
        updated.mutable_group(0)->mutable_items()->RemoveLast();
        updated.mutable_group(0)->set_selected("direct-b");
        state.setGroups(updated);
        QTRY_COMPARE(model->find("proxy")->members.size(), 1);
        QVERIFY(!controller.canSelectOutbound("proxy", "direct-a"));
        state.setGroups({});
        QTRY_COMPARE(model->rowCount(), 0);
        QVERIFY(controller.groupsReady());
        state.setGroups(sampleGroups());
        QTRY_COMPARE(model->rowCount(), 3);
        state.setState(0);
        QTRY_COMPARE(controller.state(), Controller::Idle);
        QVERIFY(!controller.groupsReady());
        QCOMPARE(model->rowCount(), 0);
        QVERIFY(!controller.canSelectOutbound("proxy", "direct-b"));
    }
    void groupStreamRecoveryAndUnsupportedApi() {
        state.setState(2);
        QTemporaryDir dir;
        Controller controller(dir.filePath("client"), QCoreApplication::applicationFilePath(),
                              "/daemon.sock", nullptr, factory());
        controller.initialize();
        QTRY_VERIFY(controller.groupsReady());
        state.failGroups = true;
        QTRY_VERIFY(!controller.groupsReady());
        QVERIFY(controller.online());
        QCOMPARE(controller.groupModel()->rowCount(), 0);
        QVERIFY(!controller.groupsError().isEmpty());
        state.failGroups = false;
        QTRY_VERIFY_WITH_TIMEOUT(controller.groupsReady(), 5000);
        state.unsupportedGroups = true;
        controller.refreshGroups();
        QTRY_COMPARE(controller.groupsError(), "groups unsupported");
        QVERIFY(controller.online());
        QVERIFY(!controller.groupsReady());
        QVERIFY(controller.canDisconnect());
        state.unsupportedGroups = false;
        controller.refreshGroups();
        QTRY_VERIFY(controller.groupsReady());
    }
    void uncertainSelectionIsReconciledWithoutReplay() {
        state.setState(2);
        state.uncertainSelection = true;
        QTemporaryDir dir;
        Controller controller(dir.filePath("client"), QCoreApplication::applicationFilePath(),
                              "/daemon.sock", nullptr, factory());
        controller.initialize();
        QTRY_VERIFY(controller.groupsReady());
        controller.selectOutbound("proxy", "direct-b");
        QTRY_VERIFY(!controller.online());
        QVERIFY(!controller.groupsReady());
        QCOMPARE(controller.groupModel()->rowCount(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(controller.groupsReady(), 5000);
        QCOMPARE(controller.groupModel()->find("proxy")->selected, "direct-b");
        QCOMPARE(state.selections.load(), 1);
    }
    void quittingCancelsPendingStart() {
        QTemporaryDir dir;
        Controller controller(dir.filePath("client"), QCoreApplication::applicationFilePath(),
                              "/daemon.sock", nullptr, factory());
        controller.initialize();
        QTRY_COMPARE(controller.workerStatus(), "Ready");
        QTRY_COMPARE(controller.state(), Controller::Idle);
        QVERIFY(controller.importProfile(writeProfile(dir, "{}")));
        state.holdCheck = true;
        controller.connectService();
        QTRY_COMPARE(state.checks.load(), 1);
        QSignalSpy stopped(&controller, &Controller::shutdownFinished);
        controller.shutdown();
        QVERIFY(controller.shuttingDown());
        QVERIFY(!controller.canConnect());
        controller.retryNow();
        controller.connectService();
        state.holdCheck = false;
        QTRY_COMPARE_WITH_TIMEOUT(stopped.size(), 1, 5000);
        QCOMPARE(state.starts.load(), 0);
        QTRY_COMPARE(state.activeStreams.load(), 0);
    }
    void cleanup() {
        state.holdCheck = false;
        state.holdSelection = false;
        state.holdDelay = false;
        QTRY_COMPARE_WITH_TIMEOUT(state.activeStreams.load(), 0, 3000);
    }
    void cleanupTestCase() {
        server->Shutdown();
        server.reset();
    }
};

int main(int argc, char** argv) {
    // Readiness/lifetime stand-in only: actual RPC behavior goes through the
    // in-process gRPC server. The separate backend suite tests the real Go worker.
    if (argc > 1 && std::string(argv[1]) == "worker") {
        std::cout << "READY" << std::endl;
        std::cin.ignore(std::numeric_limits<std::streamsize>::max());
        return 0;
    }
    QCoreApplication app(argc, argv);
    RpcTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_rpc.moc"
