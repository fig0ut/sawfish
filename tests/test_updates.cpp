#include "app/controller.h"
#include "backend/coreupdater.h"
#include "backendpath.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrlQuery>
#include <QtTest>

class Releases : public QTcpServer {
  public:
    QByteArray binary;
    QJsonArray releases;
    QMap<int, QJsonArray> pages;
    bool holdDownload = false;
    QList<QPointer<QTcpSocket>> held;
    Releases() {
        listen(QHostAddress::LocalHost, 0);
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto* socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                auto request = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, request] {
                    request->append(socket->readAll());
                    if (!request->contains("\r\n\r\n"))
                        return;
                    const bool asset = request->startsWith("GET /core ");
                    if (asset && holdDownload) {
                        held.append(socket);
                        return;
                    }
                    const QUrlQuery query(QUrl(QString::fromUtf8(request->split(' ').value(1))));
                    const int page = query.queryItemValue("page").toInt();
                    const auto body = asset ? binary
                                            : QJsonDocument(pages.value(page, releases))
                                                  .toJson(QJsonDocument::Compact);
                    socket->write(
                        "HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) +
                        "\r\nConnection: close\r\n\r\n");
                    socket->write(body);
                    socket->disconnectFromHost();
                });
            }
        });
    }
    QUrl url(QString path = "/releases") const {
        return QUrl(QString("http://127.0.0.1:%1%2").arg(serverPort()).arg(path));
    }
    QJsonObject release(QString version, bool prerelease = false) const {
        return {
            {"tag_name", version},
            {"prerelease", prerelease},
            {"draft", false},
            {"published_at", "2026-10-28T00:00:00Z"},
            {"assets",
             QJsonArray{QJsonObject{
                 {"name", CoreUpdater::platformAsset()},
                 {"state", "uploaded"},
                 {"size", binary.size()},
                 {"digest",
                  "sha256:" +
                      QString::fromLatin1(
                          QCryptographicHash::hash(binary, QCryptographicHash::Sha256).toHex())},
                 {"browser_download_url", url("/core").toString()}}}}};
    }
};

class UpdateTest : public QObject {
    Q_OBJECT
  private:
    QString backend() const {
        return desktopBackendPath();
    }
    QString version() const {
        QProcess process;
        process.start(backend(), {"version"});
        if (!process.waitForFinished(10000))
            return {};
        return QJsonDocument::fromJson(process.readAllStandardOutput())
            .object()
            .value("version")
            .toString();
    }
  private slots:
    void channelAndAssetValidation() {
        Releases server;
        server.binary = "a core";
        auto stable = server.release("v1.0.0");
        auto beta = server.release("v1.1.0-beta.1", true);
        QVERIFY(CoreUpdater::parseRelease(stable, CoreUpdater::Stable, CoreUpdater::platformAsset(),
                                          true)
                    .valid());
        QVERIFY(!CoreUpdater::parseRelease(stable, CoreUpdater::Prerelease,
                                           CoreUpdater::platformAsset(), true)
                     .valid());
        QVERIFY(CoreUpdater::parseRelease(beta, CoreUpdater::Prerelease,
                                          CoreUpdater::platformAsset(), true)
                    .valid());
        QVERIFY(!CoreUpdater::parseRelease(beta, CoreUpdater::Stable, CoreUpdater::platformAsset(),
                                           true)
                     .valid());
        QVERIFY(
            !CoreUpdater::parseRelease(stable, CoreUpdater::Stable, CoreUpdater::platformAsset())
                 .valid()); // HTTP isn't a production source.
        stable["draft"] = true;
        QVERIFY(!CoreUpdater::parseRelease(stable, CoreUpdater::Stable,
                                           CoreUpdater::platformAsset(), true)
                     .valid());
        stable["draft"] = false;
        auto asset = stable["assets"].toArray().first().toObject();
        asset["digest"] = "";
        stable["assets"] = QJsonArray{asset};
        QVERIFY(!CoreUpdater::parseRelease(stable, CoreUpdater::Stable,
                                           CoreUpdater::platformAsset(), true)
                     .valid());
        asset["digest"] = "sha256:" + QString(64, 'a');
        asset["name"] = "rostra-linux-amd64.tar.gz";
        stable["assets"] = QJsonArray{asset};
        QVERIFY(!CoreUpdater::parseRelease(stable, CoreUpdater::Stable,
                                           CoreUpdater::platformAsset(), true)
                     .valid());
    }
    void channelsPersistAndEmptyReleasesAreClear() {
        QTemporaryDir directory;
        Controller controller(directory.path(), "/old/core", {});
        Releases server;
        server.binary = "a core";
        server.releases = {server.release("v1.1-beta", true), server.release("v1.0")};
        CoreUpdater updater(&controller, directory.path(), server.url());
        updater.check();
        QTRY_COMPARE(updater.state(), CoreUpdater::Available);
        QCOMPARE(updater.availableVersion(), "v1.0");
        updater.setChannel(CoreUpdater::Prerelease);
        updater.check();
        QTRY_COMPARE(updater.state(), CoreUpdater::Available);
        QCOMPARE(updater.availableVersion(), "v1.1-beta");
        QSettings saved(directory.filePath("settings.ini"), QSettings::IniFormat);
        QCOMPARE(saved.value("core/channel").toString(), "prerelease");
        server.releases = {};
        updater.check();
        QTRY_COMPARE(updater.state(), CoreUpdater::Idle);
        QVERIFY(updater.status().contains("No compatible desktop"));
    }
    void badHashDoesNotReplaceCore() {
        QTemporaryDir directory;
        Controller controller(directory.path(), "/old/core", {});
        Releases server;
        server.binary = "expected";
        server.releases = {server.release("v1")};
        server.binary = "tampered";
        CoreUpdater updater(&controller, directory.path(), server.url());
        updater.check();
        QTRY_COMPARE(updater.state(), CoreUpdater::Available);
        updater.install();
        QTRY_COMPARE(updater.state(), CoreUpdater::Failed);
        QVERIFY(updater.status().contains("SHA-256"));
        QCOMPARE(controller.executable(), "/old/core");
        QVERIFY(!controller.busy());
        QSettings saved(directory.filePath("settings.ini"), QSettings::IniFormat);
        QVERIFY(!saved.contains("core/pendingPrevious"));
    }
    void newestPublicationAcrossPages() {
        QTemporaryDir directory;
        Controller controller(directory.path(), "/old/core", {});
        Releases server;
        server.binary = "core";
        auto older = server.release("v1");
        older["published_at"] = "2026-10-27T00:00:00Z";
        QJsonArray first;
        for (int i = 0; i < 100; ++i)
            first.append(older);
        server.pages.insert(1, first);
        server.pages.insert(2, QJsonArray{server.release("v2")});
        CoreUpdater updater(&controller, directory.path(), server.url());
        updater.check();
        QTRY_COMPARE(updater.state(), CoreUpdater::Available);
        QCOMPARE(updater.availableVersion(), "v2");
    }
    void cancelDownloadIgnoresLateReply() {
        QTemporaryDir directory;
        Controller controller(directory.path(), "/old/core", {});
        Releases server;
        server.binary = "expected";
        server.releases = {server.release("v1")};
        server.holdDownload = true;
        CoreUpdater updater(&controller, directory.path(), server.url());
        updater.check();
        QTRY_COMPARE(updater.state(), CoreUpdater::Available);
        updater.install();
        QTRY_VERIFY(!server.held.isEmpty());
        QVERIFY(controller.busy());
        updater.cancel();
        QVERIFY(!controller.busy());
        QCOMPARE(updater.state(), CoreUpdater::Idle);
        QCOMPARE(controller.executable(), "/old/core");
    }
    void interruptedActivationRestoresPreviousPath() {
        QTemporaryDir directory;
        QSettings settings(directory.filePath("settings.ini"), QSettings::IniFormat);
        settings.setValue("backend/executable", "/new/core");
        settings.setValue("core/pendingPrevious", "/old/core");
        settings.setValue("profile/selected", "unchanged");
        CoreUpdater::recoverInterrupted(settings);
        QCOMPARE(settings.value("backend/executable").toString(), "/old/core");
        QCOMPARE(settings.value("profile/selected").toString(), "unchanged");
        QVERIFY(!settings.contains("core/pendingPrevious"));
    }
    void realCoreUpgrade_data() {
        QTest::addColumn<bool>("running");
        QTest::newRow("idle") << false;
        QTest::newRow("connected") << true;
    }
    void realCoreUpgrade() {
        QFETCH(bool, running);
        QVERIFY2(QFileInfo::exists(backend()),
                 "Run scripts/build.sh or scripts/build.ps1, or set SAWFISH_TEST_BACKEND.");
        const auto coreVersion = version();
        QVERIFY(!coreVersion.isEmpty());
        QTemporaryDir directory;
        Controller controller(directory.path(), backend(), {});
        controller.initialize();
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), Controller::Idle, 15000);
        QTRY_COMPARE(controller.workerStatus(), "Ready");
        QFile config(directory.filePath("profile.json"));
        QVERIFY(config.open(QIODevice::WriteOnly));
        config.write("{\"outbounds\":[{\"type\":\"direct\"}]}");
        config.close();
        QVERIFY(controller.importProfile(config.fileName()));
        if (running) {
            controller.connectService();
            QTRY_COMPARE(controller.state(), Controller::Running);
            QTRY_VERIFY(!controller.busy());
        }
        const auto oldPid = controller.coreProcessId();
        Releases server;
        QFile binary(backend());
        QVERIFY(binary.open(QIODevice::ReadOnly));
        server.binary = binary.readAll();
        server.releases = {server.release(coreVersion)};
        CoreUpdater updater(&controller, directory.path(), server.url());
        bool triedCancel = false;
        connect(&updater, &CoreUpdater::changed, this, [&] {
            if (updater.state() == CoreUpdater::Installing && !triedCancel) {
                triedCancel = true;
                updater.cancel();
                QCOMPARE(updater.state(), CoreUpdater::Installing);
            }
        });
        updater.check();
        QTRY_COMPARE(updater.state(), CoreUpdater::Available);
        updater.install();
        QTRY_VERIFY_WITH_TIMEOUT(
            updater.state() == CoreUpdater::Idle || updater.state() == CoreUpdater::Failed, 60000);
        QCOMPARE(updater.status(), QString("Rostra %1 installed.").arg(coreVersion));
        QVERIFY(triedCancel);
        QVERIFY(controller.executable() != backend());
        QVERIFY(controller.coreProcessId() != oldPid);
        QCOMPARE(controller.state(), running ? Controller::Running : Controller::Idle);
        QVERIFY(!controller.busy());
        QSettings saved(directory.filePath("settings.ini"), QSettings::IniFormat);
        QVERIFY(!saved.contains("core/pendingPrevious"));
        QCOMPARE(saved.value("backend/executable").toString(), controller.executable());
        updater.check();
        QTRY_COMPARE(updater.state(), CoreUpdater::Idle);
        QVERIFY(updater.status().contains("already installed"));
        QSignalSpy stopped(&controller, &Controller::shutdownFinished);
        controller.shutdown();
        QTRY_COMPARE(stopped.size(), 1);
    }
#ifdef Q_OS_LINUX
    void rejectedCandidateKeepsCurrentCore_data() {
        QTest::addColumn<QString>("info");
        QTest::addColumn<int>("checkExit");
        QTest::newRow("api") << "{\"version\":\"1\",\"api_version\":99}" << 0;
        QTest::newRow("missing-api") << "{\"version\":\"1\"}" << 0;
        QTest::newRow("text-version") << "sing-box-daemon version 1" << 0;
        QTest::newRow("invalid-json") << "invalid" << 0;
        QTest::newRow("mismatched-version") << "{\"version\":\"2\",\"api_version\":4}" << 0;
        QTest::newRow("config") << "{\"version\":\"1\",\"api_version\":4}" << 1;
    }
    void rejectedCandidateKeepsCurrentCore() {
        QFETCH(QString, info);
        QFETCH(int, checkExit);
        QTemporaryDir directory;
        Controller controller(directory.path(), "/old/core", {});
        Releases server;
        server.binary = QString("#!/bin/sh\ncase \"$1\" in\nversion) echo "
                                "'%1';;\ncheck) cat >/dev/null; exit %2;;\nesac\n")
                            .arg(info)
                            .arg(checkExit)
                            .toUtf8();
        server.releases = {server.release("v1")};
        CoreUpdater updater(&controller, directory.path(), server.url());
        updater.check();
        QTRY_COMPARE(updater.state(), CoreUpdater::Available);
        updater.install();
        QTRY_COMPARE(updater.state(), CoreUpdater::Failed);
        QCOMPARE(controller.executable(), "/old/core");
        QVERIFY(!controller.busy());
    }
    void failedActivationRollsBack_data() {
        realCoreUpgrade_data();
    }
    void failedActivationRollsBack() {
        QFETCH(bool, running);
        QTemporaryDir directory;
        Controller controller(directory.path(), backend(), {});
        controller.initialize();
        QTRY_COMPARE(controller.state(), Controller::Idle);
        QTRY_COMPARE(controller.workerStatus(), "Ready");
        if (running) {
            QFile config(directory.filePath("profile.json"));
            QVERIFY(config.open(QIODevice::WriteOnly));
            config.write("{\"outbounds\":[{\"type\":\"direct\"}]}");
            config.close();
            QVERIFY(controller.importProfile(config.fileName()));
            controller.connectService();
            QTRY_COMPARE(controller.state(), Controller::Running);
            QTRY_VERIFY(!controller.busy());
        }
        Releases server;
        server.binary = "#!/bin/sh\ncase \"$1\" in\nversion) echo "
                        "'{\"version\":\"1\",\"api_version\":4}';;\ncheck) cat >/dev/null;;\n*) "
                        "exit 1;;\nesac\n";
        server.releases = {server.release("v1")};
        CoreUpdater updater(&controller, directory.path(), server.url());
        updater.check();
        QTRY_COMPARE(updater.state(), CoreUpdater::Available);
        updater.install();
        QTRY_COMPARE_WITH_TIMEOUT(updater.state(), CoreUpdater::Failed, 15000);
        QVERIFY2(updater.status().contains("previous core was restored"),
                 qPrintable(updater.status()));
        QCOMPARE(controller.executable(), backend());
        QCOMPARE(controller.state(), running ? Controller::Running : Controller::Idle);
        QVERIFY(!controller.busy());
        QSettings saved(directory.filePath("settings.ini"), QSettings::IniFormat);
        QVERIFY(!saved.contains("core/pendingPrevious"));
    }
#endif
};
QTEST_GUILESS_MAIN(UpdateTest)
#include "test_updates.moc"
