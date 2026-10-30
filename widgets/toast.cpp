#include "toast.h"
#include "controls.h"
#include <QAccessible>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>

Toast::Toast(QWidget* parent) : QWidget(parent), m_slide(this, "pos", this) {
    setObjectName("appToast");
    setAttribute(Qt::WA_StyledBackground, false);
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(18, 12, 10, 12);
    row->setSpacing(12);
    m_label = new QLabel;
    m_label->setObjectName("toastMessage");
    m_label->setTextFormat(Qt::PlainText);
    m_label->setWordWrap(true);
    m_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    row->addWidget(m_label, 1);
    auto* dismiss = new SymbolButton(SymbolButton::Close);
    dismiss->setObjectName("dismissToastButton");
    dismiss->setToolTip(tr("Dismiss notification"));
    dismiss->setAccessibleName(dismiss->toolTip());
    row->addWidget(dismiss, 0, Qt::AlignTop);
    connect(dismiss, &QToolButton::clicked, this, &QWidget::hide);
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, &QWidget::hide);
    m_slide.setDuration(180);
    m_slide.setEasingCurve(QEasingCurve::OutCubic);
    parent->installEventFilter(this);
    hide();
}

void Toast::present(const QString& message, bool error) {
    m_slide.stop();
    m_timeout.stop();
    if (message.isEmpty()) {
        hide();
        return;
    }
    const bool entering = isHidden();
    m_error = error;
    // Full diagnostics stay in Logs; an unusually long error must not cover the page.
    const auto summary = message.simplified();
    m_label->setText(summary.size() > 400 ? summary.left(400) + tr("… See Logs for details.")
                                          : summary);
    setAccessibleName(m_label->text());
    position();
    show();
    raise();
    update();
    if (isVisible()) {
        m_timeout.start(error ? 8000 : 4500);
        if (entering) {
            m_slide.setStartValue(QPoint(x(), -height()));
            m_slide.setEndValue(pos());
            m_slide.start();
        }
        QAccessibleEvent announcement(this, QAccessible::Alert);
        QAccessible::updateAccessibility(&announcement);
    }
}

void Toast::position() {
    m_slide.stop();
    const int available = parentWidget()->width() - 32;
    const int width = qMax(1, qMin(560, available));
    const int height = qMax(56, layout()->totalHeightForWidth(width));
    setGeometry((parentWidget()->width() - width) / 2, 16, width, height);
}

bool Toast::eventFilter(QObject* object, QEvent* event) {
    if (object == parentWidget() && event->type() == QEvent::Resize)
        position();
    return QWidget::eventFilter(object, event);
}

void Toast::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const auto accent =
        m_error
            ? QColor(palette().color(QPalette::Window).lightness() < 128 ? "#ff6b6b" : "#c52b35")
            : palette().color(QPalette::Highlight);
    p.setBrush(palette().brush(QPalette::Light));
    p.setPen(QPen(accent, 1.5));
    p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 10, 10);
    p.setPen(QPen(accent, 3, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(8, 16), QPointF(8, height() - 16));
}

void Toast::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    m_timeout.start(m_error ? 8000 : 4500);
}

void Toast::hideEvent(QHideEvent* event) {
    m_timeout.stop();
    m_slide.stop();
    QWidget::hideEvent(event);
}
