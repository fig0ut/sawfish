#include "backend/processcpu.h"
#include "daemon/started_service.pb.h"
#include "models/connectionmodel.h"
#include "models/groupmodel.h"
#include "models/logmodel.h"
#include "models/profilestore.h"
#include <QAbstractItemModelTester>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

class CoreTest : public QObject {
    Q_OBJECT
  private slots:
    void processCpuMeasuresOwnedProcessAndResets() {
#ifdef Q_OS_LINUX
        ProcessCpu cpu;
        const auto initial = cpu.sample(QCoreApplication::applicationPid());
        QVERIFY(initial.has_value());
        QCOMPARE(*initial, 0.);
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 100) {
        }
        const auto active = cpu.sample(QCoreApplication::applicationPid());
        QVERIFY(active.has_value());
        QVERIFY(*active > 0);
        QVERIFY(!cpu.sample(0).has_value());
        QCOMPARE(cpu.sample(QCoreApplication::applicationPid()).value(), 0.);
#endif
    }
    void logTerminalFormatting_data() {
        QTest::addColumn<QString>("message");
        QTest::addColumn<QString>("expected");
        QString printable;
        for (ushort c = 0x20; c <= 0x7e; ++c)
            printable += QChar(c);
        QTest::newRow("printable-ascii") << printable << printable;
        const auto unicode = QStringLiteral(u"出口 › café Ελληνικά 😀");
        QTest::newRow("unicode") << unicode << unicode;
        QTest::newRow("plain-log") << QStringLiteral("[0000] rostra started (0.01s)")
                                   << QStringLiteral("[0000] rostra started (0.01s)");
        QTest::newRow("sgr") << QStringLiteral("before \x1b[38;2;255;0;0mred\x1b[0m after")
                             << QStringLiteral("before red after");
        QTest::newRow("c1-csi") << (QChar(0x9b) + QStringLiteral("36mcyan") + QChar(0x9b) +
                                    QStringLiteral("0m"))
                                << QStringLiteral("cyan");
        QTest::newRow("osc-bell") << QStringLiteral("\x1b]0;terminal title\x07real error")
                                  << QStringLiteral("real error");
        QTest::newRow("osc-st") << QStringLiteral(
                                       "\x1b]8;;https://example.org\x1b\\link\x1b]8;;\x1b\\")
                                << QStringLiteral("link");
        QTest::newRow("stripped-esc")
            << QStringLiteral("[36mINFO[0m[0001] field[0]") << QStringLiteral("[0001] field[0]");
    }
    void logTerminalFormatting() {
        QFETCH(QString, message);
        QFETCH(QString, expected);
        LogModel model;
        model.append({{4, message}});
        QCOMPARE(model.index(0).data(LogModel::MessageRole).toString(), expected);
        // Paused messages pass through the sanitizer again on resume.
        model.setPaused(true);
        model.append({{4, message}});
        model.setPaused(false);
        QCOMPARE(model.index(1).data(LogModel::MessageRole).toString(), expected);
    }
    void logRetentionAndTerminalFormatting() {
        LogModel model;
        QAbstractItemModelTester tester(&model,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        model.append({{4, "\x1b[36mINFO\x1b[0m[0000] rostra started (0.01s)"},
                      {4, "[36mINFO[0m[0001] field[0]"},
                      {2, "\x1b]0;terminal title\x07real error"}});
        QCOMPARE(model.index(0).data().toString(), "[INFO] [0000] rostra started (0.01s)");
        QCOMPARE(model.index(1).data().toString(), "[INFO] [0001] field[0]");
        QCOMPARE(model.index(2).data(LogModel::MessageRole).toString(), "real error");
        model.setRetentionLimit(200);
        LogBatch batch;
        for (int i = 0; i < 500; ++i)
            batch.append({4, QString::number(i)});
        model.append(batch);
        QCOMPARE(model.rowCount(), 200);
        QCOMPARE(model.index(0).data(LogModel::MessageRole).toString(), "300");
        model.setPaused(true);
        model.append(batch);
        model.setRetentionLimit(100);
        QCOMPARE(model.rowCount(), 100);
        model.setPaused(false);
        QCOMPARE(model.rowCount(), 100);
        QCOMPARE(model.index(0).data(LogModel::MessageRole).toString(), "400");
        model.setRetentionLimit(100000);
        model.append(batch);
        QCOMPARE(model.rowCount(), 600);
    }
    void connectionDeltasHistoryAndFilters() {
        ConnectionHistory history;
        rostra_rpc::ConnectionEvents batch;
        batch.set_reset(true);
        auto* event = batch.add_events();
        event->set_id("one");
        auto* c = event->mutable_connection();
        c->set_domain("example.org");
        c->set_network("tcp");
        c->set_uplinktotal(100);
        c->set_downlinktotal(200);
        history.apply(batch);
        batch.Clear();
        event = batch.add_events();
        event->set_id("one");
        event->set_type(rostra_rpc::CONNECTION_EVENT_UPDATE);
        event->set_uplinkdelta(50);
        event->set_downlinkdelta(100);
        history.apply(batch);
        history.apply(batch);
        QCOMPARE(history.snapshot().first().upload, 200);
        QCOMPARE(history.snapshot().first().download, 400);
        ConnectionModel model;
        QAbstractItemModelTester tester(&model,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        model.replace(history.snapshot());
        QPersistentModelIndex retained(model.index(0, 0));
        ConnectionFilter filter;
        filter.setSourceModel(&model);
        filter.setFilterFixedString("EXAMPLE");
        QCOMPARE(filter.rowCount(), 1);
        event->set_type(rostra_rpc::CONNECTION_EVENT_CLOSED);
        event->set_closedat(1000);
        history.apply(batch);
        model.replace(history.snapshot());
        QVERIFY(retained.isValid());
        QCOMPARE(retained.data(ConnectionModel::IdRole).toString(), "one");
        QCOMPARE(model.find("one")->uploadRate, 0);
        QCOMPARE(model.activeCount(), 0);
        QCOMPARE(filter.rowCount(), 0);
        filter.setView(ConnectionFilter::Closed);
        QCOMPARE(filter.rowCount(), 1);
        batch.Clear();
        batch.set_reset(true);
        history.apply(batch);
        model.replace(history.snapshot());
        QCOMPARE(model.rowCount(), 0);
        QVERIFY(!retained.isValid());
        for (int i = 0; i < ConnectionHistory::MaxEntries + 2; ++i) {
            event = batch.add_events();
            event->set_id(std::to_string(i));
            event->mutable_connection()->set_domain("test");
            if (i == 1 || i == 2)
                event->mutable_connection()->set_closedat(1000);
        }
        history.apply(batch);
        QCOMPARE(history.snapshot().size(), ConnectionHistory::MaxEntries);
        QCOMPARE(history.snapshot().first().id, "0"); // Evict closed rows before active rows.
    }
    void groupModelHandlesSnapshots() {
        GroupModel model;
        QAbstractItemModelTester tester(&model,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        OutboundGroups groups = {
            {"proxy", "selector", "a", true, {{"a", "direct"}, {"b", "direct", 1700000000, 23}}},
            {"balance", "loadbalance", "a", false, {{"a", "direct"}}}};
        model.replace(groups);
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.rowCount(model.index(0, 0)), 2);
        auto selected = model.findIndex("proxy", "a");
        QVERIFY(selected.data(GroupModel::SelectedRole).toBool());
        QCOMPARE(selected.parent(), model.index(0, 0));
        QCOMPARE(model.findIndex("proxy", "b").siblingAtColumn(3).data().toString(), "23 ms");
        QVERIFY(!model.findIndex("balance", "a").data(GroupModel::SelectedRole).toBool());
        QVERIFY(!model.findIndex("balance", "a").data(GroupModel::SelectableRole).toBool());
        groups[0].members.removeFirst();
        groups[0].selected = "b";
        model.replace(groups);
        QVERIFY(!model.findIndex("proxy", "a").isValid());
        QVERIFY(model.findIndex("proxy", "b").data(GroupModel::SelectedRole).toBool());
        model.replace({});
        QCOMPARE(model.rowCount(), 0);
    }
    void wireNamesAreUnchanged() {
        QCOMPARE(QString::fromStdString(
                     std::string(rostra_rpc::ServiceStatus::descriptor()->full_name())),
                 "daemon.ServiceStatus");
    }
    void logsStayBoundedAndPauseResets() {
        LogModel model;
        QAbstractItemModelTester tester(&model,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        LogBatch batch;
        for (int i = 0; i < 5000; ++i)
            batch.append({4, QString::number(i)});
        model.append(batch);
        QCOMPARE(model.rowCount(), LogModel::MaxLines);
        QCOMPARE(model.index(0).data(LogModel::MessageRole).toString(), "2000");
        model.setPaused(true);
        model.append({{2, "first new session"}}, true);
        model.append({{4, "second message"}});
        QCOMPARE(model.rowCount(), LogModel::MaxLines);
        model.setPaused(false);
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.index(0).data(LogModel::MessageRole).toString(), "first new session");
        LogFilter filter;
        filter.setSourceModel(&model);
        filter.setMaximumLevel(2);
        QCOMPARE(filter.rowCount(), 1);
        filter.setFilterFixedString("SECOND");
        QCOMPARE(filter.rowCount(), 0);
        filter.setMaximumLevel(6);
        QCOMPARE(filter.rowCount(), 1);
        batch.clear();
        for (int i = 0; i < 3000; ++i)
            batch.append({4, QString(16384, QChar('x'))});
        model.append(batch);
        QVERIFY(model.rowCount() <= 128);
    }
    void appLogsSurviveDaemonResetsAndPause() {
        LogModel model;
        QAbstractItemModelTester tester(&model,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        model.append({{4, "old core session"}});
        model.appendApplication("Profile imported.");
        model.append({{4, "new core session"}}, true);
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.index(0).data().toString(), "[APP] [INFO] Profile imported.");
        model.setPaused(true);
        model.appendApplication("Could not download profile.", true);
        model.append({{4, "intermediate session"}}, true);
        model.appendApplication("Trying again.");
        model.append({{4, "latest session"}}, true);
        QCOMPARE(model.rowCount(), 2);
        model.setPaused(false);
        QCOMPARE(model.rowCount(), 4);
        QCOMPARE(model.index(0).data().toString(), "[APP] [INFO] Profile imported.");
        QCOMPARE(model.index(1).data().toString(), "[APP] [ERROR] Could not download profile.");
        QCOMPARE(model.index(2).data().toString(), "[APP] [INFO] Trying again.");
        QCOMPARE(model.index(3).data().toString(), "[INFO] latest session");
        LogFilter filter;
        filter.setSourceModel(&model);
        filter.setFilterFixedString("[APP]");
        QCOMPARE(filter.rowCount(), 3);
        filter.setMaximumLevel(2);
        QCOMPARE(filter.rowCount(), 1);
        model.setPaused(true);
        model.appendApplication("Pending");
        model.clear();
        model.setPaused(false);
        QCOMPARE(model.rowCount(), 0);
    }
    void profileImportPreservesTextAndPermissions() {
        QTemporaryDir directory;
        const auto source = directory.filePath("original.json");
        const QByteArray content = "{\n // preserve this comment\n \"outbounds\": "
                                   "[{\"type\":\"direct\",\"tag\":\"出口\"}]\n}\n";
        QFile file(source);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(content), content.size());
        file.close();
        ProfileStore store(directory.filePath("store"));
        QAbstractItemModelTester tester(&store,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        QString error, id;
        QVERIFY2(store.load(&error), qPrintable(error));
        QVERIFY2(store.importFile(source, &id, &error), qPrintable(error));
        QCOMPARE(store.find(id)->content.toUtf8(), content);
        ProfileStore reopened(directory.filePath("store"));
        QVERIFY2(reopened.load(&error), qPrintable(error));
        QCOMPARE(reopened.find(id)->content.toUtf8(), content);
#ifdef Q_OS_UNIX
        // QFile's Windows permissions do not represent POSIX group/other bits.
        const auto permissions = QFile::permissions(directory.filePath("store/profiles.json"));
        QVERIFY(!(permissions & (QFileDevice::ReadGroup | QFileDevice::ReadOther |
                                 QFileDevice::WriteGroup | QFileDevice::WriteOther)));
#endif
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), content);
    }
    void profileNamesEditingDeletionAndStaleChecks() {
        QTemporaryDir directory;
        ProfileStore store(directory.path());
        QAbstractItemModelTester tester(&store,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        QString error, first, second, third;
        QVERIFY(store.load(&error));
        QVERIFY(
            store.add("config.json", "invalid", "https://example.org/config.json", &first, &error));
        QVERIFY(store.add("config.json", "{}", {}, &second, &error));
        QVERIFY(store.add("config.json", "{}", {}, &third, &error));
        QCOMPARE(store.find(first)->name, "config.json");
        QCOMPARE(store.find(second)->name, "config.json (1)");
        QCOMPARE(store.find(third)->name, "config.json (2)");
        store.setValidation(first, "invalid", Profile::Invalid, "invalid configuration");
        QCOMPARE(store.find(first)->validation, Profile::Invalid);
        QVERIFY(store.update(first, "Renamed", "https://example.org/new.json", "{}", &error));
        store.setValidation(first, "invalid", Profile::Invalid, "stale result");
        QCOMPARE(store.find(first)->validation, Profile::Pending);
        QVERIFY(!store.update(first, "config.json (1)", {}, "{}", &error));
        QVERIFY(!store.update(first, "Renamed", "file:///etc/passwd", "{}", &error));
        QCOMPARE(store.find(first)->url, "https://example.org/new.json");
        QVERIFY(
            store.update(first, "Renamed", {}, "", &error)); // An empty, invalid edit can be saved.
        QVERIFY(store.remove(second, &error));
        ProfileStore reopened(directory.path());
        QVERIFY(reopened.load(&error));
        QCOMPARE(reopened.rowCount(), 2);
        QCOMPARE(reopened.find(first)->name, "Renamed");
        QCOMPARE(reopened.find(first)->content, "");
        QVERIFY(!reopened.find(second));
        QVERIFY(!store.add("binary", QByteArray(1, char(0xff)), {}, &second, &error));
        QVERIFY(!store.add("large", QByteArray(ProfileStore::MaxConfigBytes + 1, 'x'), {}, &second,
                           &error));
    }
    void corruptStoreIsNeverOverwritten() {
        QTemporaryDir directory;
        QFile existing(directory.filePath("profiles.json"));
        QVERIFY(existing.open(QIODevice::WriteOnly));
        existing.write("broken");
        existing.close();
        QFile source(directory.filePath("new.json"));
        QVERIFY(source.open(QIODevice::WriteOnly));
        source.write("{}");
        source.close();
        ProfileStore store(directory.path());
        QString error, id;
        QVERIFY(!store.load(&error));
        QVERIFY(!store.importFile(source.fileName(), &id, &error));
        QVERIFY(existing.open(QIODevice::ReadOnly));
        QCOMPARE(existing.readAll(), QByteArray("broken"));
    }
    void failedWriteDoesNotImport() {
        QTemporaryDir directory;
        QFile blocker(directory.filePath("file"));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        QFile source(directory.filePath("new.json"));
        QVERIFY(source.open(QIODevice::WriteOnly));
        source.write("{}");
        source.close();
        ProfileStore store(blocker.fileName() + "/store");
        QString error, id;
        QVERIFY(store.load(&error));
        QVERIFY(!store.importFile(source.fileName(), &id, &error));
        QCOMPARE(store.rowCount(), 0);
    }
};
QTEST_MAIN(CoreTest)
#include "test_core.moc"
