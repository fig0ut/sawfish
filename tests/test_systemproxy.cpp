#include "app/controller.h"
#include "backend/proxysettings.h"
#include "backend/systemproxy.h"
#include "backendpath.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QtTest>
#include <iostream>
#ifdef Q_OS_LINUX
#include <csignal>
#include <unistd.h>
#endif

namespace {
struct Desktop {
    QMap<QString, QString> values;
    QStringList writes;
    int failWrite = -1;
    bool rejectWrites = false;
    bool run(const QString& program, const QStringList& args, QString* output, QString* error) {
        if (program == "dbus-send")
            return true;
        const bool gnome = program == "gsettings";
        const auto key = gnome ? args[1] + "/" + args[2] : args.value(args.indexOf("--key") + 1);
        if (gnome && args[0] == "writable") {
            *output = "true";
            return true;
        }
        if ((gnome && args[0] == "get") || program.startsWith("kreadconfig")) {
            *output = values.value(key, gnome ? "''" : "__sawfish_unset_f7ef826b__");
            return true;
        }
        writes << key;
        if (writes.size() == failWrite) {
            *error = "Injected desktop write failure";
            return false;
        }
        if (!rejectWrites) {
            if (args.contains("--delete"))
                values.remove(key);
            else
                values[key] = args.last();
        }
        return true;
    }
    ProxySettings::Runner runner() {
        return [this](auto program, auto args, auto output, auto error) {
            return run(program, args, output, error);
        };
    }
};

QJsonObject readFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
bool writeFile(const QString& path, const QJsonObject& object) {
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(object).toJson()) > 0 &&
           file.commit();
}
const ProxyEndpoint Mixed{"127.0.0.1", 1080, true, true};
const QString Mode = "org.gnome.system.proxy/mode";

#ifdef Q_OS_LINUX
int fakeGsettings(const QStringList& args) {
    const auto path = qEnvironmentVariable("SAWFISH_TEST_PROXY_SETTINGS");
    if (path.isEmpty() || args.size() < 4)
        return 2;
    auto values = readFile(path);
    const auto key = args[2] + "/" + args[3];
    if (args[1] == "get")
        std::cout << values.value(key).toString("''").toStdString() << std::endl;
    else if (args[1] == "writable")
        std::cout << "true" << std::endl;
    else if (args[1] == "set" && args.size() == 5) {
        values[key] = args[4];
        return writeFile(path, values) ? 0 : 1;
    } else
        return 2;
    return 0;
}
#endif
} // namespace

class SystemProxyTest : public QObject {
    Q_OBJECT
  private:
    QByteArray m_desktop, m_path, m_settings;
    std::unique_ptr<QTemporaryDir> m_session;
    QString settingsFile() const {
        return m_session->filePath("desktop.json");
    }
    bool fakeSession() {
        m_session = std::make_unique<QTemporaryDir>();
        if (!m_session->isValid() ||
            !QFile::link(QCoreApplication::applicationFilePath(), m_session->filePath("gsettings")))
            return false;
        qputenv("PATH", m_session->path().toUtf8() + ":" + m_path);
        qputenv("SAWFISH_TEST_PROXY_SETTINGS", settingsFile().toUtf8());
        return writeFile(settingsFile(), {{Mode, "'auto'"}});
    }
  private slots:
    void init() {
        m_desktop = qgetenv("XDG_CURRENT_DESKTOP");
        m_path = qgetenv("PATH");
        m_settings = qgetenv("SAWFISH_TEST_PROXY_SETTINGS");
        qputenv("XDG_CURRENT_DESKTOP", "ubuntu:GNOME");
    }
    void cleanup() {
        qputenv("XDG_CURRENT_DESKTOP", m_desktop);
        qputenv("PATH", m_path);
        qputenv("SAWFISH_TEST_PROXY_SETTINGS", m_settings);
        m_session.reset();
    }
    void profileCommentsAndRuntimeOnlyOverrides() {
#ifndef Q_MOC_RUN
        // moc's raw-string lexer treats the escaped quote in this JSONC fixture
        // as a C++ string terminator. Only the C++ compiler needs its contents.
        const QString content = R"({
          // Don't mistake URL characters for a comment.
          "log": {"output":"https://example.test/a/*b*/\\\"c",},
          "inbounds": [
            {"type":"http","listen":"127.0.0.1","listen_port":8080},
            {"type":"mixed","listen":"::","listen_port":1080,"set_system_proxy":true},
            {"type":"tun","platform":{"http_proxy":{"enabled":true,"server":"localhost"}}},
          ], /* trailing comment */
        })";
        const auto result = prepareProxyProfile(content);
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QVERIFY(result.endpoint.valid());
        QCOMPARE(result.endpoint.host, "::1");
        QVERIFY(result.endpoint.http && result.endpoint.socks);
        const auto runtime = QJsonDocument::fromJson(result.content.toUtf8()).object();
        const auto inbounds = runtime["inbounds"].toArray();
        QVERIFY(!inbounds[1].toObject()["set_system_proxy"].toBool());
        QVERIFY(!inbounds[2]
                     .toObject()["platform"]
                     .toObject()["http_proxy"]
                     .toObject()["enabled"]
                     .toBool());
        QVERIFY(content.contains("\"set_system_proxy\":true"));
        QVERIFY(runtime["log"].toObject()["output"].toString().contains("/*b*/"));
        QVERIFY(!prepareProxyProfile("{/* unterminated").error.isEmpty());
#endif
    }
    void endpointEligibility() {
        for (const auto& extra :
             {QString(",\"users\":[{\"username\":\"u\",\"password\":\"p\"}]"),
              QString(",\"tls\":{\"enabled\":true}"), QString(",\"detour\":\"other\"")}) {
            const auto result = prepareProxyProfile(
                "{\"inbounds\":[{\"type\":\"mixed\",\"listen_port\":1080" + extra + "}]}");
            QVERIFY(!result.endpoint.valid());
        }
        for (const auto& inbound :
             {QString(R"({"type":"mixed","listen_port":0})"),
              QString(R"({"type":"mixed","listen":"192.0.2.1","listen_port":1080})"),
              QString(R"({"type":"http","version":2,"listen_port":1080})"),
              QString(R"({"type":"http","version":[2,3],"listen_port":1080})")})
            QVERIFY(!prepareProxyProfile("{\"inbounds\":[" + inbound + "]}").endpoint.valid());
        const auto socks =
            prepareProxyProfile(R"({"inbounds":[{"type":"socks","listen_port":1080}]})");
        QVERIFY(socks.endpoint.valid());
        QVERIFY(socks.endpoint.socks && !socks.endpoint.http);
        QVERIFY(!ProxyEndpoint::fromJson({{"host", "example.org"}, {"port", 1080}, {"http", true}})
                     .valid());
    }
    void applyAndRestore_data() {
        QTest::addColumn<bool>("kde");
        QTest::newRow("GNOME") << false;
        QTest::newRow("KDE") << true;
    }
    void applyAndRestore() {
        QFETCH(bool, kde);
        if (kde)
            qputenv("XDG_CURRENT_DESKTOP", "KDE");
        QTemporaryDir directory;
        Desktop desktop;
        const QString mode = kde ? "ProxyType" : Mode;
        const QString host = kde ? "httpProxy" : "org.gnome.system.proxy.http/host";
        desktop.values[mode] = kde ? "2" : "'auto'";
        desktop.values[host] = kde ? "http://old.example:3128" : "'old.example'";
        desktop.values["unrelated"] = "preserve";
        const auto before = desktop.values;
        ProxySettings settings(directory.filePath("recovery.json"), desktop.runner());
        QString error;
        QVERIFY2(settings.apply(Mixed, &error), qPrintable(error));
        QCOMPARE(desktop.writes.last(), mode);
        QCOMPARE(desktop.values[mode], kde ? "1" : "'manual'");
        QVERIFY(settings.active());
        QVERIFY2(settings.restore(&error), qPrintable(error));
        QVERIFY(!settings.active());
        for (auto it = before.cbegin(); it != before.cend(); ++it)
            QCOMPARE(desktop.values[it.key()], it.value());
        if (kde)
            QCOMPARE(desktop.values, before); // Missing keys were deleted, not set to empty.
    }
    void partialFailureRollsBackAndSilentFailureIsDetected() {
        for (bool silent : {false, true}) {
            QTemporaryDir directory;
            Desktop desktop;
            desktop.values[Mode] = "'auto'";
            desktop.values["org.gnome.system.proxy.http/host"] = "'old.example'";
            desktop.failWrite = silent ? -1 : 5;
            desktop.rejectWrites = silent;
            ProxySettings settings(directory.filePath("recovery.json"), desktop.runner());
            QString error;
            QVERIFY(!settings.apply(Mixed, &error));
            QVERIFY(!error.isEmpty());
            QCOMPARE(desktop.values[Mode], "'auto'");
            QCOMPARE(desktop.values["org.gnome.system.proxy.http/host"], "'old.example'");
            QVERIFY(!settings.active());
        }
    }
    void staleJournalRestoresButKeepsExternalEdits() {
        QTemporaryDir directory;
        Desktop desktop;
        desktop.values[Mode] = "'auto'";
        QString error;
        {
            ProxySettings original(directory.filePath("recovery.json"), desktop.runner());
            QVERIFY(original.apply(Mixed, &error));
        }
        desktop.values["org.gnome.system.proxy.http/host"] = "'new-proxy.example'";
        ProxySettings recovered(directory.filePath("recovery.json"), desktop.runner());
        QVERIFY(recovered.restore(&error));
        QCOMPARE(desktop.values[Mode], "'auto'");
        QCOMPARE(desktop.values["org.gnome.system.proxy.http/host"], "'new-proxy.example'");
        QVERIFY(!recovered.active());
    }
    void failedRecoveryKeepsJournalForRetry() {
        QTemporaryDir directory;
        Desktop desktop;
        desktop.values[Mode] = "'auto'";
        QString error;
        ProxySettings settings(directory.filePath("recovery.json"), desktop.runner());
        QVERIFY(settings.apply(Mixed, &error));
        desktop.failWrite = desktop.writes.size() + 1;
        QVERIFY(!settings.restore(&error));
        QVERIFY(settings.active());
        desktop.failWrite = -1;
        QVERIFY(settings.restore(&error));
        QVERIFY(!settings.active());
        QCOMPARE(desktop.values[Mode], "'auto'");
    }
    void invalidJournalAndUnwritableJournalNeverChangeDesktop() {
        QTemporaryDir directory;
        Desktop desktop;
        const auto path = directory.filePath("recovery.json");
        QVERIFY(writeFile(path, {{"version", 999}}));
        QString error;
        ProxySettings settings(path, desktop.runner());
        QVERIFY(!settings.apply(Mixed, &error));
        QVERIFY(desktop.writes.isEmpty());
        QVERIFY(QFile::exists(path));
        ProxySettings missingParent(directory.filePath("missing/recovery.json"), desktop.runner());
        QVERIFY(!missingParent.apply(Mixed, &error));
        QVERIFY(desktop.writes.isEmpty());
    }
#ifdef Q_OS_LINUX
    void unavailableProxyIsLoggedWithToast_data() {
        QTest::addColumn<bool>("missingHelper");
        QTest::newRow("helper cannot start") << true;
        QTest::newRow("desktop unsupported") << false;
    }
    void unavailableProxyIsLoggedWithToast() {
        QFETCH(bool, missingHelper);
        QTemporaryDir directory;
        qputenv("XDG_CURRENT_DESKTOP", "unsupported-test-desktop");
        Controller controller(directory.filePath("client"), "/unused/core", {});
        QSignalSpy toasts(&controller, &Controller::notification);
        controller.initializeSystemProxy(directory.filePath("recovery"),
                                         missingHelper ? directory.filePath("missing-helper")
                                                       : QCoreApplication::applicationFilePath());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.systemProxyBusy(), 5000);
        QVERIFY(!controller.systemProxyAvailable());
        QTRY_COMPARE(controller.logModel()->rowCount(), 1);
        const auto entry = controller.logModel()->index(0, 0);
        QVERIFY(entry.data().toString().contains("[APP]"));
        QVERIFY(entry.data().toString().contains(missingHelper ? "Could not start" : "GNOME"));
        QCOMPARE(entry.data(LogModel::LevelRole).toInt(), 2);
        QCOMPARE(toasts.size(), 1);
        QVERIFY(toasts.first()[1].toBool());
        QCOMPARE(toasts.first()[0].toString(),
                 entry.data(LogModel::MessageRole).toString().remove("[APP] "));
    }
    void helperSerializesTargetsAndRestoresOnQuit() {
        QVERIFY(fakeSession());
        SystemProxy proxy;
        QSignalSpy errors(&proxy, &SystemProxy::error);
        proxy.initialize(m_session->filePath("recovery"));
        QTRY_VERIFY_WITH_TIMEOUT(proxy.available(), 5000);
        proxy.setTarget(Mixed);
        QTRY_VERIFY_WITH_TIMEOUT(proxy.active() && !proxy.busy(), 10000);
        QCOMPARE(readFile(settingsFile())[Mode].toString(), "'manual'");
        proxy.setTarget({"::1", 2090, true, false});
        proxy.setTarget({}); // Coalesce a stop while a change is in flight.
        QTRY_VERIFY_WITH_TIMEOUT(!proxy.active() && !proxy.busy(), 10000);
        QCOMPARE(readFile(settingsFile())[Mode].toString(), "'auto'");
        proxy.setTarget(Mixed);
        QTRY_VERIFY_WITH_TIMEOUT(proxy.active() && !proxy.busy(), 10000);
        proxy.shutdown();
        QTRY_VERIFY_WITH_TIMEOUT(proxy.stopped(), 5000);
        QCOMPARE(readFile(settingsFile())[Mode].toString(), "'auto'");
        QVERIFY(errors.isEmpty());
    }
    void parentDeathRestoresWithoutRelaunchingGui() {
        QVERIFY(fakeSession());
        const auto directory = m_session->filePath("recovery");
        QProcess host;
        host.start(QCoreApplication::applicationFilePath(), {"--proxy-host", directory});
        QVERIFY(host.waitForStarted(5000));
        QByteArray output;
        QTRY_VERIFY_WITH_TIMEOUT((output += host.readAllStandardOutput()).contains("active\n"),
                                 10000);
        QCOMPARE(readFile(settingsFile())[Mode].toString(), "'manual'");
        host.kill();
        QVERIFY(host.waitForFinished(5000));
        QTRY_COMPARE_WITH_TIMEOUT(readFile(settingsFile())[Mode].toString(), "'auto'", 10000);
        QTRY_VERIFY(!QFile::exists(directory + "/recovery.json"));
        QTRY_VERIFY(!QFile::exists(directory + "/owner.lock"));
    }
    void onlyOneHelperOwnsDesktop() {
        QVERIFY(fakeSession());
        SystemProxy first, second;
        const auto path = m_session->filePath("recovery");
        first.initialize(path);
        QTRY_VERIFY(first.available());
        first.setTarget(Mixed);
        QTRY_VERIFY_WITH_TIMEOUT(first.active(), 10000);
        second.initialize(path);
        QTRY_VERIFY_WITH_TIMEOUT(!second.busy(), 5000);
        QVERIFY(!second.available());
        QVERIFY(second.reason().contains("Another Sawfish"));
        QCOMPARE(readFile(settingsFile())[Mode].toString(), "'manual'");
        second.shutdown();
        first.shutdown();
        QTRY_VERIFY(first.stopped() && second.stopped());
        QCOMPARE(readFile(settingsFile())[Mode].toString(), "'auto'");
    }
    void controllerProfileChangesFailureAndCoreCrash() {
        if (!QFileInfo(desktopBackendPath()).isExecutable())
            QSKIP("Core executable unavailable");
        QVERIFY(fakeSession());
        Controller controller(m_session->filePath("client"), desktopBackendPath(), {});
        controller.initializeSystemProxy(m_session->filePath("recovery"));
        controller.initialize();
        QTRY_VERIFY_WITH_TIMEOUT(controller.systemProxyAvailable() && controller.online() &&
                                     controller.workerStatus() == "Ready",
                                 10000);
        QString first, second, error;
        // Reserve ports to avoid collisions with concurrently running integration tests.
        QTcpServer port1, port2;
        QVERIFY(port1.listen(QHostAddress::LocalHost, 0));
        QVERIFY(port2.listen(QHostAddress::LocalHost, 0));
        const auto profile = [](int port) {
            return QString(
                       R"({"inbounds":[{"type":"mixed","listen":"127.0.0.1","listen_port":%1,"set_system_proxy":true}],"outbounds":[{"type":"direct"}]})")
                .arg(port)
                .toUtf8();
        };
        const auto firstPort = port1.serverPort(), secondPort = port2.serverPort();
        QVERIFY(controller.profiles()->add("First", profile(firstPort), {}, &first, &error));
        QVERIFY(controller.profiles()->add("Second", profile(secondPort), {}, &second, &error));
        port1.close();
        port2.close();
        controller.selectProfile(first);
        controller.setSystemProxyEnabled(true);
        QVERIFY(controller.systemProxyEnabled());
        controller.connectService();
        QTRY_VERIFY_WITH_TIMEOUT(controller.state() == Controller::Running &&
                                     controller.systemProxyActive() && !controller.busy(),
                                 10000);
        QCOMPARE(readFile(settingsFile())["org.gnome.system.proxy.http/port"].toString(),
                 QString::number(firstPort));
        controller.selectProfile(second);
        QTRY_VERIFY_WITH_TIMEOUT(controller.activeId() == second &&
                                     controller.systemProxyActive() && !controller.busy() &&
                                     !controller.systemProxyBusy(),
                                 10000);
        QCOMPARE(readFile(settingsFile())["org.gnome.system.proxy.http/port"].toString(),
                 QString::number(secondPort));
        QVERIFY(controller.profiles()->find(first)->content.contains("\"set_system_proxy\":true"));
        // A failed replacement validation must leave the live proxy unchanged.
        QVERIFY(controller.saveProfile(second, "Second", {}, "{invalid}"));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 10000);
        QVERIFY(controller.systemProxyActive());
        QCOMPARE(readFile(settingsFile())["org.gnome.system.proxy.http/port"].toString(),
                 QString::number(secondPort));
        controller.disconnectService();
        QTRY_VERIFY_WITH_TIMEOUT(controller.state() == Controller::Idle && !controller.busy() &&
                                     !controller.systemProxyActive() &&
                                     !controller.systemProxyBusy(),
                                 10000);
        QCOMPARE(readFile(settingsFile())[Mode].toString(), "'auto'");
        QVERIFY(controller.systemProxyEnabled()); // Preference survives a disconnect.
        // A real listen failure must not install a desktop proxy to a dead core.
        controller.selectProfile(first);
        QVERIFY(port1.listen(QHostAddress::LocalHost, firstPort));
        controller.connectService();
        QTRY_VERIFY_WITH_TIMEOUT(controller.state() == Controller::Failed && !controller.busy(),
                                 10000);
        QVERIFY(!controller.systemProxyActive());
        QCOMPARE(readFile(settingsFile())[Mode].toString(), "'auto'");
        port1.close();
        controller.connectService();
        QTRY_VERIFY_WITH_TIMEOUT(controller.state() == Controller::Running &&
                                     controller.systemProxyActive() && !controller.busy(),
                                 10000);
        const auto pid = controller.coreProcessId();
        QVERIFY(pid > 0);
        QCOMPARE(::kill(pid_t(pid), SIGKILL), 0);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.systemProxyActive(), 10000);
        QTRY_COMPARE(readFile(settingsFile())[Mode].toString(), "'auto'");
        QSignalSpy stopped(&controller, &Controller::shutdownFinished);
        controller.shutdown();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.size(), 1, 10000);
        QVERIFY(!QFile::exists(m_session->filePath("recovery/recovery.json")));
    }
#endif
};

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
#ifdef Q_OS_LINUX
    if (QFileInfo(QString::fromLocal8Bit(argv[0])).fileName() == "gsettings")
        return fakeGsettings(app.arguments());
    if (app.arguments().value(1) == "--system-proxy-helper")
        return runSystemProxyHelper();
    if (app.arguments().value(1) == "--proxy-host") {
        SystemProxy proxy;
        QObject::connect(&proxy, &SystemProxy::changed, &app, [&] {
            if (proxy.available() && !proxy.active() && !proxy.busy())
                proxy.setTarget(Mixed);
            if (proxy.active())
                std::cout << "active" << std::endl;
        });
        proxy.initialize(app.arguments().value(2));
        return app.exec();
    }
#endif
    SystemProxyTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_systemproxy.moc"
