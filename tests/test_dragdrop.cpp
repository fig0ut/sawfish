#include "app/controller.h"
#include "mainwindow.h"
#include <QDialog>
#include <QDrag>
#include <QHeaderView>
#include <QListWidget>
#include <QPushButton>
#include <QTableView>
#include <QTemporaryDir>
#include <QToolButton>
#include <QWindow>
#include <QtTest>

// Qt's minimal platform supports native drag delivery; offscreen does not.
class DragDropTest : public QObject {
    Q_OBJECT
  private slots:
    void init() {
        QTest::failOnWarning();
    }
    void connectionColumnsDragBetweenLists() {
        QTemporaryDir directory;
        Controller controller(directory.filePath("client"), "/missing/worker", "/daemon.sock");
        MainWindow window(&controller);
        window.show();
        window.findChild<QListWidget*>("navigation")->setCurrentRow(3);
        auto* header = window.findChild<QTableView*>("connectionsTable")->horizontalHeader();
        const auto original = header->saveState();
        if (QApplication::platformName() == "minimal")
            QTest::ignoreMessage(QtWarningMsg, "This plugin does not support propagateSizeHints()");
        window.findChild<QToolButton*>("customizeConnectionColumnsButton")->click();
        QPointer<QDialog> dialog = window.findChild<QDialog*>("connectionColumnSettings");
        QVERIFY(dialog);
        auto* shown = dialog->findChild<QListWidget*>("shownConnectionColumns");
        auto* available = dialog->findChild<QListWidget*>("availableConnectionColumns");
        auto drag = [&](QListWidget* source, int row, QListWidget* destination, int beforeRow) {
            source->setCurrentRow(row);
            source->scrollToItem(source->currentItem());
            if (beforeRow < destination->count())
                destination->scrollToItem(destination->item(beforeRow));
            QTest::qWait(10);
            auto* handle = dialog->windowHandle();
            const auto start = handle->mapFromGlobal(source->viewport()->mapToGlobal(
                source->visualItemRect(source->currentItem()).center()));
            const auto target =
                beforeRow < destination->count()
                    ? destination->visualItemRect(destination->item(beforeRow)).topLeft() +
                          QPoint(12, 1)
                    : QPoint(12, destination->viewport()->height() - 5);
            const auto end = handle->mapFromGlobal(destination->viewport()->mapToGlobal(target));
            bool released = false;
            QTimer release, watchdog;
            release.setSingleShot(true);
            watchdog.setSingleShot(true);
            connect(&release, &QTimer::timeout, this, [&] {
                QTest::mouseMove(handle, end);
                QTest::mouseRelease(handle, Qt::LeftButton, Qt::NoModifier, end);
                released = true;
            });
            connect(&watchdog, &QTimer::timeout, this, [] { QDrag::cancel(); });
            QTest::mouseMove(handle, start);
            QTest::mousePress(handle, Qt::LeftButton, Qt::NoModifier, start);
            release.start(80);
            watchdog.start(1000);
            QTest::mouseMove(handle, start + QPoint(QApplication::startDragDistance() + 4, 0));
            QTRY_VERIFY(released);
        };
        drag(shown, 0, available, 0);
        QCOMPARE(shown->count(), 11);
        QCOMPARE(available->count(), 1);
        QCOMPARE(available->item(0)->data(Qt::UserRole).toInt(), int(ConnectionModel::Host));
        QCOMPARE(header->saveState(), original); // Dragging only changes the draft.
        drag(available, 0, shown, 1);
        QCOMPARE(shown->count(), 12);
        QCOMPARE(available->count(), 0);
        QCOMPARE(shown->item(1)->data(Qt::UserRole).toInt(), int(ConnectionModel::Host));
        drag(shown, 1, shown, 0);
        QCOMPARE(shown->item(0)->data(Qt::UserRole).toInt(), int(ConnectionModel::Host));
        QCOMPARE(shown->count(), 12);
        auto* remove = dialog->findChild<QPushButton*>("hideConnectionColumnButton");
        while (shown->count() > 1) {
            shown->setCurrentRow(0);
            remove->click();
        }
        const int last = shown->item(0)->data(Qt::UserRole).toInt();
        drag(shown, 0, available, 0);
        QCOMPARE(shown->count(), 1);
        QCOMPARE(available->count(), 11);
        QCOMPARE(shown->item(0)->data(Qt::UserRole).toInt(), last);
        const auto reordered = available->item(1)->data(Qt::UserRole);
        drag(available, 1, available, 0);
        QCOMPARE(available->item(0)->data(Qt::UserRole), reordered);
        QCOMPARE(available->count(), 11);
        QSet<int> columns;
        for (auto* list : {shown, available})
            for (int row = 0; row < list->count(); ++row)
                columns.insert(list->item(row)->data(Qt::UserRole).toInt());
        QCOMPARE(columns.size(), int(ConnectionModel::ColumnCount));
        dialog->findChild<QPushButton*>("cancelConnectionColumnsButton")->click();
        QTRY_VERIFY(!dialog);
        QCOMPARE(header->saveState(), original);
        QTest::ignoreMessage(QtWarningMsg, "This plugin does not support propagateSizeHints()");
        window.findChild<QToolButton*>("customizeConnectionColumnsButton")->click();
        dialog = window.findChild<QDialog*>("connectionColumnSettings");
        QVERIFY(dialog);
        shown = dialog->findChild<QListWidget*>("shownConnectionColumns");
        available = dialog->findChild<QListWidget*>("availableConnectionColumns");
        drag(shown, 0, available, 0);
        dialog->findChild<QPushButton*>("saveConnectionColumnsButton")->click();
        QTRY_VERIFY(!dialog);
        QVERIFY(header->isSectionHidden(ConnectionModel::Host));
        QCOMPARE(controller.connectionsHeader(), header->saveState());
    }
};

int main(int argc, char** argv) {
    qunsetenv("QT_FATAL_WARNINGS");
    QApplication app(argc, argv);
    DragDropTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_dragdrop.moc"
