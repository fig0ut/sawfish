#include "settingspage.h"
#include "app/controller.h"
#include "backend/coreupdater.h"
#include "widgets/controls.h"
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

SettingPage::SettingPage(Controller* controller, QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(28, 28, 28, 24);
    layout->setSpacing(16);
    auto* title = new QLabel(tr("Settings"));
    auto font = title->font();
    font.setPointSize(24);
    font.setBold(true);
    title->setFont(font);
    layout->addWidget(title);
    auto* version = new QLabel;
    version->setTextFormat(Qt::PlainText);
    layout->addWidget(version);
    auto* updates = controller->coreUpdater();
    auto* channel = new ThemedComboBox;
    channel->setObjectName("coreUpdateChannel");
    channel->addItem(tr("Stable release"), CoreUpdater::Stable);
    channel->addItem(tr("Prerelease"), CoreUpdater::Prerelease);
    channel->setCurrentIndex(updates->channel());
    auto* updateForm = new QFormLayout;
    updateForm->addRow(tr("Core update channel"), channel);
    layout->addLayout(updateForm);
    auto* updateStatus = new QLabel;
    updateStatus->setObjectName("coreUpdateStatus");
    updateStatus->setTextFormat(Qt::PlainText);
    updateStatus->setWordWrap(true);
    layout->addWidget(updateStatus);
    auto* progress = new QProgressBar;
    progress->setObjectName("coreUpdateProgress");
    layout->addWidget(progress);
    auto* updateButtons = new QHBoxLayout;
    auto* check = new QPushButton(tr("Check for updates"));
    check->setObjectName("checkCoreUpdatesButton");
    auto* install = new QPushButton(tr("Install update"));
    install->setObjectName("installCoreUpdateButton");
    auto* cancel = new QPushButton(tr("Cancel"));
    cancel->setObjectName("cancelCoreUpdateButton");
    updateButtons->addWidget(check);
    updateButtons->addWidget(install);
    updateButtons->addWidget(cancel);
    updateButtons->addStretch();
    layout->addLayout(updateButtons);
    auto* updateHint =
        new QLabel(tr("Installing briefly interrupts your connection. Sawfish checks the new core "
                      "and restores the previous version if activation fails."));
    updateHint->setWordWrap(true);
    layout->addWidget(updateHint);
    connect(channel, &QComboBox::currentIndexChanged, updates, [=](int index) {
        updates->setChannel(CoreUpdater::Channel(channel->itemData(index).toInt()));
    });
    connect(check, &QPushButton::clicked, updates, &CoreUpdater::check);
    connect(install, &QPushButton::clicked, updates, &CoreUpdater::install);
    connect(cancel, &QPushButton::clicked, updates, &CoreUpdater::cancel);
    auto refreshUpdates = [=] {
        channel->setEnabled(!updates->busy());
        check->setEnabled(!updates->busy() && !controller->shuttingDown());
        install->setVisible(updates->state() == CoreUpdater::Available);
        install->setEnabled(!controller->busy() && controller->managedBackend() &&
                            !controller->shuttingDown() && !controller->importing() &&
                            controller->state() != Controller::Starting &&
                            controller->state() != Controller::Stopping);
        install->setText(tr("Install %1").arg(updates->availableVersion()));
        cancel->setVisible(updates->busy() && updates->state() != CoreUpdater::Installing);
        updateStatus->setText(updates->status());
        progress->setVisible(updates->busy());
        progress->setRange(0, updates->state() == CoreUpdater::Downloading ? 100 : 0);
        progress->setValue(updates->progress());
    };
    connect(updates, &CoreUpdater::changed, this, refreshUpdates);
    connect(controller, &Controller::changed, this, refreshUpdates);
    refreshUpdates();
    auto* advanced = new QToolButton;
    advanced->setText(tr("Advanced"));
    advanced->setObjectName("advancedSettingsButton");
    advanced->setCheckable(true);
    advanced->setArrowType(Qt::RightArrow);
    advanced->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    layout->addWidget(advanced, 0, Qt::AlignLeft);
    auto* panel = new QWidget;
    panel->setObjectName("advancedSettings");
    auto* panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    auto* form = new QFormLayout;
    auto* binaryRow = new QHBoxLayout;
    auto* executable = new QLineEdit(controller->executable());
    auto* browse = new QPushButton(tr("Browse…"));
    binaryRow->addWidget(executable, 1);
    binaryRow->addWidget(browse);
    form->addRow(tr("Rostra executable"), binaryRow);
    auto* socket = new QLabel;
    socket->setTextFormat(Qt::PlainText);
    socket->setWordWrap(true);
    socket->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Private endpoint"), socket);
    auto* worker = new QLabel;
    worker->setWordWrap(true);
    worker->setTextFormat(Qt::PlainText);
    form->addRow(tr("Configuration worker"), worker);
    panelLayout->addLayout(form);
    auto* save = new QPushButton(tr("Apply"));
    panelLayout->addWidget(save, 0, Qt::AlignLeft);
    auto* hint = new QLabel(tr("The matching Rostra executable is normally bundled with Sawfish. "
                               "Disconnect before changing it."));
    hint->setWordWrap(true);
    panelLayout->addWidget(hint);
    panel->hide();
    layout->addWidget(panel);
    layout->addStretch();
    connect(advanced, &QToolButton::toggled, this, [=](bool open) {
        advanced->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
        panel->setVisible(open);
    });
    connect(browse, &QPushButton::clicked, this, [this, executable] {
        const auto path =
            QFileDialog::getOpenFileName(this, tr("Select Rostra executable"), executable->text());
        if (!path.isEmpty())
            executable->setText(path);
    });
    connect(save, &QPushButton::clicked, controller,
            [=] { controller->configure(executable->text().trimmed(), controller->socket()); });
    auto refresh = [=] {
        save->setEnabled(controller->canConfigure());
        browse->setEnabled(controller->canConfigure());
        executable->setEnabled(controller->canConfigure());
        version->setText(controller->online() ? tr("Rostra %1").arg(controller->daemonVersion())
                                              : tr("Rostra is not ready."));
        worker->setText(controller->workerStatus());
        socket->setText(controller->socket());
    };
    connect(controller, &Controller::changed, this, refresh);
    refresh();
}
