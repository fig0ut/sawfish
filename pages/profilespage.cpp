#include "profilespage.h"
#include "app/controller.h"
#include "widgets/controls.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGridLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QUrl>
#include <QVBoxLayout>

namespace {
class ValidationBar : public QWidget {
  public:
    Profile::Validation validation = Profile::Pending;
    explicit ValidationBar(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedSize(4, 22);
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        // These two semantic colors are intentionally independent of the accent.
        const bool dark = palette().color(QPalette::Base).lightness() < 128;
        const auto color = validation == Profile::Valid     ? QColor(dark ? "#69d69a" : "#18834b")
                           : validation == Profile::Invalid ? QColor(dark ? "#ff6b6b" : "#c52b35")
                                                            : palette().color(QPalette::Mid);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawRoundedRect(rect(), 2, 2);
    }
};
class ProfileCard : public QWidget {
  public:
    QLabel *name, *source, *status, *selected;
    ValidationBar* indicator;
    SymbolButton* menuButton;
    QAction *selectAction, *editAction, *updateAction, *deleteAction;
    bool isSelected = false;
    QString fullName;
    explicit ProfileCard(const QString& id, QWidget* parent) : QWidget(parent) {
        setObjectName("profileCard");
        setProperty("profileId", id);
        setMinimumWidth(240);
        setFixedHeight(154);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(16, 12, 12, 14);
        layout->setSpacing(8);
        auto* header = new QHBoxLayout;
        header->setSpacing(8);
        indicator = new ValidationBar;
        indicator->setObjectName("validationIndicator");
        header->addWidget(indicator);
        name = new QLabel;
        name->setObjectName("profileName");
        name->setTextFormat(Qt::PlainText);
        auto font = name->font();
        font.setBold(true);
        name->setFont(font);
        name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        header->addWidget(name, 1);
        menuButton = new SymbolButton(SymbolButton::More);
        menuButton->setObjectName("profileMenuButton");
        menuButton->setPopupMode(QToolButton::InstantPopup);
        auto* menu = new RoundedMenu(menuButton);
        selectAction = menu->addAction(tr("Select"));
        selectAction->setObjectName("selectProfileAction");
        editAction = menu->addAction(tr("Edit"));
        editAction->setObjectName("editProfileAction");
        updateAction = menu->addAction(tr("Update"));
        updateAction->setObjectName("updateProfileAction");
        menu->addSeparator();
        deleteAction = menu->addAction(tr("Delete"));
        deleteAction->setObjectName("deleteProfileAction");
        menuButton->setMenu(menu);
        header->addWidget(menuButton);
        layout->addLayout(header);
        source = new QLabel;
        source->setTextFormat(Qt::PlainText);
        source->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        layout->addWidget(source);
        layout->addStretch();
        auto* footer = new QHBoxLayout;
        status = new QLabel;
        status->setObjectName("profileValidation");
        status->setTextFormat(Qt::PlainText);
        footer->addWidget(status);
        footer->addStretch();
        selected = new QLabel(tr("Selected"));
        selected->setStyleSheet("background: palette(highlight); color: palette(highlighted-text); "
                                "border-radius: 5px; padding: 3px 7px;");
        footer->addWidget(selected);
        layout->addLayout(footer);
    }
    void refresh(const Profile& profile, Controller* controller) {
        fullName = profile.name;
        name->setText(name->fontMetrics().elidedText(fullName, Qt::ElideRight, name->width()));
        name->setToolTip(fullName);
        setAccessibleName(fullName);
        menuButton->setAccessibleName(tr("Actions for %1").arg(fullName));
        menuButton->setToolTip(tr("Profile actions"));
        // Display only the host: subscription URLs can contain private tokens.
        source->setText(profile.url.isEmpty() ? tr("Local configuration")
                                              : QUrl(profile.url).host());
        indicator->validation = profile.validation;
        indicator->setProperty("validation", int(profile.validation));
        indicator->update();
        const auto text = profile.validation == Profile::Valid      ? tr("Check passed")
                          : profile.validation == Profile::Invalid  ? tr("Check failed")
                          : profile.validation == Profile::Checking ? tr("Checking…")
                                                                    : tr("Waiting for check…");
        status->setText(text);
        status->setToolTip(profile.validationError);
        indicator->setToolTip(text);
        indicator->setAccessibleName(text);
        isSelected = profile.id == controller->selectedId();
        selected->setVisible(isSelected);
        selectAction->setVisible(!isSelected);
        selectAction->setEnabled(!controller->busy());
        updateAction->setVisible(!profile.url.isEmpty());
        updateAction->setText(controller->updatingProfile(profile.id) ? tr("Updating…")
                                                                      : tr("Update"));
        updateAction->setEnabled(!controller->busy() && !controller->importing());
        editAction->setEnabled(!controller->busy());
        deleteAction->setEnabled(!controller->busy());
        update();
    }

  protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        name->setText(name->fontMetrics().elidedText(fullName, Qt::ElideRight, name->width()));
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(palette().brush(QPalette::Base));
        p.setPen(QPen(palette().color(isSelected ? QPalette::Highlight : QPalette::Mid),
                      isSelected ? 2 : 1));
        p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 10, 10);
    }
};

void editProfile(Controller* controller, const QString& id, QWidget* parent) {
    const auto* profile = controller->profiles()->find(id);
    if (!profile)
        return;
    auto* dialog = new QDialog(parent);
    dialog->setObjectName("profileEditor");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QObject::tr("Edit profile"));
    dialog->setWindowModality(Qt::WindowModal);
    dialog->resize(680, 540);
    auto* layout = new QVBoxLayout(dialog);
    auto* form = new QFormLayout;
    auto* name = new QLineEdit(profile->name);
    name->setObjectName("profileNameEdit");
    auto* url = new QLineEdit(profile->url);
    url->setObjectName("profileUrlEdit");
    url->setPlaceholderText(QObject::tr("https://… (optional)"));
    form->addRow(QObject::tr("Name"), name);
    form->addRow(QObject::tr("Source URL"), url);
    layout->addLayout(form);
    auto* content = new QPlainTextEdit(profile->content);
    content->setObjectName("profileContentEdit");
    content->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    content->setAccessibleName(QObject::tr("Configuration content"));
    layout->addWidget(content, 1);
    auto* hint = new QLabel(
        QObject::tr("The content above is used to connect. Saving checks it automatically."));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto* error = new QLabel(profile->validationError);
    error->setObjectName("profileEditorError");
    error->setTextFormat(Qt::PlainText);
    error->setWordWrap(true);
    error->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Save)->setObjectName("saveProfileButton");
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    QObject::connect(controller, &Controller::changed, dialog, [controller, buttons] {
        buttons->button(QDialogButtonBox::Save)->setEnabled(!controller->busy());
    });
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, [=] {
        if (controller->saveProfile(id, name->text(), url->text(), content->toPlainText()))
            dialog->accept();
        else
            error->setText(controller->message());
    });
    dialog->open();
}
} // namespace

struct ProfilePage::Ui {
    Controller* controller;
    ProfilePage* page;
    QScrollArea* scroll;
    QWidget* content;
    QGridLayout* grid;
    QLabel *empty, *result;
    SymbolButton* import;
    QMap<QString, ProfileCard*> cards;
    int columns = 0;
    void arrange() {
        const int count = std::max(1, (scroll->viewport()->width() - 8) / 272);
        if (columns == count)
            return;
        columns = count;
        while (auto* item = grid->takeAt(0))
            delete item;
        for (int col = 0; col < grid->columnCount(); ++col)
            grid->setColumnStretch(col, 0);
        for (int row = 0; row < controller->profiles()->rowCount(); ++row) {
            const auto id = controller->profiles()->at(row)->id;
            if (cards.contains(id))
                grid->addWidget(cards[id], row / columns, row % columns);
        }
        for (int col = 0; col < columns; ++col)
            grid->setColumnStretch(col, 1);
    }
    void refresh() {
        QSet<QString> retained;
        bool structureChanged = false;
        for (int row = 0; row < controller->profiles()->rowCount(); ++row) {
            const auto* profile = controller->profiles()->at(row);
            const auto id = profile->id;
            retained.insert(id);
            auto* card = cards.value(id);
            if (!card) {
                structureChanged = true;
                card = new ProfileCard(id, content);
                cards.insert(id, card);
                QObject::connect(card->selectAction, &QAction::triggered, page,
                                 [this, id] { controller->selectProfile(id); });
                QObject::connect(card->editAction, &QAction::triggered, page,
                                 [this, id] { editProfile(controller, id, page); });
                QObject::connect(card->updateAction, &QAction::triggered, page,
                                 [this, id] { controller->updateProfile(id); });
                QObject::connect(card->deleteAction, &QAction::triggered, page, [this, id] {
                    const auto* profile = controller->profiles()->find(id);
                    if (!profile)
                        return;
                    auto* box = new QMessageBox(
                        QMessageBox::Question, ProfilePage::tr("Delete profile"),
                        ProfilePage::tr("Delete “%1”?%2")
                            .arg(profile->name,
                                 controller->activeId() == id
                                     ? ProfilePage::tr(" The current connection will stop.")
                                     : QString{}),
                        QMessageBox::Cancel | QMessageBox::Yes, page);
                    box->setTextFormat(Qt::PlainText);
                    box->setAttribute(Qt::WA_DeleteOnClose);
                    box->setDefaultButton(QMessageBox::Cancel);
                    box->button(QMessageBox::Yes)->setText(ProfilePage::tr("Delete"));
                    QObject::connect(box, &QMessageBox::finished, page, [this, id](int response) {
                        if (response == QMessageBox::Yes)
                            controller->deleteProfile(id);
                    });
                    box->open();
                });
            }
            card->refresh(*profile, controller);
        }
        for (auto it = cards.begin(); it != cards.end();) {
            if (retained.contains(it.key())) {
                ++it;
            } else {
                structureChanged = true;
                it.value()->hide();
                grid->removeWidget(it.value());
                it.value()->deleteLater();
                it = cards.erase(it);
            }
        }
        if (structureChanged)
            columns = 0;
        arrange();
        empty->setVisible(cards.isEmpty());
        import->setEnabled(!controller->busy() && !controller->importing() &&
                           controller->profileError().isEmpty());
        result->setText(controller->profileError());
        result->setVisible(!result->text().isEmpty());
    }
};
ProfilePage::ProfilePage(Controller* controller, QWidget* parent)
    : QWidget(parent), m_ui(std::make_unique<Ui>()) {
    auto& ui = *m_ui;
    ui.controller = controller;
    ui.page = this;
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(28, 28, 28, 24);
    layout->setSpacing(14);
    auto* header = new QHBoxLayout;
    auto* title = new QLabel(tr("Profiles"));
    auto font = title->font();
    font.setPointSize(24);
    font.setBold(true);
    title->setFont(font);
    header->addWidget(title);
    header->addStretch();
    ui.import = new SymbolButton(SymbolButton::Plus);
    ui.import->setFixedSize(48, 48);
    ui.import->setIconSize(QSize(32, 32));
    ui.import->setObjectName("importProfileButton");
    ui.import->setAccessibleName(tr("Import profile"));
    ui.import->setToolTip(tr("Import profile"));
    ui.import->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new RoundedMenu(ui.import);
    auto* fileAction = menu->addAction(tr("From file…"));
    fileAction->setObjectName("importFileAction");
    auto* urlAction = menu->addAction(tr("From URL…"));
    urlAction->setObjectName("importUrlAction");
    ui.import->setMenu(menu);
    header->addWidget(ui.import);
    layout->addLayout(header);
    ui.empty = new QLabel(tr("Add your first profile using + above."));
    layout->addWidget(ui.empty);
    ui.scroll = new QScrollArea;
    ui.scroll->setObjectName("profileScroll");
    ui.scroll->setFrameShape(QFrame::NoFrame);
    ui.scroll->setWidgetResizable(true);
    ui.scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ui.content = new QWidget;
    ui.grid = new QGridLayout(ui.content);
    ui.grid->setContentsMargins(0, 0, 6, 0);
    ui.grid->setSpacing(14);
    ui.grid->setAlignment(Qt::AlignTop);
    ui.scroll->setWidget(ui.content);
    layout->addWidget(ui.scroll, 1);
    ui.result = new QLabel;
    ui.result->setObjectName("profileResult");
    ui.result->setTextFormat(Qt::PlainText);
    ui.result->setWordWrap(true);
    ui.result->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(ui.result);
    connect(fileAction, &QAction::triggered, this, [this, controller] {
        const auto path =
            QFileDialog::getOpenFileName(this, tr("Import Rostra configuration"), {},
                                         tr("JSON configurations (*.json *.jsonc);;All files (*)"));
        if (!path.isEmpty())
            controller->importProfile(path);
    });
    connect(urlAction, &QAction::triggered, this, [this, controller] {
        auto* dialog = new QInputDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setObjectName("importUrlDialog");
        dialog->setWindowTitle(tr("Import from URL"));
        dialog->setLabelText(tr("Rostra configuration URL (HTTP or HTTPS)"));
        dialog->setOkButtonText(tr("Import"));
        connect(dialog, &QInputDialog::textValueSelected, controller,
                &Controller::importProfileUrl);
        dialog->open();
    });
    auto refresh = [this] { m_ui->refresh(); };
    connect(controller->profiles(), &QAbstractItemModel::rowsInserted, this, refresh);
    connect(controller->profiles(), &QAbstractItemModel::rowsRemoved, this, refresh);
    connect(controller->profiles(), &QAbstractItemModel::modelReset, this, refresh);
    connect(controller->profiles(), &QAbstractItemModel::dataChanged, this, refresh);
    connect(controller, &Controller::profileChanged, this, refresh);
    connect(controller, &Controller::changed, this, refresh);
    ui.refresh();
}
ProfilePage::~ProfilePage() = default;
void ProfilePage::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    QTimer::singleShot(0, this, [this] { m_ui->arrange(); });
}
