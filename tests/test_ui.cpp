#include "app/controller.h"
#include "fakebackend.h"
#include "mainwindow.h"
#include "widgets/applicationicon.h"
#include "widgets/controls.h"
#include "widgets/dashboard.h"
#include "widgets/icons.h"
#include <QAction>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopeGuard>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTableView>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>
#include <cmath>
#include <iostream>
#include <limits>

class UiTest : public QObject {
    Q_OBJECT
  private slots:
    void init() {
        QTest::failOnWarning();
    }
    void systemProxyPreferenceCanBeDisabledWithoutDesktopSupport() {
#ifdef Q_OS_LINUX
        QTemporaryDir directory;
        QSettings settings(directory.filePath("settings.ini"), QSettings::IniFormat);
        settings.setValue("proxy/enabled", true);
        settings.sync();
        Controller controller(directory.path(), "/missing/rostra", {});
        MainWindow window(&controller);
        window.resize(window.minimumSize());
        window.show();
        controller.initialize();
        auto* toggle = window.findChild<QAbstractButton*>("systemProxySwitch");
        QVERIFY(toggle);
        QCOMPARE(toggle->accessibleName(), "System proxy");
        QTRY_VERIFY(toggle->visibleRegion().contains(toggle->rect().center()));
        QVERIFY(toggle->isChecked());
        QVERIFY(toggle->isEnabled());
        QVERIFY(!controller.systemProxyAvailable());
        toggle->click();
        QVERIFY(!toggle->isChecked());
        QVERIFY(!toggle->isEnabled());
        QVERIFY(toggle->visibleRegion().contains(toggle->rect().center()));
        QVERIFY(!controller.systemProxyEnabled());
        settings.sync();
        QVERIFY(!settings.value("proxy/enabled").toBool());
#endif
    }
    void switchesShareVisibleThemeColors() {
        const auto luminance = [](const QColor& color) {
            const auto linear = [](double channel) {
                return channel <= 0.04045 ? channel / 12.92
                                          : std::pow((channel + 0.055) / 1.055, 2.4);
            };
            return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) +
                   0.0722 * linear(color.blueF());
        };
        const auto contrast = [&](const QColor& first, const QColor& second) {
            const auto a = luminance(first), b = luminance(second);
            return (qMax(a, b) + 0.05) / (qMin(a, b) + 0.05);
        };
        for (bool dark : {false, true}) {
            ToggleSwitch toggle;
            ConnectionSwitch connection;
            connection.resize(connection.sizeHint());
            auto palette = toggle.palette();
            const QColor background(dark ? "#191b28" : "#efefef");
            palette.setColor(QPalette::Window, background);
            palette.setColor(QPalette::WindowText, QColor(dark ? "#c8cfdb" : "#24262a"));
            // Reproduce themes whose surface/disabled roles have almost no contrast.
            palette.setColor(QPalette::Base, background);
            palette.setColor(QPalette::Mid, background);
            palette.setColor(QPalette::Button, background);
            palette.setColor(QPalette::Disabled, QPalette::WindowText, background);
            palette.setColor(QPalette::Highlight, QColor(dark ? "#d000db" : "#1673d6"));
            palette.setColor(QPalette::HighlightedText, Qt::white);
            toggle.setPalette(palette);
            connection.setPalette(palette);
            for (bool enabled : {false, true}) {
                toggle.setEnabled(enabled);
                connection.setEnabled(enabled);
                for (bool checked : {false, true}) {
                    toggle.setChecked(checked);
                    connection.setChecked(checked);
                    for (qreal scale : {1., 1.25, 2.}) {
                        const auto render = [&](QWidget& widget) {
                            QImage image(QSize(qRound(widget.width() * scale),
                                               qRound(widget.height() * scale)),
                                         QImage::Format_ARGB32_Premultiplied);
                            image.setDevicePixelRatio(scale);
                            image.fill(background);
                            widget.render(&image);
                            return image;
                        };
                        const auto image = render(toggle);
                        const auto connectionImage = render(connection);
                        const auto track = image.pixelColor(qRound(27 * scale), qRound(15 * scale));
                        const auto thumb = image.pixelColor(qRound((checked ? 40 : 14) * scale),
                                                            qRound(15 * scale));
                        QVERIFY2(contrast(thumb, track) >= (enabled ? 3.0 : 1.7),
                                 "The thumb must remain distinguishable from the track.");
                        QVERIFY2(contrast(track, background) >= (enabled ? 1.7 : 1.25),
                                 "The track must remain visible against the page.");
                        QCOMPARE(connectionImage.pixelColor(qRound(46 * scale), qRound(24 * scale)),
                                 track);
                        // Sample below the play/pause icon, inside the connection thumb.
                        QCOMPARE(connectionImage.pixelColor(qRound((checked ? 69 : 23) * scale),
                                                            qRound(36 * scale)),
                                 thumb);
                        const auto screenshot = qEnvironmentVariable("SAWFISH_SWITCH_SCREENSHOT");
                        if (!screenshot.isEmpty() && scale == 2.) {
                            QImage preview(324, 96, QImage::Format_ARGB32_Premultiplied);
                            preview.setDevicePixelRatio(scale);
                            preview.fill(background);
                            QPainter painter(&preview);
                            painter.drawImage(QPoint(0, 0), connectionImage);
                            painter.drawImage(QPoint(108, 9), image);
                            painter.end();
                            QVERIFY(preview.save(screenshot + (dark ? ".dark" : ".light") +
                                                 (enabled ? ".enabled" : ".disabled") +
                                                 (checked ? ".on.png" : ".off.png")));
                        }
                    }
                }
            }
        }
    }
    void libraryIconsRenderWithThemeAndPixelDensity() {
        const QList<QColor> colors{QColor("#182030"), QColor("#e8eef8"), QColor("#d000db")};
        for (qreal scale : {1., 1.25, 2.}) {
            for (const auto& color : colors) {
                for (int name = Icons::Plus; name <= Icons::Check; ++name) {
                    QImage image(QSize(qRound(32 * scale), qRound(32 * scale)),
                                 QImage::Format_ARGB32_Premultiplied);
                    image.setDevicePixelRatio(scale);
                    image.fill(Qt::transparent);
                    QPainter painter(&image);
                    Icons::paint(painter, Icons::Name(name), QRectF(4, 4, 24, 24), color);
                    painter.end();
                    int ink = 0;
                    for (int y = 0; y < image.height(); ++y)
                        for (int x = 0; x < image.width(); ++x) {
                            const auto pixel = image.pixelColor(x, y);
                            if (pixel.alpha() < 128)
                                continue;
                            ++ink;
                            QVERIFY(qAbs(pixel.red() - color.red()) <= 2);
                            QVERIFY(qAbs(pixel.green() - color.green()) <= 2);
                            QVERIFY(qAbs(pixel.blue() - color.blue()) <= 2);
                            QVERIFY(x > 0 && y > 0 && x < image.width() - 1 &&
                                    y < image.height() - 1);
                        }
                    QVERIFY2(ink > 8, qPrintable(QString("Missing library icon %1").arg(name)));
                }
            }
        }
        const auto path = qEnvironmentVariable("SAWFISH_LIBRARY_ICONS_SCREENSHOT");
        if (!path.isEmpty()) {
            QImage image(7 * 100, 4 * 88, QImage::Format_ARGB32_Premultiplied);
            QPainter painter(&image);
            const QStringList labels{"Add",     "Menu",  "Close", "Edit",     "Save",
                                     "Refresh", "Pause", "Play",  "Settings", "Copy",
                                     "Clear",   "Down",  "Right", "Selected"};
            for (int theme = 0; theme < 2; ++theme) {
                const QColor background(theme ? "#171b25" : "#f5f6f8");
                const QColor foreground(theme ? "#e8eef8" : "#182030");
                painter.fillRect(QRect(0, theme * 176, 700, 176), background);
                for (int name = Icons::Plus; name <= Icons::Check; ++name) {
                    const QPoint origin((name % 7) * 100, theme * 176 + (name / 7) * 88);
                    Icons::paint(painter, Icons::Name(name),
                                 QRectF(origin + QPoint(34, 10), QSize(32, 32)), foreground);
                    painter.setPen(foreground);
                    painter.drawText(QRect(origin + QPoint(0, 52), QSize(100, 24)), Qt::AlignCenter,
                                     labels[name]);
                }
            }
            painter.end();
            QVERIFY(image.save(path));
        }
    }
    void idleTrafficRingHasEqualSegments() {
        QTemporaryDir directory;
        Controller controller(directory.filePath("client"), "/missing/worker", "/daemon.sock");
        MainWindow window(&controller);
        window.show();
        QWidget* ring = nullptr;
        for (auto* card : window.findChild<Dashboard*>()->findChildren<QWidget*>("dashboardCard"))
            if (card->property("widgetKind").toInt() == Dashboard::TotalTraffic)
                ring = card;
        QVERIFY(ring);
        auto palette = ring->palette();
        palette.setColor(QPalette::Base, QColor("#161924"));
        palette.setColor(QPalette::Text, QColor("#d0d6e0"));
        palette.setColor(QPalette::Highlight, QColor("#d000db"));
        ring->setPalette(palette);
        for (int diameter : {94, 130}) {
            ring->resize(350, diameter + 110);
            for (qreal scale : {1., 1.25, 2.}) {
                QImage image(QSize(qRound(ring->width() * scale), qRound(ring->height() * scale)),
                             QImage::Format_ARGB32_Premultiplied);
                image.setDevicePixelRatio(scale);
                image.fill(Qt::transparent);
                ring->render(&image);
                int upload = 0, download = 0;
                for (int y = qCeil(42 * scale); y < qFloor((54 + diameter) * scale); ++y)
                    for (int x = qCeil((175 - diameter / 2 - 6) * scale);
                         x < qFloor((175 + diameter / 2 + 6) * scale); ++x) {
                        const auto color = image.pixelColor(x, y);
                        if (color.saturation() > 200 && color.value() > 150) {
                            if (color.hue() > 260)
                                ++upload;
                            else if (color.hue() < 160)
                                ++download;
                        }
                    }
                QVERIFY(upload > 100 && download > 100);
                QVERIFY2(qAbs(upload - download) < (upload + download) * .02,
                         "An empty traffic ring must give each color half the visible area.");
            }
        }
        const auto path = qEnvironmentVariable("SAWFISH_RING_SCREENSHOT");
        if (!path.isEmpty())
            QVERIFY(ring->grab().save(path));
    }
    void dashboardEditingPersists() {
        QTemporaryDir directory;
        QVariantList saved;
        {
            Controller controller(directory.filePath("client"), "/missing/worker", "/daemon.sock");
            MainWindow window(&controller);
            window.show();
            auto* dashboard = window.findChild<Dashboard*>();
            auto* edit = window.findChild<QToolButton*>("editWidgetsButton");
            QVERIFY(dashboard);
            QCOMPARE(dashboard->savedLayout().size(), 4);
            auto* card = dashboard->findChildren<QWidget*>("dashboardCard").first();
            const auto before = dashboard->savedLayout();
            QTest::keyClick(card, Qt::Key_Down);
            QCOMPARE(dashboard->savedLayout(), before);
            QTest::mouseClick(edit, Qt::LeftButton);
            QVERIFY(dashboard->editing());
            QCOMPARE(edit->accessibleName(), "Save widgets");
            QVERIFY(!window.findChild<QPushButton*>("finishEditingWidgetsButton"));
            QTest::keyClick(card, Qt::Key_Down);
            QCOMPARE(dashboard->savedLayout().first().toMap()["y"].toInt(), 1);
            QTest::keyClick(card, Qt::Key_Right, Qt::ShiftModifier);
            QCOMPARE(dashboard->savedLayout().first().toMap()["width"].toInt(), 3);
            // Dragging and corner resizing use the same grid/collision rules.
            QTest::mousePress(card, Qt::LeftButton, Qt::NoModifier, QPoint(24, 24));
            QTest::mouseMove(card, QPoint(24, 132));
            QTest::mouseRelease(card, Qt::LeftButton, Qt::NoModifier, QPoint(24, 24));
            QCOMPARE(dashboard->savedLayout().first().toMap()["y"].toInt(), 2);
            const auto corner = card->rect().bottomRight() - QPoint(8, 8);
            QTest::mousePress(card, Qt::LeftButton, Qt::NoModifier, corner);
            QTest::mouseMove(card, corner + QPoint(0, 108));
            QTest::mouseRelease(card, Qt::LeftButton, Qt::NoModifier, corner);
            QCOMPARE(dashboard->savedLayout().first().toMap()["height"].toInt(), 3);
            card->findChild<QToolButton*>("removeWidgetButton")->click();
            QVERIFY(!dashboard->contains(Dashboard::Upload));
            window.findChild<QAction*>("addWidget0")->trigger();
            QVERIFY(dashboard->contains(Dashboard::Upload));
            saved = dashboard->savedLayout();
            QCOMPARE(saved.size(), 4);
            edit->click();
            QVERIFY(!dashboard->editing());
            QCOMPARE(edit->accessibleName(), "Edit widgets");
            QCOMPARE(controller.dashboardLayout(), saved);
            edit->click();
            QVERIFY(dashboard->editing());
            auto* navigation = window.findChild<QListWidget*>("navigation");
            navigation->setCurrentRow(3);
            QVERIFY(!dashboard->editing());
            QVERIFY(!edit->isChecked());
            QCOMPARE(edit->accessibleName(), "Edit widgets");
            navigation->setCurrentRow(0);
            QVERIFY(!window.findChild<QToolButton*>("addWidgetButton")->isVisible());
            QCOMPARE(dashboard->savedLayout(), saved);
        }
        Controller reopened(directory.filePath("client"), "/missing/worker", "/daemon.sock");
        MainWindow window(&reopened);
        QCOMPARE(window.findChild<Dashboard*>()->savedLayout(), saved);
    }
    void idleDashboardAndAdditionalWidgets() {
        QTemporaryDir directory;
        {
            Controller controller(directory.filePath("client"), "/missing/worker", "/daemon.sock");
            MainWindow window(&controller);
            window.show();
            auto* dashboard = window.findChild<Dashboard*>();
            auto* add = window.findChild<QToolButton*>("addWidgetButton");
            QVERIFY(add->text().isEmpty());
            QCOMPARE(add->accessibleName(), "Add widget");
            window.findChild<QToolButton*>("editWidgetsButton")->click();
            for (int kind : {Dashboard::Cpu, Dashboard::Memory, Dashboard::Traffic}) {
                window.findChild<QAction*>(QStringLiteral("addWidget%1").arg(kind))->trigger();
                QVERIFY(dashboard->contains(Dashboard::Kind(kind)));
            }
            QCOMPARE(dashboard->savedLayout().size(), 7);
            window.findChild<QToolButton*>("editWidgetsButton")->click();
            for (auto* card : dashboard->findChildren<QWidget*>("dashboardCard")) {
                if (card->property("widgetKind").toInt() == Dashboard::TotalTraffic)
                    continue;
                const auto image = card->grab().toImage();
                const int y = card->height() - 30;
                const auto color = image.pixelColor(card->width() / 2, y);
                QVERIFY(color != card->palette().color(QPalette::Base));
                int same = 0;
                for (int x = 18; x < card->width() - 18; ++x)
                    same += image.pixelColor(x, y) == color;
                QVERIFY2(same >= card->width() - 40,
                         "An idle line chart must start with a full-width baseline.");
            }
            const auto path = qEnvironmentVariable("SAWFISH_WIDGETS_SCREENSHOT");
            if (!path.isEmpty()) {
                window.resize(1100, 1100);
                dashboard->findChild<QScrollArea*>()->verticalScrollBar()->setValue(0);
                QTest::qWait(30);
                QVERIFY(window.grab().save(path + ".idle.png"));
            }
        }
        Controller reopened(directory.filePath("client"), "/missing/worker", "/daemon.sock");
        MainWindow window(&reopened);
        auto* dashboard = window.findChild<Dashboard*>();
        QVERIFY(dashboard->contains(Dashboard::Cpu));
        QVERIFY(dashboard->contains(Dashboard::Memory));
        QVERIFY(dashboard->contains(Dashboard::Traffic));
    }
    void logSettingsPersistAndCopyPlainText() {
        QTemporaryDir directory;
        {
            Controller controller(directory.filePath("client"), "/missing/worker", "/daemon.sock");
            MainWindow window(&controller);
            window.show();
            window.findChild<QListWidget*>("navigation")->setCurrentRow(4);
            auto* settings = window.findChild<QToolButton*>("logSettingsButton");
            QTest::ignoreMessage(QtWarningMsg, "This plugin does not support propagateSizeHints()");
            settings->click();
            QPointer<QDialog> dialog = window.findChild<QDialog*>("logSettingsDialog");
            QVERIFY(dialog);
            dialog->findChild<QSpinBox*>("logRetentionLimit")->setValue(100);
            dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel)->click();
            QTRY_VERIFY(!dialog);
            QCOMPARE(controller.logModel()->retentionLimit(), 3000);
            QTest::ignoreMessage(QtWarningMsg, "This plugin does not support propagateSizeHints()");
            settings->click();
            dialog = window.findChild<QDialog*>("logSettingsDialog");
            QVERIFY(dialog);
            dialog->findChild<QSpinBox*>("logRetentionLimit")->setValue(100);
            const auto path = qEnvironmentVariable("SAWFISH_WIDGETS_SCREENSHOT");
            if (!path.isEmpty())
                QVERIFY(dialog->grab().save(path + ".log-settings.png"));
            dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
            QTRY_VERIFY(!dialog);
            QCOMPARE(controller.logModel()->retentionLimit(), 100);
            controller.logModel()->append({{4, "\x1b[36mINFO\x1b[0m[0000] rostra started (0.01s)"},
                                           {3, "A warning"},
                                           {2, "An error"},
                                           {5, "A debug message"},
                                           {6, "A trace message"}});
            window.findChild<QToolButton*>("copyLogsButton")->click();
            QVERIFY(QApplication::clipboard()->text().startsWith("[INFO] [0000] rostra started"));
            QVERIFY(!QApplication::clipboard()->text().contains("[36m"));
            if (!path.isEmpty())
                QVERIFY(window.grab().save(path + ".levels.png"));
            if (!path.isEmpty()) {
                const auto original = QApplication::palette();
                const auto restore = qScopeGuard([&] { QApplication::setPalette(original); });
                auto dark = original;
                dark.setColor(QPalette::Window, QColor("#202124"));
                dark.setColor(QPalette::Base, QColor("#292b30"));
                dark.setColor(QPalette::AlternateBase, QColor("#34363c"));
                dark.setColor(QPalette::Button, QColor("#34363c"));
                for (auto role : {QPalette::Text, QPalette::WindowText, QPalette::ButtonText})
                    dark.setColor(role, QColor("#eceff4"));
                dark.setColor(QPalette::Mid, QColor("#626873"));
                QApplication::setPalette(dark);
                QTest::qWait(20);
                QVERIFY(window.grab().save(path + ".levels.dark.png"));
            }
            auto* pause = window.findChild<QToolButton*>("pauseLogsButton");
            pause->click();
            QCOMPARE(pause->accessibleName(), "Resume logs");
            controller.logModel()->append({{4, "Pending"}});
            QCOMPARE(controller.logModel()->rowCount(), 5);
            pause->click();
            QCOMPARE(controller.logModel()->rowCount(), 6);
            window.findChild<QToolButton*>("clearLogsButton")->click();
            QCOMPARE(controller.logModel()->rowCount(), 0);
        }
        Controller reopened(directory.filePath("client"), "/missing/worker", "/daemon.sock");
        QCOMPARE(reopened.logModel()->retentionLimit(), 100);
    }
    void connectionColumnsCanBeCustomizedAndRestored() {
        QTemporaryDir directory;
        {
            Controller controller(directory.filePath("client"), "/missing/worker", "/daemon.sock");
            MainWindow window(&controller);
            window.show();
            auto* navigation = window.findChild<QListWidget*>("navigation");
            navigation->setCurrentRow(3);
            auto* table = window.findChild<QTableView*>("connectionsTable");
            auto* header = table->horizontalHeader();
            auto* customize = window.findChild<QToolButton*>("customizeConnectionColumnsButton");
            const auto defaults = header->saveState();
            if (QApplication::platformName() == "offscreen")
                QTest::ignoreMessage(QtWarningMsg,
                                     "This plugin does not support propagateSizeHints()");
            customize->click();
            QPointer<QDialog> dialog = window.findChild<QDialog*>("connectionColumnSettings");
            QVERIFY(dialog);
            auto* shown = dialog->findChild<QListWidget*>("shownConnectionColumns");
            auto* available = dialog->findChild<QListWidget*>("availableConnectionColumns");
            QCOMPARE(shown->count(), int(ConnectionModel::ColumnCount));
            QCOMPARE(available->count(), 0);
            for (int row = 0; row < shown->count(); ++row)
                if (shown->item(row)->data(Qt::UserRole).toInt() == ConnectionModel::Process)
                    shown->setCurrentRow(row);
            dialog->findChild<QPushButton*>("hideConnectionColumnButton")->click();
            QCOMPARE(available->count(), 1);
            QVERIFY(!table->isColumnHidden(ConnectionModel::Process)); // Changes are still a draft.
            for (int row = 0; row < shown->count(); ++row)
                if (shown->item(row)->data(Qt::UserRole).toInt() == ConnectionModel::Network)
                    shown->setCurrentRow(row);
            while (shown->currentRow() > 0)
                dialog->findChild<QPushButton*>("moveConnectionColumnUpButton")->click();
            const auto screenshot = qEnvironmentVariable("SAWFISH_COLUMNS_SCREENSHOT");
            if (!screenshot.isEmpty())
                QVERIFY(dialog->grab().save(screenshot + ".dialog.png"));
            dialog->findChild<QPushButton*>("saveConnectionColumnsButton")->click();
            QTRY_VERIFY(!dialog);
            QVERIFY(table->isColumnHidden(ConnectionModel::Process));
            QCOMPARE(header->logicalIndex(0), int(ConnectionModel::Network));
            QCOMPARE(controller.connectionsHeader(), header->saveState());
            const auto saved = header->saveState();
            if (QApplication::platformName() == "offscreen")
                QTest::ignoreMessage(QtWarningMsg,
                                     "This plugin does not support propagateSizeHints()");
            customize->click();
            dialog = window.findChild<QDialog*>("connectionColumnSettings");
            QVERIFY(dialog);
            dialog->findChild<QPushButton*>("resetConnectionColumnsButton")->click();
            dialog->findChild<QPushButton*>("cancelConnectionColumnsButton")->click();
            QTRY_VERIFY(!dialog);
            QCOMPARE(header->saveState(), saved);
            QVERIFY(saved != defaults);
            header->resizeSection(ConnectionModel::Host, 287);
            navigation->setCurrentRow(0); // Flush a pending resize save on leaving the page.
            QCOMPARE(controller.connectionsHeader(), header->saveState());
        }
        Controller reopened(directory.filePath("client"), "/missing/worker", "/daemon.sock");
        MainWindow window(&reopened);
        window.show();
        window.findChild<QListWidget*>("navigation")->setCurrentRow(3);
        auto* table = window.findChild<QTableView*>("connectionsTable");
        QVERIFY(table->isColumnHidden(ConnectionModel::Process));
        QCOMPARE(table->horizontalHeader()->logicalIndex(0), int(ConnectionModel::Network));
        QCOMPARE(table->columnWidth(ConnectionModel::Host), 287);
        if (QApplication::platformName() == "offscreen")
            QTest::ignoreMessage(QtWarningMsg, "This plugin does not support propagateSizeHints()");
        window.findChild<QToolButton*>("customizeConnectionColumnsButton")->click();
        QPointer<QDialog> dialog = window.findChild<QDialog*>("connectionColumnSettings");
        auto* available = dialog->findChild<QListWidget*>("availableConnectionColumns");
        available->setCurrentRow(0);
        dialog->findChild<QPushButton*>("showConnectionColumnButton")->click();
        auto* shown = dialog->findChild<QListWidget*>("shownConnectionColumns");
        QCOMPARE(shown->count(), int(ConnectionModel::ColumnCount));
        auto* remove = dialog->findChild<QPushButton*>("hideConnectionColumnButton");
        while (shown->count() > 1) {
            shown->setCurrentRow(0);
            remove->click();
        }
        QVERIFY(!remove->isEnabled());
        dialog->findChild<QPushButton*>("resetConnectionColumnsButton")->click();
        dialog->findChild<QPushButton*>("saveConnectionColumnsButton")->click();
        QTRY_VERIFY(!dialog);
        QCOMPARE(table->horizontalHeader()->hiddenSectionCount(), 0);
        QCOMPARE(table->horizontalHeader()->logicalIndex(0), int(ConnectionModel::Host));
        QCOMPARE(table->columnWidth(ConnectionModel::Host), 180);
    }
    void connectionsFilteringPauseDetailsAndClose() {
        FakeState state;
        state.setState(2);
        state.sendConnections(sampleConnections());
        FakeDesktop desktop(state);
        FakeManaged managed(state);
        FakeStarted started(state);
        grpc::ServerBuilder builder;
        builder.RegisterService(&desktop);
        builder.RegisterService(&managed);
        builder.RegisterService(&started);
        auto server = builder.BuildAndStart();
        QVERIFY(server);
        {
            QTemporaryDir directory;
            Controller controller(directory.filePath("client"), "/missing/worker", "/daemon.sock",
                                  nullptr,
                                  [&](const QString&) { return server->InProcessChannel({}); });
            MainWindow window(&controller);
            window.show();
            controller.initialize();
            QTRY_COMPARE(controller.connectionModel()->rowCount(), 3);
            QTRY_VERIFY(controller.trafficSample().available);
            state.setTraffic(10000, 64000, 3);
            QTRY_COMPARE(controller.trafficSample().uploadTotal, 10000);
            state.setTraffic(11000, 80000, 2);
            QTRY_COMPARE(controller.trafficSample().uploadTotal, 11000);
            auto* dashboard = window.findChild<Dashboard*>();
            QWidget* uploadCard = nullptr;
            for (auto* card : dashboard->findChildren<QWidget*>("dashboardCard"))
                if (card->property("widgetKind").toInt() == Dashboard::Upload)
                    uploadCard = card;
            QVERIFY(uploadCard);
            const auto chart = uploadCard->grab().toImage();
            const auto accent = uploadCard->palette().color(QPalette::Highlight);
            int ink = 0;
            for (int y = 108; y < chart.height() - 30; ++y)
                for (int x = 16; x < chart.width() - 16; ++x)
                    ink += chart.pixelColor(x, y) == accent;
            QVERIFY2(
                ink > 40,
                "The graph must draw segments between live samples, not only a last-value dot.");
            auto* navigation = window.findChild<QListWidget*>("navigation");
            navigation->setCurrentRow(3);
            QCOMPARE(navigation->currentItem()->text(), "Connections");
            auto* table = window.findChild<QTableView*>("connectionsTable");
            auto* search = window.findChild<QLineEdit*>("connectionSearch");
            auto* view = window.findChild<QComboBox*>("connectionViewSelector");
            QCOMPARE(table->model()->rowCount(), 2);
            search->setText("firefox");
            QCOMPARE(table->model()->rowCount(), 1);
            table->setCurrentIndex(table->model()->index(0, 0));
            QVERIFY(window.findChild<QWidget*>("connectionDetails")->isVisible());
            window.findChild<QToolButton*>("hideConnectionDetailsButton")->click();
            QVERIFY(!window.findChild<QWidget*>("connectionDetails")->isVisible());
            QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier,
                              table->visualRect(table->currentIndex()).center());
            QVERIFY(window.findChild<QWidget*>("connectionDetails")->isVisible());
            QVERIFY(window.findChild<QPlainTextEdit*>("connectionDescription")
                        ->toPlainText()
                        .contains("docs.example.org"));
            auto* pause = window.findChild<QToolButton*>("pauseConnectionsButton");
            pause->click();
            rostra_rpc::ConnectionEvents update;
            auto* event = update.add_events();
            event->set_id("browser");
            event->set_type(rostra_rpc::CONNECTION_EVENT_UPDATE);
            event->set_downlinkdelta(1024);
            state.sendConnections(update);
            QTRY_COMPARE(controller.connectionModel()->find("browser")->download, 5120);
            QCOMPARE(table->model()
                         ->index(0, ConnectionModel::Download)
                         .data(ConnectionModel::SortRole)
                         .toLongLong(),
                     4096);
            pause->click();
            QCOMPARE(table->model()
                         ->index(0, ConnectionModel::Download)
                         .data(ConnectionModel::SortRole)
                         .toLongLong(),
                     5120);
            auto* close = window.findChild<QPushButton*>("closeConnectionButton");
            QVERIFY(close->isEnabled());
            close->click();
            QTRY_COMPARE(table->model()->rowCount(), 0);
            QVERIFY(!close->isEnabled());
            view->setCurrentIndex(1);
            QCOMPARE(table->model()->rowCount(), 1);
            search->clear();
            QCOMPARE(table->model()->rowCount(), 2);
            view->setCurrentIndex(2);
            QCOMPARE(table->model()->rowCount(), 3);
            QVERIFY(!close->isEnabled()); // Filtering must not leave an invisible close target.
            table->setCurrentIndex(table->model()->index(0, 0));
            const auto screenshot = qEnvironmentVariable("SAWFISH_DASHBOARD_SCREENSHOT");
            if (!screenshot.isEmpty()) {
                window.findChild<QToolButton*>("dismissToastButton")->click();
                QVERIFY(window.grab().save(screenshot + ".connections.png"));
                navigation->setCurrentRow(0);
                QVERIFY(window.grab().save(screenshot + ".home.png"));
                window.findChild<QToolButton*>("editWidgetsButton")->click();
                QVERIFY(window.grab().save(screenshot + ".edit.png"));
                navigation->setCurrentRow(4);
                QVERIFY(window.grab().save(screenshot + ".logs.png"));
                navigation->setCurrentRow(3);
            }
            window.findChild<QPushButton*>("closeAllConnectionsButton")->click();
            QTRY_COMPARE(controller.connectionModel()->activeCount(), 0);
            QTRY_VERIFY(!window.findChild<QPushButton*>("closeAllConnectionsButton")->isEnabled());
        }
        server->Shutdown();
    }
    void toastsReplaceMessagesAndKeepAppHistory() {
        QTemporaryDir directory;
        Controller controller(directory.filePath("client"), "/missing/rostra-daemon",
                              directory.filePath("missing.sock"));
        QString error;
        QVERIFY2(controller.profiles()->load(&error), qPrintable(error));
        MainWindow window(&controller);
        window.show();
        auto* toast = window.findChild<QWidget*>("appToast");
        auto* label = window.findChild<QLabel*>("toastMessage");
        auto* navigation = window.findChild<QListWidget*>("navigation");
        QVERIFY(toast);
        QVERIFY(label);
        QVERIFY(toast->isHidden());
        QVERIFY(!window.findChild<QStatusBar*>());
        QFile file(directory.filePath("Home.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{}");
        file.close();
        QVERIFY(controller.importProfile(file.fileName()));
        QVERIFY(toast->isHidden());
        auto* logs = controller.logModel();
        QCOMPARE(logs->rowCount(), 1);
        QVERIFY(logs->index(0).data().toString().startsWith("[APP] [INFO] Profile imported"));
        navigation->setCurrentRow(4);
        navigation->setFocus();
        const auto focused = QApplication::focusWidget();
        controller.notify("Could not import <profile>.", true);
        QCOMPARE(QApplication::focusWidget(), focused);
        QCOMPARE(label->text(), "Could not import <profile>.");
        QCOMPARE(label->textFormat(), Qt::PlainText);
        controller.notify({}); // Successful/background activity must not dismiss an error.
        QVERIFY(toast->isVisible());
        QCOMPARE(logs->index(1).data().toString(), "[APP] [ERROR] Could not import <profile>.");
        window.resize(1200, 800);
        QTRY_COMPARE(toast->geometry().center().x(), window.centralWidget()->rect().center().x());
        logs->append({{4, "Core restarted"}}, true);
        QCOMPARE(logs->rowCount(), 3);
        window.findChild<QToolButton*>("dismissToastButton")->click();
        QVERIFY(toast->isHidden());
        QCOMPARE(logs->rowCount(), 3);
        // A message received in the tray remains available on restoring the window.
        window.hide();
        controller.notify("Profile update failed.", true);
        window.show();
        QTRY_COMPARE(toast->y(), 16);
        QVERIFY(toast->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(toast->isHidden(), 9000);
        QCOMPARE(logs->rowCount(), 4);
        const auto diagnostic = QString("Detailed error:\n") + QString(1200, 'x');
        controller.notify(diagnostic, true);
        QVERIFY(label->text().endsWith("See Logs for details."));
        QTRY_COMPARE(toast->y(), 16);
        QVERIFY(window.centralWidget()->rect().contains(toast->geometry()));
        QVERIFY(toast->height() < 250);
        QCOMPARE(logs->index(4).data(LogModel::MessageRole).toString(), "[APP] " + diagnostic);
    }
    void closeUsesTrayAndQuitStopsSession() {
        FakeState state;
        state.setState(2);
        FakeDesktop desktop(state);
        FakeManaged managed(state);
        FakeStarted started(state);
        grpc::ServerBuilder builder;
        builder.RegisterService(&desktop);
        builder.RegisterService(&managed);
        builder.RegisterService(&started);
        auto server = builder.BuildAndStart();
        QVERIFY(server);
        {
            QTemporaryDir directory;
            Controller controller(directory.filePath("client"), "/missing/rostra-daemon",
                                  "/daemon.sock", nullptr,
                                  [&](const QString&) { return server->InProcessChannel({}); });
            bool trayAvailable = true;
            MainWindow window(&controller, nullptr, [&] { return trayAvailable; });
            QSignalSpy quit(&window, &MainWindow::quitReady);
            window.show();
            controller.initialize();
            QTRY_COMPARE(controller.state(), Controller::Running);
            window.close();
            QVERIFY(!window.isVisible());
            QVERIFY(!controller.shuttingDown());
            QCOMPARE(state.stops.load(), 0);
            window.findChild<QAction*>("showWindowAction")->trigger();
            QVERIFY(window.isVisible());
            window.close();
            trayAvailable = false;
            QTRY_VERIFY_WITH_TIMEOUT(window.isVisible(), 3000);
            window.close();
            QVERIFY(window.isMinimized());
            QVERIFY(!controller.shuttingDown());
            window.restoreWindow();
            auto* disconnect = window.findChild<QAction*>("trayConnectionAction");
            QVERIFY(disconnect->isEnabled());
            QCOMPARE(disconnect->text(), "Stop");
            disconnect->trigger();
            QTRY_COMPARE(controller.state(), Controller::Idle);
            QTRY_VERIFY(!controller.busy());
            QCOMPARE(state.stops.load(), 1);
            QCOMPARE(disconnect->text(), "Start");
            QVERIFY(!disconnect->isEnabled()); // No selected profile or ready worker here.
            state.setState(2);
            QTRY_COMPARE(controller.state(), Controller::Running);
            window.findChild<QAction*>("quitAction")->trigger();
            QTRY_COMPARE_WITH_TIMEOUT(quit.size(), 1, 5000);
            QVERIFY(!window.isVisible());
            QVERIFY(controller.shuttingDown());
            QCOMPARE(state.stops.load(), 2);
            window.requestQuit();
            QCOMPARE(quit.size(), 1);
        }
        server->Shutdown();
    }
    void trayStartStopAndIconTrackActualConnection() {
        FakeState state;
        FakeDesktop desktop(state);
        FakeManaged managed(state);
        FakeApplication application(state);
        FakeStarted started(state);
        grpc::ServerBuilder builder;
        builder.RegisterService(&desktop);
        builder.RegisterService(&managed);
        builder.RegisterService(&application);
        builder.RegisterService(&started);
        auto server = builder.BuildAndStart();
        QVERIFY(server);
        {
            QTemporaryDir directory;
            Controller controller(directory.filePath("client"),
                                  QCoreApplication::applicationFilePath(), "/daemon.sock", nullptr,
                                  [&](const QString&) { return server->InProcessChannel({}); });
            MainWindow window(&controller, nullptr, [] { return true; });
            window.show();
            controller.initialize();
            QTRY_COMPARE(controller.workerStatus(), "Ready");
            QTRY_COMPARE(controller.state(), Controller::Idle);
            auto* action = window.findChild<QAction*>("trayConnectionAction");
            auto* tray = window.findChild<QSystemTrayIcon*>();
            QVERIFY(action && tray);
            QCOMPARE(action->text(), "Start");
            QVERIFY(!action->isEnabled());
            const auto idle = window.windowIcon().pixmap(24).toImage();
            QCOMPARE(window.property("connectionIconState").toInt(),
                     int(ConnectionIconState::Idle));
            QFile file(directory.filePath("config.json"));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("{}");
            file.close();
            QVERIFY(controller.importProfile(file.fileName()));
            QTRY_COMPARE(controller.selectedProfile()->validation, Profile::Valid);
            window.hide(); // Tray commands must work independently of Home and visibility.
            state.holdCheck = true;
            action->trigger();
            QVERIFY(!action->isEnabled());
            QCOMPARE(window.property("connectionIconState").toInt(),
                     int(ConnectionIconState::Working));
            QCOMPARE(state.starts.load(), 0);
            state.holdCheck = false;
            QTRY_VERIFY(!controller.busy());
            QTRY_COMPARE(controller.state(), Controller::Running);
            QCOMPARE(action->text(), "Stop");
            QVERIFY(action->isEnabled());
            QCOMPARE(window.property("connectionIconState").toInt(),
                     int(ConnectionIconState::Running));
            const auto running = window.windowIcon().pixmap(24).toImage();
            QVERIFY(running != idle);
            QCOMPARE(tray->icon().pixmap(24).toImage(), running);
            QVERIFY(!window.isVisible());
            action->trigger();
            QCOMPARE(window.property("connectionIconState").toInt(),
                     int(ConnectionIconState::Working));
            QTRY_VERIFY(!controller.busy());
            QTRY_COMPARE(controller.state(), Controller::Idle);
            QCOMPARE(action->text(), "Start");
            QCOMPARE(window.windowIcon().pixmap(24).toImage(), idle);
            state.setState(2); // A backend event, with no button click, updates both icons.
            QTRY_COMPARE(window.property("connectionIconState").toInt(),
                         int(ConnectionIconState::Running));
            state.setState(4);
            QTRY_COMPARE(window.property("connectionIconState").toInt(),
                         int(ConnectionIconState::Unavailable));
            QVERIFY(controller.saveProfile(controller.selectedId(), "Invalid", {}, "invalid"));
            QTRY_COMPARE(controller.selectedProfile()->validation, Profile::Invalid);
            action->trigger();
            QTRY_VERIFY(!controller.busy());
            QCOMPARE(window.property("connectionIconState").toInt(),
                     int(ConnectionIconState::Unavailable));
            QCOMPARE(state.starts.load(), 1);
        }
        server->Shutdown();
    }
    void iconSizesAndThemePreview() {
        const auto path = qEnvironmentVariable("SAWFISH_ICON_PREVIEW");
        QPixmap preview(480, 180);
        preview.fill(QApplication::palette().color(QPalette::Window));
        QPainter painter(&preview);
        painter.setPen(QApplication::palette().color(QPalette::WindowText));
        int column = 0;
        for (auto state : {ConnectionIconState::Idle, ConnectionIconState::Running,
                           ConnectionIconState::Working, ConnectionIconState::Unavailable}) {
            const auto icon = applicationIcon(QApplication::palette(), state);
            for (int size : {16, 24, 32, 64})
                QVERIFY(icon.availableSizes().contains(QSize(size, size)));
            icon.paint(&painter, QRect(column * 120 + 28, 10, 64, 64));
            icon.paint(&painter, QRect(column * 120 + 40, 90, 24, 24));
            icon.paint(&painter, QRect(column * 120 + 44, 126, 16, 16));
            painter.drawText(QRect(column * 120, 152, 120, 25), Qt::AlignCenter,
                             QStringList{"Idle", "Running", "Working", "Unavailable"}[column]);
            ++column;
        }
        painter.end();
        if (!path.isEmpty())
            QVERIFY(preview.save(path));
    }
    void profilesAndUnavailableBackend() {
        QTemporaryDir directory;
        Controller controller(directory.filePath("client"), "/missing/rostra-daemon",
                              directory.filePath("missing.sock"));
        MainWindow window(&controller);
        window.show();
        controller.initialize();
        auto* toggle = window.findChild<QAbstractButton*>("connectionSwitch");
        QVERIFY(toggle);
        QVERIFY(!toggle->isEnabled());
        QVERIFY(!window.findChild<QMenuBar*>());
        QVERIFY(!window.findChild<QPushButton*>("connectButton"));
        QVERIFY(!window.findChild<QPushButton*>("disconnectButton"));
        QVERIFY(controller.workerStatus().contains("Settings"));
        QFile profile(directory.filePath("first.json"));
        QVERIFY(profile.open(QIODevice::WriteOnly));
        profile.write("{\"outbounds\":[{\"type\":\"direct\",\"tag\":\"direct\"}]}");
        profile.close();
        QVERIFY(controller.importProfile(profile.fileName()));
        const auto firstId = controller.selectedId();
        QFile second(directory.filePath("second.json"));
        QVERIFY(second.open(QIODevice::WriteOnly));
        second.write("{\"log\":{\"level\":\"debug\"}}");
        second.close();
        QVERIFY(controller.importProfile(second.fileName()));
        const auto secondId = controller.selectedId();
        auto* navigation = window.findChild<QListWidget*>("navigation");
        QVERIFY(navigation);
        QCOMPARE(navigation->item(2)->text(), "Proxies");
        navigation->setCurrentRow(2);
        QVERIFY(!window.findChild<QPushButton*>("selectOutboundButton"));
        QVERIFY(!window.findChild<QToolButton*>("refreshGroupsButton")->isEnabled());
        QVERIFY(window.findChild<QLabel*>("proxiesState")->text().contains("Connect a profile"));
        navigation->setCurrentRow(1);
        auto findCard = [&](const QString& id) -> QWidget* {
            for (auto* card : window.findChildren<QWidget*>("profileCard"))
                if (card->property("profileId").toString() == id)
                    return card;
            return nullptr;
        };
        QVERIFY(findCard(firstId));
        QVERIFY(findCard(secondId));
        QVERIFY(!findCard(secondId)->findChild<QAction*>("selectProfileAction")->isVisible());
        QVERIFY(findCard(firstId)->findChild<QAction*>("selectProfileAction")->isVisible());
        QVERIFY(!findCard(firstId)->findChild<QAction*>("updateProfileAction")->isVisible());
        QCOMPARE(findCard(firstId)->findChild<QLabel*>("profileName")->toolTip(), "first.json");
        findCard(firstId)->findChild<QAction*>("selectProfileAction")->trigger();
        QCOMPARE(controller.selectedId(), firstId);
        findCard(secondId)->findChild<QAction*>("selectProfileAction")->trigger();
        QCOMPARE(controller.selectedId(), secondId);
        QVERIFY(!window.findChild<QPushButton*>("validateButton"));
        QVERIFY(window.findChild<QToolButton*>("importProfileButton")->menu());
        navigation->setCurrentRow(0);
        QTest::qWait(100);
        const auto screenshot = qEnvironmentVariable("SAWFISH_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(window.grab().save(screenshot));
        window.close();
    }
    void profileCardsSwitchEditingAndThemeChanges() {
        FakeState state;
        FakeDesktop desktop(state);
        FakeManaged managed(state);
        FakeApplication application(state);
        FakeStarted started(state);
        grpc::ServerBuilder builder;
        builder.RegisterService(&desktop);
        builder.RegisterService(&managed);
        builder.RegisterService(&application);
        builder.RegisterService(&started);
        auto server = builder.BuildAndStart();
        QVERIFY(server);
        {
            QTemporaryDir directory;
            Controller controller(directory.filePath("client"),
                                  QCoreApplication::applicationFilePath(), "/daemon.sock", nullptr,
                                  [&](const QString&) { return server->InProcessChannel({}); });
            MainWindow window(&controller);
            window.show();
            controller.initialize();
            QTRY_COMPARE(controller.workerStatus(), "Ready");
            QTRY_COMPARE(controller.state(), Controller::Idle);
            QFile file(directory.filePath("Home.json"));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("{}");
            file.close();
            QVERIFY(controller.importProfile(file.fileName()));
            const auto first = controller.selectedId();
            QTRY_COMPARE(controller.selectedProfile()->validation, Profile::Valid);
            const auto screenshot = qEnvironmentVariable("SAWFISH_DESIGN_SCREENSHOT");
            if (!screenshot.isEmpty()) {
                window.findChild<QToolButton*>("dismissToastButton")->click();
                QVERIFY(window.grab().save(screenshot + ".home.idle.png"));
                auto* menu = window.findChild<QToolButton*>("importProfileButton")->menu();
                menu->adjustSize();
                menu->setActiveAction(menu->actions().first());
                QVERIFY(menu->grab().save(screenshot + ".import-menu.png"));
                auto* navigation = window.findChild<QListWidget*>("navigation");
                navigation->setCurrentRow(5);
                QVERIFY(window.grab().save(screenshot + ".settings.png"));
                navigation->setCurrentRow(0);
            }
            auto* toggle = window.findChild<QAbstractButton*>("connectionSwitch");
            QVERIFY(toggle->isEnabled());
            QTest::mouseClick(toggle, Qt::LeftButton);
            QTRY_COMPARE(controller.state(), Controller::Running);
            QTRY_VERIFY(!controller.busy());
            QVERIFY(toggle->isChecked());
            QCOMPARE(state.starts.load(), 1);
            if (!screenshot.isEmpty())
                QVERIFY(window.grab().save(screenshot + ".home.png"));
            auto* navigation = window.findChild<QListWidget*>("navigation");
            navigation->setCurrentRow(1);
            auto findCard = [&](const QString& id) -> QWidget* {
                for (auto* card : window.findChildren<QWidget*>("profileCard"))
                    if (card->property("profileId").toString() == id)
                        return card;
                return nullptr;
            };
            auto* firstCard = findCard(first);
            QVERIFY(firstCard);
            if (QApplication::platformName() == "offscreen")
                QTest::ignoreMessage(QtWarningMsg,
                                     "This plugin does not support propagateSizeHints()");
            firstCard->findChild<QAction*>("editProfileAction")->trigger();
            auto* editor = window.findChild<QDialog*>("profileEditor");
            QVERIFY(editor);
            editor->findChild<QLineEdit*>("profileNameEdit")->setText("My home");
            editor->findChild<QLineEdit*>("profileUrlEdit")
                ->setText("https://example.org/home.json");
            editor->findChild<QPlainTextEdit*>("profileContentEdit")->setPlainText("{ }");
            editor->findChild<QPushButton*>("saveProfileButton")->click();
            QTRY_VERIFY(!controller.busy());
            QTRY_COMPARE(controller.selectedProfile()->validation, Profile::Valid);
            QCOMPARE(controller.selectedProfile()->name, "My home");
            QCOMPARE(controller.selectedProfile()->url, "https://example.org/home.json");
            QVERIFY(firstCard->findChild<QAction*>("updateProfileAction")->isVisible());
            QVERIFY(!firstCard->findChild<QAction*>("selectProfileAction")->isVisible());
            QCOMPARE(state.starts.load(), 2);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("invalid");
            file.close();
            QVERIFY(controller.importProfile(file.fileName()));
            const auto invalid = controller.selectedId();
            QTRY_VERIFY(!controller.busy());
            QTRY_COMPARE(controller.selectedProfile()->validation, Profile::Invalid);
            QCOMPARE(state.starts.load(), 2);
            QCOMPARE(controller.activeId(), first);
            auto* invalidCard = findCard(invalid);
            QVERIFY(invalidCard);
            QCOMPARE(firstCard->findChild<QWidget*>("validationIndicator")
                         ->property("validation")
                         .toInt(),
                     int(Profile::Valid));
            QCOMPARE(invalidCard->findChild<QWidget*>("validationIndicator")
                         ->property("validation")
                         .toInt(),
                     int(Profile::Invalid));
            if (!screenshot.isEmpty())
                QVERIFY(window.grab().save(screenshot + ".profiles.png"));

            const auto originalPalette = QApplication::palette();
            const auto restore = qScopeGuard([&] { QApplication::setPalette(originalPalette); });
            auto palette = originalPalette;
            palette.setColor(QPalette::Window, QColor("#202126"));
            palette.setColor(QPalette::WindowText, QColor("#eeeeee"));
            palette.setColor(QPalette::Base, QColor("#282a30"));
            palette.setColor(QPalette::AlternateBase, QColor("#30333a"));
            palette.setColor(QPalette::Text, QColor("#eeeeee"));
            palette.setColor(QPalette::Button, QColor("#30333a"));
            palette.setColor(QPalette::ButtonText, QColor("#eeeeee"));
            palette.setColor(QPalette::Mid, QColor("#60636b"));
            palette.setColor(QPalette::Midlight, QColor("#40434b"));
            palette.setColor(QPalette::Highlight, QColor("#bd8eff"));
            palette.setColor(QPalette::HighlightedText, QColor("#201830"));
            QApplication::setPalette(palette);
            QTest::qWait(50);
            QCOMPARE(toggle->palette().color(QPalette::Highlight),
                     palette.color(QPalette::Highlight));
            QCOMPARE(firstCard->palette().color(QPalette::Base), palette.color(QPalette::Base));
            QCOMPARE(firstCard->findChild<QToolButton*>("profileMenuButton")
                         ->palette()
                         .color(QPalette::ButtonText),
                     palette.color(QPalette::ButtonText));
            if (!screenshot.isEmpty())
                QVERIFY(window.grab().save(screenshot + ".profiles.dark.png"));
            navigation->setCurrentRow(0);
            const auto switchImage = toggle->grab().toImage();
            QCOMPARE(switchImage.pixelColor(10, toggle->height() / 2),
                     palette.color(QPalette::Highlight));
            if (!screenshot.isEmpty())
                QVERIFY(window.grab().save(screenshot + ".home.dark.png"));
            QTest::mouseClick(toggle, Qt::LeftButton);
            QTRY_COMPARE(controller.state(), Controller::Idle);
            QTRY_VERIFY(!controller.busy());
            QVERIFY(!toggle->isChecked());
            if (!screenshot.isEmpty()) {
                window.findChild<QToolButton*>("dismissToastButton")->click();
                QVERIFY(window.grab().save(screenshot + ".home.idle.dark.png"));
                auto* menu = firstCard->findChild<QToolButton*>("profileMenuButton")->menu();
                menu->adjustSize();
                menu->setActiveAction(menu->actions().first());
                QVERIFY(menu->grab().save(screenshot + ".profile-menu.dark.png"));
                controller.notify("Configuration check failed. Open Profiles to edit it.", true);
                QTest::qWait(200);
                QVERIFY(window.grab().save(screenshot + ".toast.dark.png"));
                window.findChild<QToolButton*>("dismissToastButton")->click();
                navigation->setCurrentRow(5);
                QVERIFY(window.grab().save(screenshot + ".settings.dark.png"));
            }
            navigation->setCurrentRow(1);
            if (QApplication::platformName() == "offscreen")
                QTest::ignoreMessage(QtWarningMsg,
                                     "This plugin does not support propagateSizeHints()");
            invalidCard->findChild<QAction*>("deleteProfileAction")->trigger();
            auto* confirm = window.findChild<QMessageBox*>();
            QVERIFY(confirm);
            confirm->button(QMessageBox::Yes)->click();
            QTRY_COMPARE(controller.profiles()->rowCount(), 1);
            QCOMPARE(controller.selectedId(), first);
            // Changing the dropdown uses the same automatic selection path.
            auto* combo = window.findChild<QComboBox*>("homeProfileSelector");
            QCOMPARE(combo->currentData().toString(), first);
        }
        server->Shutdown();
    }
    void proxiesFollowDaemonAndPreserveFocus() {
        FakeState state;
        state.setState(2);
        FakeDesktop desktop(state);
        FakeManaged managed(state);
        FakeStarted started(state);
        grpc::ServerBuilder builder;
        builder.RegisterService(&desktop);
        builder.RegisterService(&managed);
        builder.RegisterService(&started);
        auto server = builder.BuildAndStart();
        QVERIFY(server);
        {
            QTemporaryDir directory;
            Controller controller(directory.filePath("client"), "/missing/rostra-daemon",
                                  "/daemon.sock", nullptr,
                                  [&](const QString&) { return server->InProcessChannel({}); });
            MainWindow window(&controller);
            window.show();
            controller.initialize();
            QTRY_VERIFY(controller.groupsReady());
            auto* navigation = window.findChild<QListWidget*>("navigation");
            navigation->setCurrentRow(2);
            auto findCards = [&](const QString& tag) -> QListView* {
                for (auto* view : window.findChildren<QListView*>("outboundCards"))
                    if (view->property("groupTag").toString() == tag)
                        return view;
                return nullptr;
            };
            auto findToggle = [&](const QString& tag) -> QAbstractButton* {
                for (auto* button : window.findChildren<QAbstractButton*>("proxyGroup"))
                    if (button->property("groupTag").toString() == tag)
                        return button;
                return nullptr;
            };
            QTRY_VERIFY(findCards("proxy"));
            auto* cards = findCards("proxy");
            auto* model = controller.groupModel();
            QVERIFY(!window.findChild<QPushButton*>("selectOutboundButton"));
            QTRY_COMPARE(cards->rootIndex(), model->findIndex("proxy"));
            cards->setCurrentIndex(model->findIndex("proxy", "direct-a"));
            cards->setFocus();
            QTest::keyClick(cards, Qt::Key_Right);
            QCOMPARE(cards->currentIndex().data(GroupModel::OutboundTagRole).toString(),
                     "direct-b");
            QCOMPARE(state.selections.load(), 0); // Moving focus never changes the active proxy.
            QVERIFY(model->findIndex("proxy", "direct-a").data(GroupModel::SelectedRole).toBool());
            state.holdSelection = true;
            QTest::keyClick(cards, Qt::Key_Return);
            QTRY_COMPARE(state.selections.load(), 1);
            QVERIFY(controller.selectingOutbound("proxy", "direct-b"));
            QCOMPARE(model->find("proxy")->selected, "direct-a");
            QTest::keyClick(cards, Qt::Key_Space);
            QCOMPARE(state.selections.load(), 1);
            state.holdSelection = false;
            QTRY_VERIFY(!controller.busy());
            QTRY_COMPARE(model->find("proxy")->selected, "direct-b");
            QTRY_COMPARE(cards->currentIndex().data(GroupModel::OutboundTagRole).toString(),
                         "direct-b");
            auto* automaticToggle = findToggle("urltest");
            QVERIFY(automaticToggle);
            QVERIFY(!window.findChild<QToolButton*>("groupToggle"));
            auto* proxySection = findToggle("proxy");
            const QPoint emptySpace(1, 1); // The gap around the first outbound is group background.
            QTest::mouseClick(cards->viewport(), Qt::LeftButton, Qt::NoModifier, emptySpace);
            QVERIFY(!proxySection->isChecked());
            QCOMPARE(state.selections.load(), 1);
            QTest::keyClick(proxySection, Qt::Key_Space);
            QVERIFY(proxySection->isChecked());
            QTest::mouseClick(automaticToggle, Qt::LeftButton, Qt::NoModifier, QPoint(35, 24));
            QVERIFY(!automaticToggle->isChecked());
            auto updated = sampleGroups();
            updated.mutable_group(0)->set_selected("direct-b");
            updated.mutable_group(0)->mutable_items()->SwapElements(0, 1);
            state.setGroups(updated);
            QTRY_COMPARE(model->find("proxy")->members.first().tag, "direct-b");
            QTRY_COMPARE(cards->currentIndex().data(GroupModel::OutboundTagRole).toString(),
                         "direct-b");
            QVERIFY(!automaticToggle->isChecked());
            QVERIFY(!findCards("urltest")->isVisible());
            automaticToggle->setFocus();
            QTest::keyClick(automaticToggle, Qt::Key_Space);
            QVERIFY(automaticToggle->isChecked());
            QTest::keyClick(automaticToggle, Qt::Key_Return);
            QVERIFY(!automaticToggle->isChecked());
            auto* balance = findCards("loadbalance");
            balance->setCurrentIndex(model->findIndex("loadbalance", "direct-a"));
            QTest::keyClick(balance, Qt::Key_Return);
            QCOMPARE(state.selections.load(), 1);
            QVERIFY(!model->findIndex("loadbalance", "direct-a")
                         .data(GroupModel::SelectedRole)
                         .toBool());
            automaticToggle->click();
            auto* automatic = findCards("urltest");
            automatic->setCurrentIndex(model->findIndex("urltest", "direct-b"));
            QTest::keyClick(automatic, Qt::Key_Space);
            QCOMPARE(state.selections.load(), 1);

            auto* search = window.findChild<QLineEdit*>("proxySearch");
            QVERIFY(search);
            search->setText("DIRECT-B");
            QVERIFY(!cards->isRowHidden(0));
            QVERIFY(cards->isRowHidden(1));
            search->setText("no-match");
            QVERIFY(window.findChild<QLabel*>("proxiesState")->text().contains("No proxies match"));
            search->clear();
            QVERIFY(!cards->isRowHidden(1));

            // The badge is a separate hit target, including on automatic groups.
            auto* proxyGroup = findToggle("proxy");
            const auto delayIndex = model->findIndex("proxy", "direct-a");
            const auto cell = cards->visualRect(delayIndex);
            const QPoint delayPoint(cell.right() - 35, cell.bottom() - 22);
            const auto iconBeforeTest = window.windowIcon().cacheKey();
            QTest::mouseClick(cards->viewport(), Qt::LeftButton, Qt::NoModifier, delayPoint);
            QTRY_COMPARE(state.delayTests.load(), 1);
            QVERIFY(controller.testingDelay("direct-a"));
            QCOMPARE(state.selections.load(), 1);
            QVERIFY(proxyGroup->isChecked());
            QCOMPARE(window.windowIcon().cacheKey(), iconBeforeTest);
            QTest::mouseClick(cards->viewport(), Qt::LeftButton, Qt::NoModifier, delayPoint);
            QCOMPARE(state.delayTests.load(), 1);
            auto measured = updated;
            for (auto& group : *measured.mutable_group())
                for (auto& member : *group.mutable_items())
                    if (member.tag() == "direct-a") {
                        member.set_urltesttime(1700000001);
                        member.set_urltestdelay(38);
                    }
            state.setGroups(measured);
            QTRY_VERIFY(!controller.testingDelay("direct-a"));
            QTRY_COMPARE(model->findIndex("proxy", "direct-a").siblingAtColumn(3).data().toString(),
                         "38 ms");
            state.rejectDelay = true;
            automatic->setCurrentIndex(model->findIndex("urltest", "direct-b"));
            QTest::keyClick(automatic, Qt::Key_T);
            QTRY_COMPARE(state.delayTests.load(), 2);
            QTRY_VERIFY(!controller.testingDelay("direct-b"));
            QVERIFY(controller.delayError("direct-b").contains("outbound was removed"));
            QCOMPARE(state.selections.load(), 1);
            state.rejectDelay = false;

            // Mouse activation is one click and keeps the old active card on RPC failure.
            state.rejectSelection = true;
            const auto first = model->findIndex("proxy", "direct-a");
            auto* scroll = window.findChild<QScrollArea*>("proxyScroll");
            scroll->verticalScrollBar()->setValue(0);
            QTRY_VERIFY(!cards->visualRect(first).isEmpty());
            QTest::mouseClick(cards->viewport(), Qt::LeftButton, Qt::NoModifier,
                              cards->visualRect(first).center());
            QTRY_COMPARE(state.selections.load(), 2);
            QTRY_VERIFY(!controller.busy());
            QVERIFY(
                window.findChild<QLabel*>("toastMessage")->text().contains("outbound was removed"));
            QCOMPARE(model->find("proxy")->selected, "direct-b");
            state.rejectSelection = false;
            QTest::mouseClick(cards->viewport(), Qt::LeftButton, Qt::NoModifier,
                              cards->visualRect(first).center());
            QTRY_VERIFY(!controller.busy());
            QTRY_COMPARE(model->find("proxy")->selected, "direct-a");
            QTRY_COMPARE(cards->currentIndex().data(GroupModel::OutboundTagRole).toString(),
                         "direct-a");
            const auto screenshot = qEnvironmentVariable("SAWFISH_PROXIES_SCREENSHOT");
            if (!screenshot.isEmpty())
                QVERIFY(window.grab().save(screenshot));

            auto horizontalGap = [&] {
                auto a = cards->visualRect(model->findIndex("proxy", "direct-a"));
                auto b = cards->visualRect(model->findIndex("proxy", "direct-b"));
                if (a.x() > b.x())
                    std::swap(a, b);
                return b.left() - a.right() - 1;
            };
            // Delegate padding adds a fixed 8-pixel gap to adjacent cells.
            QCOMPARE(horizontalGap(), 0);
            window.resize(800, 600);
            QTRY_VERIFY(cards->viewport()->width() < 600);
            QTest::qWait(50); // Icon-mode wrapping is laid out on the next event-loop turn.
            QCOMPARE(horizontalGap(), 0);
            QTRY_VERIFY(cards->viewport()->rect().contains(
                cards->visualRect(model->findIndex("proxy", "direct-a"))));
            QTRY_VERIFY(cards->viewport()->rect().contains(
                cards->visualRect(model->findIndex("proxy", "direct-b"))));
            if (!screenshot.isEmpty())
                QVERIFY(window.grab().save(screenshot + ".narrow.png"));
            const auto originalPalette = QApplication::palette();
            const auto restorePalette =
                qScopeGuard([&] { QApplication::setPalette(originalPalette); });
            auto palette = originalPalette;
            palette.setColor(QPalette::Window, QColor(30, 32, 36));
            palette.setColor(QPalette::WindowText, QColor(235, 235, 235));
            palette.setColor(QPalette::Base, QColor(36, 39, 44));
            palette.setColor(QPalette::AlternateBase, QColor(45, 48, 54));
            palette.setColor(QPalette::Text, QColor(235, 235, 235));
            palette.setColor(QPalette::PlaceholderText, QColor(160, 163, 169));
            palette.setColor(QPalette::Button, QColor(45, 48, 54));
            palette.setColor(QPalette::ButtonText, QColor(235, 235, 235));
            palette.setColor(QPalette::Mid, QColor(85, 88, 94));
            palette.setColor(QPalette::Midlight, QColor(62, 65, 71));
            QApplication::setPalette(palette);
            {
                MainWindow darkWindow(&controller);
                darkWindow.resize(800, 600);
                darkWindow.show();
                darkWindow.findChild<QListWidget*>("navigation")->setCurrentRow(2);
                QTest::qWait(50);
                if (!screenshot.isEmpty())
                    QVERIFY(darkWindow.grab().save(screenshot + ".dark.png"));
            }
            QApplication::setPalette(originalPalette);

            // Live latency/membership snapshots must not move a scrolled page to the top.
            auto many = updated;
            many.mutable_group(0)->set_selected("direct-a");
            for (int i = 0; i < 18; ++i) {
                auto* member = many.mutable_group(0)->add_items();
                member->set_tag("extra-outbound-" + std::to_string(i));
                member->set_type("direct");
            }
            state.setGroups(many);
            QTRY_COMPARE(model->find("proxy")->members.size(), 20);
            QTRY_VERIFY(scroll->verticalScrollBar()->maximum() > 400);
            QTRY_VERIFY(cards->viewport()->rect().contains(
                cards->visualRect(model->findIndex("proxy", "extra-outbound-17"))));
            cards->clearFocus();
            scroll->verticalScrollBar()->setValue(200);
            const int oldScroll = scroll->verticalScrollBar()->value();
            many.mutable_group(0)->mutable_items(0)->set_urltestdelay(32);
            state.setGroups(many);
            QTRY_COMPARE(model->find("proxy")->members.first().delay, 32);
            QTRY_COMPARE(cards->rootIndex(), model->findIndex("proxy"));
            QCOMPARE(scroll->verticalScrollBar()->value(), oldScroll);

            updated.mutable_group(0)->mutable_items()->RemoveLast();
            updated.mutable_group(0)->set_selected("direct-b");
            state.setGroups(updated);
            QTRY_COMPARE(model->find("proxy")->members.size(), 1);
            QTRY_VERIFY(!cards->currentIndex().isValid());
            state.setGroups({});
            QTRY_COMPARE(model->rowCount(), 0);
            QTRY_VERIFY(
                window.findChild<QLabel*>("proxiesState")->text().contains("no outbound groups"));
            state.setState(0);
            QTRY_COMPARE(controller.state(), Controller::Idle);
            QVERIFY(!window.findChild<QToolButton*>("refreshGroupsButton")->isEnabled());
            window.close();
        }
        server->Shutdown();
    }
};
int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "worker") {
        std::cout << "READY" << std::endl;
        std::cin.ignore(std::numeric_limits<std::streamsize>::max());
        return 0;
    }
    // Dialog sizing is unsupported by the offscreen platform. Expected warnings
    // are matched explicitly; every other Qt warning fails the current test.
    qunsetenv("QT_FATAL_WARNINGS");
    QApplication app(argc, argv);
    UiTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_ui.moc"
