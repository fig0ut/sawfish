#include "homepage.h"
#include "app/controller.h"
#include "widgets/controls.h"
#include "widgets/dashboard.h"
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

HomePage::HomePage(Controller* controller, QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(28, 28, 28, 24);
    layout->setSpacing(18);

    auto* title = new QLabel(tr("Home"));
    auto font = title->font();
    font.setPointSize(24);
    font.setBold(true);
    title->setFont(font);
    auto* heading = new QHBoxLayout;
    heading->addWidget(title);
    heading->addStretch();
    auto* add = new SymbolButton(SymbolButton::Plus);
    add->setObjectName("addWidgetButton");
    add->setAccessibleName(tr("Add widget"));
    add->setToolTip(add->accessibleName());
    add->setPopupMode(QToolButton::InstantPopup);
    add->setStyleSheet("QToolButton::menu-indicator { image: none; width: 0; height: 0; }");
    auto* menu = new RoundedMenu(add);
    add->setMenu(menu);
    add->hide();
    heading->addWidget(add);
    auto* edit = m_edit = new SymbolButton(SymbolButton::Pen);
    edit->setObjectName("editWidgetsButton");
    edit->setCheckable(true);
    edit->setAccessibleName(tr("Edit widgets"));
    edit->setToolTip(edit->accessibleName());
    heading->addWidget(edit);
    layout->addLayout(heading);

    auto* connectionRow = new QHBoxLayout;
    auto* state = new QLabel;
    state->setObjectName("connectionState");
    font.setPointSize(18);
    state->setFont(font);
    connectionRow->addWidget(state);
    connectionRow->addStretch();
    auto* toggle = new ConnectionSwitch;
    toggle->setObjectName("connectionSwitch");
    connectionRow->addWidget(toggle);
    auto* retry = new QPushButton(tr("Retry"));
    retry->setObjectName("retryButton");
    connectionRow->addWidget(retry);
    layout->addLayout(connectionRow);

    auto* form = new QFormLayout;
    auto* profiles = new ThemedComboBox;
    profiles->setObjectName("homeProfileSelector");
    profiles->setModel(controller->profiles());
    profiles->setPlaceholderText(tr("Import a configuration in Profiles"));
    form->addRow(tr("Profile"), profiles);
    layout->addLayout(form);
    auto* active = new QLabel;
    active->setTextFormat(Qt::PlainText);
    active->setWordWrap(true);
    layout->addWidget(active);

    auto* proxyRow = new QHBoxLayout;
    auto* proxyLabel = new QLabel(tr("System proxy"));
    auto* proxyToggle = new ToggleSwitch;
    proxyToggle->setObjectName("systemProxySwitch");
    proxyToggle->setAccessibleName(tr("System proxy"));
    proxyToggle->setToolTip(tr("Use Sawfish for applications that follow desktop proxy settings."));
    proxyLabel->setBuddy(proxyToggle);
    proxyRow->addWidget(proxyLabel);
    proxyRow->addStretch();
    proxyRow->addWidget(proxyToggle);
    layout->addLayout(proxyRow);
#ifndef Q_OS_LINUX
    proxyLabel->hide();
    proxyToggle->hide();
#endif
    connect(proxyToggle, &QAbstractButton::clicked, controller, &Controller::setSystemProxyEnabled);

    auto* dashboard = new Dashboard(controller);
    layout->addWidget(dashboard, 1);
    connect(edit, &QToolButton::toggled, this, [=](bool editing) {
        dashboard->setEditing(editing);
        add->setVisible(editing);
        edit->setSymbol(editing ? SymbolButton::Save : SymbolButton::Pen);
        edit->setAccessibleName(editing ? tr("Save widgets") : tr("Edit widgets"));
        edit->setToolTip(edit->accessibleName());
    });
    for (int kind = Dashboard::Upload; kind <= Dashboard::Traffic; ++kind) {
        auto* action = menu->addAction(Dashboard::title(Dashboard::Kind(kind)));
        action->setObjectName(QStringLiteral("addWidget%1").arg(kind));
        connect(action, &QAction::triggered, dashboard,
                [=] { dashboard->addWidget(Dashboard::Kind(kind)); });
        auto updateAction = [=] {
            action->setEnabled(!dashboard->contains(Dashboard::Kind(kind)));
        };
        connect(dashboard, &Dashboard::layoutChanged, action, updateAction);
        updateAction();
    }

    connect(toggle, &QAbstractButton::clicked, controller, [controller](bool checked) {
        if (checked)
            controller->connectService();
        else
            controller->disconnectService();
    });
    connect(retry, &QPushButton::clicked, controller, &Controller::retryNow);
    connect(profiles, qOverload<int>(&QComboBox::activated), controller, [controller](int row) {
        if (const auto* profile = controller->profiles()->at(row))
            controller->selectProfile(profile->id);
    });

    auto refresh = [=] {
        QSignalBlocker proxyBlocker(proxyToggle);
        proxyToggle->setChecked(controller->systemProxyEnabled());
        proxyToggle->setEnabled(
            (controller->systemProxyAvailable() || controller->systemProxyEnabled()) &&
            !controller->busy() && !controller->systemProxyBusy() &&
            !controller->connectionChanging());
        state->setText(controller->stateText());
        const bool connected = controller->state() == Controller::Running ||
                               controller->state() == Controller::Stopping;
        QSignalBlocker toggleBlocker(toggle);
        toggle->setChecked(connected);
        toggle->setEnabled(connected ? controller->canDisconnect() : controller->canConnect());
        toggle->setToolTip(connected ? tr("Pause connection") : tr("Start connection"));
        toggle->setAccessibleDescription(state->text());
        const bool different = connected && !controller->activeName().isEmpty() &&
                               controller->activeId() != controller->selectedId();
        active->setText(different ? tr("Still using %1").arg(controller->activeName()) : QString{});
        active->setVisible(different);
        retry->setEnabled(!controller->busy());
        retry->setVisible(!controller->online() && controller->messageIsError());
        profiles->setEnabled(!controller->busy());
        QSignalBlocker blocker(profiles);
        profiles->setCurrentIndex(profiles->findData(controller->selectedId(), Qt::UserRole));
    };
    connect(controller, &Controller::changed, this, refresh);
    connect(controller, &Controller::profileChanged, this, refresh);

    refresh();
}

void HomePage::finishEditing() {
    m_edit->setChecked(false);
}
