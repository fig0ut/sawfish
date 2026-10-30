#include "logspage.h"
#include "app/controller.h"
#include "models/logmodel.h"
#include "widgets/controls.h"
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QSpinBox>
#include <QStyledItemDelegate>
#include <QTextLayout>
#include <QVBoxLayout>

namespace {
qreal arrangeText(QTextLayout& layout, qreal width) {
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    layout.beginLayout();
    qreal height = 0;
    while (true) {
        auto line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(qMax(1., width));
        line.setPosition(QPointF(0, height));
        height += line.height();
    }
    layout.endLayout();
    return height;
}

class LogDelegate : public QStyledItemDelegate {
  public:
    explicit LogDelegate(QListView* view) : QStyledItemDelegate(view), m_view(view) {}
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QTextLayout layout(index.data().toString().replace('\n', QChar(0x2028)), option.font);
        return {0, qCeil(arrangeText(layout, m_view->viewport()->width() - 12)) + 8};
    }
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        auto opt = option;
        initStyleOption(&opt, index);
        const auto text = opt.text.replace('\n', QChar(0x2028));
        opt.text.clear();
        m_view->style()->drawControl(QStyle::CE_ItemViewItem, &opt, painter, m_view);
        const bool selected = opt.state & QStyle::State_Selected;
        const auto foreground =
            opt.palette.color(selected ? QPalette::HighlightedText : QPalette::Text);
        const bool dark = opt.palette.color(QPalette::Base).lightness() < 128;
        const int level = index.data(LogModel::LevelRole).toInt();
        const QColor color = selected     ? foreground
                             : level <= 2 ? QColor(dark ? "#ff8a80" : "#b42318")
                             : level == 3 ? QColor(dark ? "#ffd166" : "#926000")
                             : level == 4 ? QColor(dark ? "#73d0ff" : "#006c91")
                             : level == 5 ? QColor(dark ? "#c4a7ff" : "#6544a5")
                                          : QColor(dark ? "#aab2c0" : "#59636f");
        QTextLayout layout(text, opt.font);
        QTextLayout::FormatRange format;
        format.start = text.startsWith("[APP] ") ? 6 : 0;
        format.length = text.indexOf(']', format.start) - format.start + 1;
        format.format.setForeground(color);
        layout.setFormats({format});
        arrangeText(layout, opt.rect.width() - 12);
        painter->save();
        painter->setClipRect(opt.rect);
        painter->setPen(foreground);
        layout.draw(painter, opt.rect.topLeft() + QPoint(6, 4));
        painter->restore();
    }

  private:
    QListView* m_view;
};
} // namespace

LogPage::LogPage(Controller* controller, QWidget* parent) : QWidget(parent) {
    auto* model = controller->logModel();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(28, 28, 28, 24);
    layout->setSpacing(14);
    auto* title = new QLabel(tr("Logs"));
    auto font = title->font();
    font.setPointSize(24);
    font.setBold(true);
    title->setFont(font);
    layout->addWidget(title);
    auto* filter = new LogFilter(this);
    filter->setSourceModel(model);
    auto* toolbar = new QHBoxLayout;
    auto* level = new ThemedComboBox;
    level->setObjectName("logLevelSelector");
    level->addItem(tr("All levels"), 6);
    level->addItem(tr("Debug and above"), 5);
    level->addItem(tr("Info and above"), 4);
    level->addItem(tr("Warnings and errors"), 3);
    level->addItem(tr("Errors"), 2);
    auto* search = new QLineEdit;
    search->setPlaceholderText(tr("Filter messages"));
    search->setClearButtonEnabled(true);
    auto* pause = new SymbolButton(SymbolButton::Pause);
    pause->setObjectName("pauseLogsButton");
    pause->setAccessibleName(tr("Pause logs"));
    pause->setToolTip(pause->accessibleName());
    pause->setCheckable(true);
    auto* copy = new SymbolButton(SymbolButton::Copy);
    copy->setObjectName("copyLogsButton");
    copy->setAccessibleName(tr("Copy filtered logs"));
    copy->setToolTip(copy->accessibleName());
    auto* clear = new SymbolButton(SymbolButton::Trash);
    clear->setObjectName("clearLogsButton");
    clear->setAccessibleName(tr("Clear logs"));
    clear->setToolTip(clear->accessibleName());
    auto* settings = new SymbolButton(SymbolButton::Settings);
    settings->setObjectName("logSettingsButton");
    settings->setAccessibleName(tr("Log settings"));
    settings->setToolTip(settings->accessibleName());
    toolbar->addWidget(level);
    toolbar->addWidget(search, 1);
    toolbar->addWidget(pause);
    toolbar->addWidget(copy);
    toolbar->addWidget(clear);
    toolbar->addWidget(settings);
    layout->addLayout(toolbar);
    auto* view = new QListView;
    view->setObjectName("logsList");
    view->setModel(filter);
    view->setItemDelegate(new LogDelegate(view));
    view->setWordWrap(true);
    view->setResizeMode(QListView::Adjust);
    view->setUniformItemSizes(false);
    view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    layout->addWidget(view, 1);
    connect(search, &QLineEdit::textChanged, filter, &LogFilter::setFilterFixedString);
    connect(level, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [=] { filter->setMaximumLevel(level->currentData().toInt()); });
    connect(pause, &QToolButton::toggled, this, [=](bool paused) {
        model->setPaused(paused);
        pause->setSymbol(paused ? SymbolButton::Play : SymbolButton::Pause);
        pause->setAccessibleName(paused ? tr("Resume logs") : tr("Pause logs"));
        pause->setToolTip(pause->accessibleName());
    });
    connect(clear, &QToolButton::clicked, model, &LogModel::clear);
    connect(copy, &QToolButton::clicked, this, [=] {
        QStringList lines;
        for (int row = 0; row < filter->rowCount(); ++row)
            lines.append(filter->index(row, 0).data().toString());
        QApplication::clipboard()->setText(lines.join('\n'));
    });
    connect(settings, &QToolButton::clicked, this, [=] {
        auto* dialog = new QDialog(this);
        dialog->setObjectName("logSettingsDialog");
        dialog->setWindowTitle(tr("Log settings"));
        dialog->setWindowModality(Qt::WindowModal);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        auto* form = new QFormLayout(dialog);
        auto* limit = new QSpinBox;
        limit->setObjectName("logRetentionLimit");
        limit->setRange(LogModel::MinimumRetention, LogModel::MaximumRetention);
        limit->setSingleStep(100);
        limit->setGroupSeparatorShown(true);
        limit->setValue(model->retentionLimit());
        form->addRow(tr("Maximum retained lines"), limit);
        auto* description = new QLabel(tr(
            "Oldest entries are removed first. Lowering the limit "
            "also trims existing logs. A 2 MiB text limit still applies. Logs are kept in memory; "
            "this setting is saved for the next launch."));
        description->setWordWrap(true);
        form->addRow(description);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
        form->addRow(buttons);
        connect(buttons, &QDialogButtonBox::accepted, dialog, [=] {
            controller->saveLogRetention(limit->value());
            dialog->accept();
        });
        connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
        dialog->resize(420, 200);
        dialog->open();
    });
    auto follow = std::make_shared<bool>(true);
    connect(filter, &QAbstractItemModel::rowsAboutToBeInserted, view, [=] {
        *follow = view->verticalScrollBar()->value() >= view->verticalScrollBar()->maximum() - 4;
    });
    connect(filter, &QAbstractItemModel::rowsInserted, view, [=] {
        if (*follow)
            view->scrollToBottom();
    });
}
