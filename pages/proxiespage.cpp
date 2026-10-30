#include "proxiespage.h"
#include "app/controller.h"
#include "widgets/controls.h"
#include "widgets/icons.h"
#include <QAbstractButton>
#include <QApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMap>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QStyledItemDelegate>
#include <QToolTip>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

namespace {
constexpr int CardHeight = 84;
constexpr int MinimumCardWidth = 210;

QFont detailFont(QFont font) {
    font.setPointSizeF(std::max(8.0, font.pointSizeF() - 1));
    return font;
}

QString delayText(Controller* controller, const QModelIndex& index) {
    const auto outbound = index.data(GroupModel::OutboundTagRole).toString();
    return controller->testingDelay(outbound) ? QObject::tr("Testing…")
           : !controller->delayError(outbound).isEmpty()
               ? QObject::tr("Retry")
               : index.siblingAtColumn(3).data().toString();
}
QRect delayBadge(const QRect& cell, const QFont& font, const QString& text) {
    const auto rect = cell.adjusted(4, 4, -4, -4);
    const int width = QFontMetrics(detailFont(font)).horizontalAdvance(text) + 16;
    return {rect.right() - width - 10, rect.bottom() - 30, width, 22};
}

// The fill represents the daemon's selection. Keyboard focus has its own outline.
class ProxyCardDelegate : public QStyledItemDelegate {
  public:
    ProxyCardDelegate(Controller* controller, QObject* parent)
        : QStyledItemDelegate(parent), m_controller(controller) {}

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        if (const auto* view = qobject_cast<const QListView*>(parent());
            view && view->gridSize().isValid())
            return view->gridSize();
        return {MinimumCardWidth, CardHeight};
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        const auto group = index.data(GroupModel::GroupTagRole).toString();
        const auto outbound = index.data(GroupModel::OutboundTagRole).toString();
        const bool active = index.data(GroupModel::SelectedRole).toBool();
        const bool pending = m_controller->selectingOutbound(group, outbound);
        const bool actionable = m_controller->canSelectOutbound(group, outbound);
        const auto& palette = option.palette;
        const auto rect = option.rect.adjusted(4, 4, -4, -4);
        const auto foreground = palette.color(active ? QPalette::HighlightedText : QPalette::Text);
        auto background = palette.color(active ? QPalette::Highlight : QPalette::AlternateBase);
        if (!active && actionable && (option.state & QStyle::State_MouseOver))
            background = palette.color(QPalette::Midlight);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setBrush(background);
        painter->setPen(QPen(active || pending ? palette.color(QPalette::Highlight)
                                               : palette.color(QPalette::Mid),
                             pending ? 2 : 1));
        painter->drawRoundedRect(rect, 8, 8);
        auto nameFont = option.font;
        nameFont.setBold(true);
        painter->setFont(nameFont);
        painter->setPen(foreground);
        const auto nameRect = rect.adjusted(12, 9, active ? -32 : -12, -39);
        painter->drawText(
            nameRect, Qt::AlignLeft | Qt::AlignVCenter,
            QFontMetrics(nameFont).elidedText(outbound, Qt::ElideRight, nameRect.width()));

        const auto font = detailFont(option.font);
        painter->setFont(font);
        const QFontMetrics metrics(font);
        const auto latency = delayText(m_controller, index);
        const auto badge = delayBadge(option.rect, option.font, latency);
        auto badgeColor = foreground;
        badgeColor.setAlpha(active ? 38 : 16);
        painter->setPen(Qt::NoPen);
        painter->setBrush(badgeColor);
        painter->drawRoundedRect(badge, 5, 5);
        painter->setPen(foreground);
        painter->drawText(badge, Qt::AlignCenter, latency);
        const auto detail = pending ? tr("Switching…") : index.siblingAtColumn(1).data().toString();
        const QRect detailRect(rect.left() + 12, badge.top(), badge.left() - rect.left() - 20,
                               badge.height());
        painter->drawText(detailRect, Qt::AlignVCenter | Qt::AlignLeft,
                          metrics.elidedText(detail, Qt::ElideRight, detailRect.width()));
        if (option.state & QStyle::State_HasFocus) {
            painter->setBrush(Qt::NoBrush);
            painter->setPen(QPen(foreground, 1, Qt::DashLine));
            painter->drawRoundedRect(rect.adjusted(3, 3, -3, -3), 5, 5);
        }
        painter->restore();
    }

  private:
    Controller* m_controller;
};

class ProxyCardView : public QListView {
  public:
    ProxyCardView(Controller* controller, QWidget* parent)
        : QListView(parent), m_controller(controller) {
        setObjectName("outboundCards");
        setViewMode(QListView::IconMode);
        setMovement(QListView::Static);
        setResizeMode(QListView::Adjust);
        setWrapping(true);
        setUniformItemSizes(true);
        setSelectionMode(QAbstractItemView::SingleSelection);
        setEditTriggers(QAbstractItemView::NoEditTriggers);
        setFrameShape(QFrame::NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMouseTracking(true);
        setStyleSheet("QListView { background: transparent; border: none; }");
        setItemDelegate(new ProxyCardDelegate(controller, this));
        setModel(controller->groupModel());
        connect(this, &QListView::clicked, this,
                [this](const QModelIndex& index) { choose(index); });
    }

    void arrange() {
        // Icon-mode layout wraps a cell whose right edge equals the viewport width.
        const int available = std::max(1, viewport()->width() - 1);
        const int columns = std::max(1, available / MinimumCardWidth);
        int visible = 0;
        for (int row = 0; row < model()->rowCount(rootIndex()); ++row)
            if (!isRowHidden(row))
                ++visible;
        const QSize grid(available / columns, CardHeight);
        if (gridSize() != grid)
            setGridSize(grid);
        const int wanted = ((visible + columns - 1) / columns) * CardHeight;
        if (height() != wanted)
            setFixedHeight(wanted);
    }

  protected:
    void resizeEvent(QResizeEvent* event) override {
        QListView::resizeEvent(event);
        arrange();
    }
    void mousePressEvent(QMouseEvent* event) override {
        const auto index = indexAt(event->position().toPoint());
        m_foldPress =
            event->button() == Qt::LeftButton && !hitsCard(index, event->position().toPoint());
        if (m_foldPress) {
            m_delayPress.clear();
            event->accept();
            return;
        }
        if (event->button() == Qt::LeftButton && hitsDelay(index, event->position().toPoint())) {
            m_delayPress = index.data(GroupModel::OutboundTagRole).toString();
            setCurrentIndex(index);
            setFocus(Qt::MouseFocusReason);
            event->accept();
            return;
        }
        m_delayPress.clear();
        QListView::mousePressEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && std::exchange(m_foldPress, false)) {
            if (!hitsCard(indexAt(event->position().toPoint()), event->position().toPoint())) {
                if (auto* group = qobject_cast<QAbstractButton*>(parentWidget())) {
                    group->setFocus(Qt::MouseFocusReason);
                    group->click();
                }
            }
            event->accept();
            return;
        }
        if (event->button() == Qt::LeftButton && !m_delayPress.isEmpty()) {
            const auto index = indexAt(event->position().toPoint());
            const auto tag = std::exchange(m_delayPress, {});
            if (index.data(GroupModel::OutboundTagRole).toString() == tag &&
                hitsDelay(index, event->position().toPoint()))
                test(index);
            event->accept();
            return;
        }
        QListView::mouseReleaseEvent(event);
    }
    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (hitsDelay(indexAt(event->position().toPoint()), event->position().toPoint()))
            mousePressEvent(event);
        else
            QListView::mouseDoubleClickEvent(event);
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        const auto index = indexAt(event->position().toPoint());
        const auto group = index.data(GroupModel::GroupTagRole).toString();
        const auto outbound = index.data(GroupModel::OutboundTagRole).toString();
        const bool action = !hitsCard(index, event->position().toPoint()) ||
                            (hitsDelay(index, event->position().toPoint())
                                 ? m_controller->canTestDelay(group, outbound)
                                 : m_controller->canSelectOutbound(group, outbound));
        viewport()->setCursor(action ? Qt::PointingHandCursor : Qt::ArrowCursor);
        QListView::mouseMoveEvent(event);
    }
    bool viewportEvent(QEvent* event) override {
        if (event->type() == QEvent::ToolTip) {
            auto* help = static_cast<QHelpEvent*>(event);
            const auto index = indexAt(help->pos());
            if (hitsDelay(index, help->pos())) {
                const auto outbound = index.data(GroupModel::OutboundTagRole).toString();
                const auto error = m_controller->delayError(outbound);
                QToolTip::showText(
                    help->globalPos(),
                    error.isEmpty() ? tr("Test delay for %1 (T)").arg(outbound) : error, this);
                return true;
            }
        }
        return QListView::viewportEvent(event);
    }
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_T && event->modifiers() == Qt::NoModifier) {
            if (!event->isAutoRepeat())
                test(currentIndex());
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter ||
            event->key() == Qt::Key_Space) {
            if (!event->isAutoRepeat())
                choose(currentIndex());
            event->accept();
            return;
        }
        QListView::keyPressEvent(event);
    }

  private:
    bool hitsCard(const QModelIndex& index, const QPoint& point) const {
        return index.isValid() && visualRect(index).adjusted(4, 4, -4, -4).contains(point);
    }
    bool m_foldPress = false;
    bool hitsDelay(const QModelIndex& index, const QPoint& point) const {
        return index.isValid() &&
               delayBadge(visualRect(index), font(), delayText(m_controller, index))
                   .contains(point);
    }
    void test(const QModelIndex& index) {
        if (index.isValid() && !isRowHidden(index.row()))
            m_controller->testDelay(index.data(GroupModel::GroupTagRole).toString(),
                                    index.data(GroupModel::OutboundTagRole).toString());
    }
    QString m_delayPress;
    void choose(const QModelIndex& index) {
        if (!index.isValid() || isRowHidden(index.row()))
            return;
        m_controller->selectOutbound(index.data(GroupModel::GroupTagRole).toString(),
                                     index.data(GroupModel::OutboundTagRole).toString());
    }
    Controller* m_controller;
};

class ProxyGroupSection : public QAbstractButton {
  public:
    ProxyGroupSection(Controller* controller, const QString& tag, QWidget* parent)
        : QAbstractButton(parent), m_controller(controller), m_tag(tag) {
        setObjectName("proxyGroup");
        setProperty("groupTag", tag);
        setCheckable(true);
        setChecked(true);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
        setAccessibleName(tag);
        setAccessibleDescription(tr("Expanded. Press Space to fold this group."));
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(16, 14, 16, 12);
        layout->setSpacing(6);
        m_name = new QLabel(tag);
        m_name->setObjectName("groupName");
        m_name->setTextFormat(Qt::PlainText);
        m_name->setWordWrap(true);
        m_name->setContentsMargins(0, 0, 24, 0);
        m_name->setAttribute(Qt::WA_TransparentForMouseEvents);
        auto font = m_name->font();
        font.setBold(true);
        m_name->setFont(font);
        layout->addWidget(m_name);
        m_current = new QLabel;
        m_current->setObjectName("groupCurrent");
        m_current->setTextFormat(Qt::PlainText);
        m_current->setWordWrap(true);
        m_current->setAttribute(Qt::WA_TransparentForMouseEvents);
        layout->addWidget(m_current);
        cards = new ProxyCardView(controller, this);
        cards->setProperty("groupTag", tag);
        cards->setAccessibleName(tr("Outbounds in %1").arg(tag));
        layout->addWidget(cards);
        connect(this, &QAbstractButton::toggled, this, [this](bool open) {
            setAccessibleDescription(open ? tr("Expanded. Press Space to fold this group.")
                                          : tr("Collapsed. Press Space to unfold this group."));
            cards->setVisible(open);
            if (open)
                cards->arrange();
        });
    }

    void saveFocus() {
        m_focusedMember = cards->currentIndex().data(GroupModel::OutboundTagRole).toString();
        m_hadFocus = cards->hasFocus();
    }
    void synchronize() {
        auto* model = m_controller->groupModel();
        cards->setRootIndex(model->findIndex(m_tag));
        cards->setCurrentIndex(model->findIndex(m_tag, m_focusedMember));
        // A removed member must not turn a group row into a card selection.
        if (m_focusedMember.isEmpty() || cards->currentIndex().parent() != cards->rootIndex())
            cards->setCurrentIndex({});
        if (m_hadFocus && cards->isVisible())
            cards->setFocus(Qt::OtherFocusReason);
    }
    bool refresh(const QString& keyword) {
        const auto* group = m_controller->groupModel()->find(m_tag);
        if (!group)
            return false;
        const auto kind = group->type == "selector"      ? tr("Selector")
                          : group->type == "urltest"     ? tr("Automatic")
                          : group->type == "loadbalance" ? tr("Load balance")
                                                         : group->type;
        const auto current = group->type == "loadbalance" ? tr("Per connection")
                             : group->selected.isEmpty()  ? tr("No active outbound")
                                                          : tr("Using %1").arg(group->selected);
        m_current->setText(
            tr("%1 · %2 outbounds · %3").arg(kind).arg(group->members.size()).arg(current));
        const bool all = keyword.isEmpty() || m_tag.contains(keyword, Qt::CaseInsensitive);
        int visible = 0;
        for (int row = 0; row < group->members.size(); ++row) {
            const auto& member = group->members[row];
            const bool match = all || member.tag.contains(keyword, Qt::CaseInsensitive) ||
                               member.type.contains(keyword, Qt::CaseInsensitive);
            cards->setRowHidden(row, !match);
            visible += match;
        }
        cards->setEnabled(m_controller->groupsReady());
        cards->setCursor(group->selectable && m_controller->groupsReady() && !m_controller->busy()
                             ? Qt::PointingHandCursor
                             : Qt::ArrowCursor);
        cards->arrange();
        cards->viewport()->update();
        setVisible(all || visible > 0);
        return all || visible > 0;
    }

    ProxyCardView* cards;

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setBrush(palette().brush(QPalette::Button));
        painter.setPen(QPen(palette().color(hasFocus() ? QPalette::Highlight : QPalette::Mid),
                            hasFocus() ? 2 : 1));
        painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 10, 10);
        Icons::paint(painter, isChecked() ? Icons::ChevronDown : Icons::ChevronRight,
                     QRectF(width() - 35, 14, 20, 20), palette().color(QPalette::Text));
    }
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            if (!event->isAutoRepeat())
                click();
            event->accept();
        } else
            QAbstractButton::keyPressEvent(event);
    }

  private:
    Controller* m_controller;
    QString m_tag, m_focusedMember;
    bool m_hadFocus = false;
    QLabel *m_name, *m_current;
};
} // namespace

struct ProxiesPage::Ui {
    Controller* controller;
    QWidget* content;
    QScrollArea* scroll;
    QVBoxLayout* groupsLayout;
    QLineEdit* search;
    SymbolButton* refreshButton;
    QLabel* state;
    QMap<QString, ProxyGroupSection*> groups;
    int scrollPosition = 0;
    bool snapshotPending = false;

    void synchronize() {
        snapshotPending = false;
        auto* model = controller->groupModel();
        QSet<QString> retained;
        for (int row = 0; row < model->rowCount(); ++row) {
            const auto tag = model->index(row, 0).data(GroupModel::GroupTagRole).toString();
            retained.insert(tag);
            auto* section = groups.value(tag);
            if (!section) {
                section = new ProxyGroupSection(controller, tag, content);
                groups.insert(tag, section);
            }
            groupsLayout->removeWidget(section);
            groupsLayout->insertWidget(row, section);
            section->synchronize();
        }
        for (auto it = groups.begin(); it != groups.end();) {
            if (retained.contains(it.key())) {
                ++it;
            } else {
                delete it.value();
                it = groups.erase(it);
            }
        }
        refresh();
        groupsLayout->activate();
        scroll->verticalScrollBar()->setValue(scrollPosition);
    }

    void refresh() {
        if (snapshotPending)
            return;
        const auto keyword = search->text().trimmed();
        int visible = 0;
        for (auto* section : groups)
            visible += section->refresh(keyword);
        refreshButton->setEnabled(controller->online() &&
                                  controller->state() == Controller::Running &&
                                  !controller->busy());
        if (!controller->online() || controller->state() != Controller::Running)
            state->setText(ProxiesPage::tr("Connect a profile on Home to choose a proxy."));
        else if (!controller->groupsError().isEmpty())
            state->setText(
                ProxiesPage::tr("Unable to load proxies: %1").arg(controller->groupsError()));
        else if (!controller->groupsReady())
            state->setText(ProxiesPage::tr("Loading proxies…"));
        else if (groups.isEmpty())
            state->setText(ProxiesPage::tr("This profile has no outbound groups. Use a profile "
                                           "with a selector to switch proxies here."));
        else if (!visible)
            state->setText(ProxiesPage::tr("No proxies match your search."));
        else
            state->clear();
        state->setVisible(!state->text().isEmpty());
    }
};

ProxiesPage::ProxiesPage(Controller* controller, QWidget* parent)
    : QWidget(parent), m_ui(std::make_unique<Ui>()) {
    auto& ui = *m_ui;
    ui.controller = controller;
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(28, 28, 28, 24);
    layout->setSpacing(14);
    auto* heading = new QHBoxLayout;
    auto* title = new QLabel(tr("Proxies"));
    auto font = title->font();
    font.setPointSize(24);
    font.setBold(true);
    title->setFont(font);
    heading->addWidget(title);
    heading->addStretch();
    ui.refreshButton = new SymbolButton(SymbolButton::Refresh);
    ui.refreshButton->setObjectName("refreshGroupsButton");
    ui.refreshButton->setAccessibleName(tr("Refresh proxies"));
    ui.refreshButton->setToolTip(ui.refreshButton->accessibleName());
    heading->addWidget(ui.refreshButton);
    layout->addLayout(heading);
    ui.search = new QLineEdit;
    ui.search->setObjectName("proxySearch");
    ui.search->setPlaceholderText(tr("Search groups or proxies"));
    ui.search->setAccessibleName(tr("Search groups or proxies"));
    ui.search->setClearButtonEnabled(true);
    layout->addWidget(ui.search);
    ui.state = new QLabel;
    ui.state->setObjectName("proxiesState");
    ui.state->setTextFormat(Qt::PlainText);
    ui.state->setWordWrap(true);
    layout->addWidget(ui.state);
    ui.scroll = new QScrollArea;
    ui.scroll->setObjectName("proxyScroll");
    ui.scroll->setWidgetResizable(true);
    ui.scroll->setFrameShape(QFrame::NoFrame);
    ui.scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ui.content = new QWidget;
    ui.groupsLayout = new QVBoxLayout(ui.content);
    ui.groupsLayout->setContentsMargins(0, 0, 6, 0);
    ui.groupsLayout->setSpacing(12);
    ui.groupsLayout->addStretch();
    ui.scroll->setWidget(ui.content);
    layout->addWidget(ui.scroll, 1);
    connect(ui.refreshButton, &QToolButton::clicked, controller, &Controller::refreshGroups);
    connect(ui.search, &QLineEdit::textChanged, this, [this] { m_ui->refresh(); });
    connect(controller->groupModel(), &QAbstractItemModel::modelAboutToBeReset, this, [this] {
        if (m_ui->snapshotPending)
            return;
        m_ui->snapshotPending = true;
        m_ui->scrollPosition = m_ui->scroll->verticalScrollBar()->value();
        for (auto* section : m_ui->groups)
            section->saveFocus();
    });
    // Run after the individual views have reset their root/current indexes.
    connect(
        controller->groupModel(), &QAbstractItemModel::modelReset, this,
        [this] { m_ui->synchronize(); }, Qt::QueuedConnection);
    connect(controller, &Controller::changed, this, [this] { m_ui->refresh(); });
    ui.synchronize();
}

ProxiesPage::~ProxiesPage() = default;
