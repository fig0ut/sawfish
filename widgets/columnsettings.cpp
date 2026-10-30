#include "columnsettings.h"
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <memory>

namespace {
class ColumnList : public QListWidget {
  public:
    ColumnList* peer = nullptr;
    bool keepOne = false;
    explicit ColumnList(bool shown) : keepOne(shown) {
        setDragDropMode(QAbstractItemView::DragDrop);
        setDefaultDropAction(Qt::MoveAction);
        setDragDropOverwriteMode(false);
        setDropIndicatorShown(true);
        setSelectionMode(QAbstractItemView::SingleSelection);
    }

  protected:
    void startDrag(Qt::DropActions) override {
        // A column can only exist in one list; modifier keys must not copy it.
        QListWidget::startDrag(Qt::MoveAction);
    }
    void dragEnterEvent(QDragEnterEvent* event) override {
        if (accepts(event))
            QListWidget::dragEnterEvent(event);
        else
            event->ignore();
    }
    void dragMoveEvent(QDragMoveEvent* event) override {
        if (accepts(event))
            QListWidget::dragMoveEvent(event);
        else
            event->ignore();
    }
    void dropEvent(QDropEvent* event) override {
        if (!accepts(event)) {
            event->ignore();
            return;
        }
        const auto* source = event->source() == this ? this : peer;
        const auto logical = source->currentItem()->data(Qt::UserRole);
        QListWidget::dropEvent(event);
        if (event->isAccepted())
            for (int row = 0; row < count(); ++row)
                if (item(row)->data(Qt::UserRole) == logical) {
                    setCurrentRow(row);
                    break;
                }
    }

  private:
    bool accepts(const QDropEvent* event) const {
        if (!(event->possibleActions() & Qt::MoveAction))
            return false;
        if (event->source() == this)
            return currentItem() != nullptr;
        return peer && event->source() == peer && peer->currentItem() &&
               (!peer->keepOne || peer->count() > 1);
    }
};
} // namespace

ColumnSettings::ColumnSettings(QHeaderView* header, const QByteArray& defaults,
                               const QList<int>& defaultOrder, QWidget* parent)
    : QDialog(parent) {
    setObjectName("connectionColumnSettings");
    setWindowTitle(tr("Customize columns"));
    setWindowModality(Qt::WindowModal);
    setAttribute(Qt::WA_DeleteOnClose);
    resize(580, 500);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(12);
    auto* hint =
        new QLabel(tr("Drag columns between Shown and Available, or within a list to "
                      "reorder them. You can also double-click or use the buttons below."));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto* lists = new QHBoxLayout;
    auto* shown = new ColumnList(true);
    shown->setObjectName("shownConnectionColumns");
    shown->setAccessibleName(tr("Shown columns"));
    auto* available = new ColumnList(false);
    available->setObjectName("availableConnectionColumns");
    available->setAccessibleName(tr("Available columns"));
    shown->peer = available;
    available->peer = shown;
    auto* shownTitle = new QLabel;
    auto* availableTitle = new QLabel;
    for (auto* list : {shown, available}) {
        auto* column = new QVBoxLayout;
        column->addWidget(list == shown ? shownTitle : availableTitle);
        column->addWidget(list);
        lists->addLayout(column, 1);
        list->setStyleSheet(R"(
            QListWidget { background: palette(base); border: 1px solid palette(mid);
                border-radius: 8px; padding: 5px; }
            QListWidget::item { padding: 7px; margin: 2px 0; border-radius: 5px;
                background: palette(alternate-base); color: palette(text); }
            QListWidget::item:selected { background: palette(highlight); color: palette(highlighted-text); }
        )");
    }
    layout->addLayout(lists, 1);
    auto* actions = new QHBoxLayout;
    auto* remove = new QPushButton(tr("Remove"));
    remove->setObjectName("hideConnectionColumnButton");
    auto* up = new QPushButton(tr("Move up"));
    up->setObjectName("moveConnectionColumnUpButton");
    auto* down = new QPushButton(tr("Move down"));
    down->setObjectName("moveConnectionColumnDownButton");
    auto* add = new QPushButton(tr("Add"));
    add->setObjectName("showConnectionColumnButton");
    actions->addWidget(remove);
    actions->addWidget(up);
    actions->addWidget(down);
    actions->addStretch();
    actions->addWidget(add);
    layout->addLayout(actions);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel |
                                         QDialogButtonBox::RestoreDefaults);
    buttons->button(QDialogButtonBox::Save)->setObjectName("saveConnectionColumnsButton");
    buttons->button(QDialogButtonBox::Cancel)->setObjectName("cancelConnectionColumnsButton");
    buttons->button(QDialogButtonBox::RestoreDefaults)
        ->setObjectName("resetConnectionColumnsButton");
    layout->addWidget(buttons);
    auto append = [header](QListWidget* list, int logical) {
        auto* item = new QListWidgetItem(
            header->model()->headerData(logical, Qt::Horizontal).toString(), list);
        item->setData(Qt::UserRole, logical);
    };
    for (int visual = 0; visual < header->count(); ++visual) {
        const auto logical = header->logicalIndex(visual);
        append(header->isSectionHidden(logical) ? available : shown, logical);
    }
    auto refresh = [=] {
        shownTitle->setText(tr("Shown (%1)").arg(shown->count()));
        availableTitle->setText(tr("Available (%1)").arg(available->count()));
        remove->setEnabled(shown->currentRow() >= 0 && shown->count() > 1);
        remove->setToolTip(shown->count() == 1 ? tr("Keep at least one column shown") : QString{});
        add->setEnabled(available->currentRow() >= 0);
        up->setEnabled(shown->currentRow() > 0);
        down->setEnabled(shown->currentRow() >= 0 && shown->currentRow() < shown->count() - 1);
        buttons->button(QDialogButtonBox::Save)->setEnabled(shown->count() > 0);
    };
    auto hideColumn = [=] {
        if (shown->currentRow() < 0 || shown->count() <= 1)
            return;
        auto* item = shown->takeItem(shown->currentRow());
        available->addItem(item);
        available->setCurrentItem(item);
        refresh();
    };
    auto showColumn = [=] {
        if (available->currentRow() < 0)
            return;
        auto* item = available->takeItem(available->currentRow());
        shown->addItem(item);
        shown->setCurrentItem(item);
        refresh();
    };
    connect(remove, &QPushButton::clicked, this, hideColumn);
    connect(shown, &QListWidget::itemDoubleClicked, this, hideColumn);
    connect(add, &QPushButton::clicked, this, showColumn);
    connect(available, &QListWidget::itemDoubleClicked, this, showColumn);
    auto move = [=](int offset) {
        const int row = shown->currentRow(), target = row + offset;
        if (row < 0 || target < 0 || target >= shown->count())
            return;
        auto* item = shown->takeItem(row);
        shown->insertItem(target, item);
        shown->setCurrentItem(item);
        refresh();
    };
    connect(up, &QPushButton::clicked, this, [move] { move(-1); });
    connect(down, &QPushButton::clicked, this, [move] { move(1); });
    connect(shown, &QListWidget::currentRowChanged, this, refresh);
    connect(available, &QListWidget::currentRowChanged, this, refresh);
    for (auto* list : {shown, available}) {
        connect(list->model(), &QAbstractItemModel::rowsMoved, this, refresh);
        connect(list->model(), &QAbstractItemModel::rowsInserted, this, refresh);
        connect(list->model(), &QAbstractItemModel::rowsRemoved, this, refresh);
    }
    auto reset = std::make_shared<bool>(false);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this, [=] {
        *reset = true;
        shown->clear();
        available->clear();
        for (int logical : defaultOrder)
            append(shown, logical);
        shown->setCurrentRow(0);
        refresh();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [=] {
        if (!shown->count())
            return;
        if (*reset)
            header->restoreState(defaults);
        for (int row = 0; row < shown->count(); ++row) {
            const int logical = shown->item(row)->data(Qt::UserRole).toInt();
            header->showSection(logical);
            header->moveSection(header->visualIndex(logical), row);
        }
        for (int row = 0; row < available->count(); ++row)
            header->hideSection(available->item(row)->data(Qt::UserRole).toInt());
        emit applied();
        accept();
    });
    shown->setCurrentRow(0);
    refresh();
}
