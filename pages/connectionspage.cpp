#include "connectionspage.h"
#include "app/controller.h"
#include "models/format.h"
#include "widgets/columnsettings.h"
#include "widgets/controls.h"
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QFrame>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QTableView>
#include <QVBoxLayout>

struct ConnectionsPage::Ui {
    Controller* controller;
    ConnectionModel* model;
    ConnectionFilter* filter;
    QTableView* table;
    ThemedComboBox* view;
    QLineEdit* search;
    QLabel *count, *state;
    QPushButton *close, *closeAll;
    SymbolButton *pause, *refresh;
    QWidget* details;
    QPlainTextEdit* description;
    QString selected;
    QTimer* columnSave;
    QPointer<ColumnSettings> columnSettings;
    QByteArray defaultColumns;
    QList<int> defaultOrder;

    void saveColumns() {
        columnSave->stop();
        controller->saveConnectionsHeader(table->horizontalHeader()->saveState());
    }

    void updateDetails() {
        const auto* c = model->find(selected);
        if (!c) {
            details->hide();
            return;
        }
        QStringList lines;
        auto add = [&](const QString& name, const QString& value) {
            if (!value.isEmpty())
                lines << name + "\n" + value + "\n";
        };
        add(ConnectionsPage::tr("Host"), c->domain.isEmpty() ? c->destination : c->domain);
        add(ConnectionsPage::tr("State"),
            c->active() ? ConnectionsPage::tr("Active") : ConnectionsPage::tr("Closed"));
        add(ConnectionsPage::tr("Source → destination"), c->source + " → " + c->destination);
        add(ConnectionsPage::tr("Network / protocol"),
            c->network.toUpper() + (c->protocol.isEmpty() ? "" : " / " + c->protocol));
        add(ConnectionsPage::tr("Inbound"),
            c->inbound + (c->inboundType.isEmpty() ? "" : " (" + c->inboundType + ")"));
        add(ConnectionsPage::tr("Route"), c->chain.isEmpty() ? c->outbound : c->chain.join(" → "));
        add(ConnectionsPage::tr("Rule"), c->rule);
        add(ConnectionsPage::tr("Traffic"),
            ConnectionsPage::tr("↑ %1 (%2/s)\n↓ %3 (%4/s)")
                .arg(formatBytes(c->upload), formatBytes(c->uploadRate), formatBytes(c->download),
                     formatBytes(c->downloadRate)));
        add(ConnectionsPage::tr("Process"), c->processPath);
        if (c->processId)
            add(ConnectionsPage::tr("Process ID"), QString::number(c->processId));
        add(ConnectionsPage::tr("User"), c->userName.isEmpty() ? c->user : c->userName);
        add(ConnectionsPage::tr("Packages"), c->packages.join(", "));
        add(ConnectionsPage::tr("Started"),
            QDateTime::fromMSecsSinceEpoch(c->createdAt).toString(Qt::ISODate));
        if (c->closedAt)
            add(ConnectionsPage::tr("Closed"),
                QDateTime::fromMSecsSinceEpoch(c->closedAt).toString(Qt::ISODate));
        add(ConnectionsPage::tr("ID"), c->id);
        const auto text = lines.join('\n');
        if (description->toPlainText() != text) {
            const auto cursor = description->textCursor();
            const int scroll = description->verticalScrollBar()->value();
            description->setPlainText(text);
            auto restored = description->textCursor();
            restored.setPosition(qMin(cursor.position(), text.size()));
            description->setTextCursor(restored);
            description->verticalScrollBar()->setValue(scroll);
        }
    }
    void clearSelection() {
        table->clearSelection();
        table->setCurrentIndex({});
        selected.clear();
        details->hide();
    }
    void refreshState() {
        count->setText(ConnectionsPage::tr("%1 shown · %2 active · %3 closed%4")
                           .arg(filter->rowCount())
                           .arg(model->activeCount())
                           .arg(model->rowCount() - model->activeCount())
                           .arg(pause->isChecked() ? ConnectionsPage::tr(" · Paused") : QString{}));
        if (!controller->online() || controller->state() != Controller::Running)
            state->setText(
                ConnectionsPage::tr("Connect a profile on Home to see its connections."));
        else if (!controller->connectionsError().isEmpty())
            state->setText(ConnectionsPage::tr("Connections unavailable: %1")
                               .arg(controller->connectionsError()));
        else if (!controller->connectionsReady())
            state->setText(ConnectionsPage::tr("Loading connections…"));
        else if (!filter->rowCount())
            state->setText(search->text().isEmpty()
                               ? ConnectionsPage::tr("No connections in this view.")
                               : ConnectionsPage::tr("No connections match your search."));
        else
            state->clear();
        state->setVisible(!state->text().isEmpty());
        const auto* current = controller->connectionModel()->find(selected);
        close->setEnabled(controller->canCloseConnections() && current && current->active());
        closeAll->setEnabled(controller->canCloseConnections() &&
                             controller->connectionModel()->activeCount());
        refresh->setEnabled(controller->online() && controller->state() == Controller::Running &&
                            !controller->busy());
        updateDetails();
    }
    void synchronize() {
        // Pausing freezes only the view; closure actions still consult live state.
        if (!pause->isChecked() || !controller->online() ||
            controller->state() != Controller::Running)
            model->replace(controller->connectionModel()->snapshot());
        refreshState();
    }
};

ConnectionsPage::ConnectionsPage(Controller* controller, QWidget* parent)
    : QWidget(parent), m_ui(std::make_unique<Ui>()) {
    auto& ui = *m_ui;
    ui.controller = controller;
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(28, 28, 28, 24);
    layout->setSpacing(14);
    auto* title = new QLabel(tr("Connections"));
    auto font = title->font();
    font.setPointSize(24);
    font.setBold(true);
    title->setFont(font);
    layout->addWidget(title);
    auto* filters = new QHBoxLayout;
    ui.view = new ThemedComboBox;
    ui.view->setObjectName("connectionViewSelector");
    ui.view->addItems({tr("Active"), tr("Closed"), tr("All")});
    ui.search = new QLineEdit;
    ui.search->setObjectName("connectionSearch");
    ui.search->setPlaceholderText(tr("Search host, process, source, route or rule"));
    ui.search->setClearButtonEnabled(true);
    ui.pause = new SymbolButton(SymbolButton::Pause);
    ui.pause->setAccessibleName(tr("Pause connections"));
    ui.pause->setToolTip(ui.pause->accessibleName());
    ui.pause->setCheckable(true);
    ui.pause->setObjectName("pauseConnectionsButton");
    ui.refresh = new SymbolButton(SymbolButton::Refresh);
    ui.refresh->setAccessibleName(tr("Refresh connections"));
    ui.refresh->setToolTip(ui.refresh->accessibleName());
    ui.refresh->setObjectName("refreshConnectionsButton");
    auto* columns = new SymbolButton(SymbolButton::Settings);
    columns->setObjectName("customizeConnectionColumnsButton");
    columns->setToolTip(tr("Customize columns"));
    columns->setAccessibleName(columns->toolTip());
    filters->addWidget(ui.view);
    filters->addWidget(ui.search, 1);
    filters->addWidget(ui.pause);
    filters->addWidget(ui.refresh);
    filters->addWidget(columns);
    layout->addLayout(filters);
    auto* actions = new QHBoxLayout;
    ui.count = new QLabel;
    ui.count->setObjectName("connectionCounts");
    ui.close = new QPushButton(tr("Close selected"));
    ui.close->setObjectName("closeConnectionButton");
    ui.closeAll = new QPushButton(tr("Close all"));
    ui.closeAll->setObjectName("closeAllConnectionsButton");
    ui.closeAll->setToolTip(tr("Close all active connections, including those hidden by filters"));
    actions->addWidget(ui.count, 1);
    actions->addWidget(ui.close);
    actions->addWidget(ui.closeAll);
    layout->addLayout(actions);
    ui.state = new QLabel;
    ui.state->setWordWrap(true);
    ui.state->setTextFormat(Qt::PlainText);
    ui.state->setObjectName("connectionsState");
    layout->addWidget(ui.state);
    auto* splitter = new QSplitter;
    ui.model = new ConnectionModel(this);
    ui.filter = new ConnectionFilter(this);
    ui.filter->setSourceModel(ui.model);
    ui.table = new QTableView;
    ui.table->setObjectName("connectionsTable");
    ui.table->setFrameShape(QFrame::NoFrame);
    ui.table->setStyleSheet(R"(
        QTableView { border: none; background: palette(base); }
        QHeaderView::section { background: palette(alternate-base); color: palette(text);
            border: none; border-bottom: 1px solid palette(mid); padding: 7px; }
    )");
    ui.table->setModel(ui.filter);
    ui.table->setSortingEnabled(true);
    ui.table->sortByColumn(ConnectionModel::Started, Qt::DescendingOrder);
    ui.table->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui.table->setSelectionMode(QAbstractItemView::SingleSelection);
    ui.table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui.table->setAlternatingRowColors(true);
    ui.table->setShowGrid(false);
    ui.table->verticalHeader()->hide();
    ui.table->verticalHeader()->setDefaultSectionSize(38);
    ui.table->horizontalHeader()->setHighlightSections(false);
    ui.table->horizontalHeader()->setSectionsMovable(true);
    ui.table->horizontalHeader()->setDefaultSectionSize(130);
    ui.table->setColumnWidth(ConnectionModel::Host, 210);
    ui.table->setColumnWidth(ConnectionModel::Route, 240);
    const QList<int> order{
        ConnectionModel::Host,        ConnectionModel::UploadRate, ConnectionModel::DownloadRate,
        ConnectionModel::Upload,      ConnectionModel::Download,   ConnectionModel::Route,
        ConnectionModel::Process,     ConnectionModel::Network,    ConnectionModel::Source,
        ConnectionModel::Destination, ConnectionModel::Started,    ConnectionModel::State};
    for (int section = 0; section < order.size(); ++section)
        ui.table->horizontalHeader()->moveSection(
            ui.table->horizontalHeader()->visualIndex(order[section]), section);
    ui.table->setColumnWidth(ConnectionModel::Host, 180);
    for (int column = ConnectionModel::UploadRate; column <= ConnectionModel::Download; ++column)
        ui.table->setColumnWidth(column, 100);
    ui.defaultOrder = order;
    auto* header = ui.table->horizontalHeader();
    ui.defaultColumns = header->saveState();
    if (!controller->connectionsHeader().isEmpty() &&
        !header->restoreState(controller->connectionsHeader()))
        header->restoreState(ui.defaultColumns);
    if (header->hiddenSectionCount() == header->count())
        header->showSection(ConnectionModel::Host);
    ui.columnSave = new QTimer(this);
    ui.columnSave->setSingleShot(true);
    ui.columnSave->setInterval(250);
    connect(header, &QHeaderView::sectionMoved, ui.columnSave,
            [this] { m_ui->columnSave->start(); });
    connect(header, &QHeaderView::sectionResized, ui.columnSave,
            [this] { m_ui->columnSave->start(); });
    connect(ui.columnSave, &QTimer::timeout, this, [this] { m_ui->saveColumns(); });
    connect(columns, &QToolButton::clicked, this, [this] {
        if (m_ui->columnSettings) {
            m_ui->columnSettings->raise();
            m_ui->columnSettings->activateWindow();
            return;
        }
        auto* dialog = new ColumnSettings(m_ui->table->horizontalHeader(), m_ui->defaultColumns,
                                          m_ui->defaultOrder, this);
        m_ui->columnSettings = dialog;
        connect(dialog, &ColumnSettings::applied, this, [this] { m_ui->saveColumns(); });
        dialog->open();
    });
    auto* tableCard = new QFrame;
    tableCard->setObjectName("connectionsList");
    tableCard->setStyleSheet("#connectionsList { background: palette(base); border: 1px solid "
                             "palette(mid); border-radius: 12px; }");
    auto* tableLayout = new QVBoxLayout(tableCard);
    tableLayout->setContentsMargins(7, 7, 7, 7);
    tableLayout->addWidget(ui.table);
    splitter->addWidget(tableCard);
    ui.details = new QWidget;
    ui.details->setObjectName("connectionDetails");
    ui.details->setMinimumWidth(240);
    auto* detailLayout = new QVBoxLayout(ui.details);
    detailLayout->setContentsMargins(10, 0, 0, 0);
    auto* detailHeader = new QHBoxLayout;
    detailHeader->addWidget(new QLabel(tr("Details")), 1);
    auto* copy = new QPushButton(tr("Copy"));
    detailHeader->addWidget(copy);
    auto* hide = new SymbolButton(SymbolButton::Close);
    hide->setObjectName("hideConnectionDetailsButton");
    hide->setAccessibleName(tr("Hide details"));
    hide->setToolTip(hide->accessibleName());
    detailHeader->addWidget(hide);
    detailLayout->addLayout(detailHeader);
    ui.description = new QPlainTextEdit;
    ui.description->setObjectName("connectionDescription");
    ui.description->setReadOnly(true);
    detailLayout->addWidget(ui.description);
    splitter->addWidget(ui.details);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 1);
    ui.details->hide();
    layout->addWidget(splitter, 1);
    auto showDetails = [this](const QModelIndex& index) {
        if (!index.isValid())
            return;
        m_ui->selected = index.data(ConnectionModel::IdRole).toString();
        m_ui->details->show();
        m_ui->refreshState();
        m_ui->description->verticalScrollBar()->setValue(0);
    };
    connect(ui.table->selectionModel(), &QItemSelectionModel::currentRowChanged, this, showDetails);
    connect(ui.table, &QTableView::clicked, this, showDetails);
    connect(ui.table, &QTableView::activated, this, showDetails);
    connect(hide, &QToolButton::clicked, ui.details, &QWidget::hide);
    connect(copy, &QPushButton::clicked, this,
            [this] { QApplication::clipboard()->setText(m_ui->description->toPlainText()); });
    connect(ui.view, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int view) {
        m_ui->clearSelection();
        m_ui->filter->setView(ConnectionFilter::View(view));
        m_ui->refreshState();
    });
    connect(ui.search, &QLineEdit::textChanged, this, [this](const QString& text) {
        m_ui->clearSelection();
        m_ui->filter->setFilterFixedString(text.trimmed());
        m_ui->refreshState();
    });
    connect(ui.pause, &QToolButton::toggled, this, [this](bool paused) {
        m_ui->pause->setSymbol(paused ? SymbolButton::Play : SymbolButton::Pause);
        m_ui->pause->setAccessibleName(paused ? tr("Resume connections") : tr("Pause connections"));
        m_ui->pause->setToolTip(m_ui->pause->accessibleName());
        m_ui->synchronize();
    });
    connect(ui.close, &QPushButton::clicked, this,
            [this] { m_ui->controller->closeConnection(m_ui->selected); });
    connect(ui.closeAll, &QPushButton::clicked, controller, &Controller::closeAllConnections);
    connect(ui.refresh, &QToolButton::clicked, controller, &Controller::refreshConnections);
    connect(controller, &Controller::connectionsChanged, this, [this] { m_ui->synchronize(); });
    connect(controller, &Controller::changed, this, [this] { m_ui->synchronize(); });
    ui.synchronize();
}
ConnectionsPage::~ConnectionsPage() {
    if (m_ui->columnSave->isActive())
        m_ui->saveColumns();
}

void ConnectionsPage::hideEvent(QHideEvent* event) {
    if (m_ui->columnSave->isActive())
        m_ui->saveColumns();
    QWidget::hideEvent(event);
}
