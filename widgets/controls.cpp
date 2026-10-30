#include "controls.h"
#include "icons.h"
#include <QPainter>
#include <QStyleOptionComboBox>

SymbolButton::SymbolButton(Symbol symbol, QWidget* parent) : QToolButton(parent), m_symbol(symbol) {
    setFixedSize(32, 32);
    setIconSize(QSize(22, 22));
    setCursor(Qt::PointingHandCursor);
    setStyleSheet("QToolButton::menu-indicator { image: none; width: 0; height: 0; }");
}
void SymbolButton::setSymbol(Symbol symbol) {
    m_symbol = symbol;
    update();
}
void SymbolButton::paintEvent(QPaintEvent* event) {
    QToolButton::paintEvent(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const auto color = palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled,
                                       (isDown() || isChecked()) ? QPalette::HighlightedText
                                                                 : QPalette::ButtonText);
    const QSize size = iconSize();
    const QRectF bounds((width() - size.width()) / 2., (height() - size.height()) / 2.,
                        size.width(), size.height());
    Icons::paint(p, static_cast<Icons::Name>(m_symbol), bounds, color);
}
ConnectionSwitch::ConnectionSwitch(QWidget* parent) : QAbstractButton(parent) {
    setCheckable(true);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
    setAccessibleName(tr("Connection"));
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}
void ConnectionSwitch::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const auto group = isEnabled() ? QPalette::Active : QPalette::Disabled;
    const auto background = palette().color(group, QPalette::Window);
    const auto foreground = palette().color(group, QPalette::WindowText);
    const auto blend = [&](qreal amount) {
        return QColor::fromRgbF(background.redF() * (1 - amount) + foreground.redF() * amount,
                                background.greenF() * (1 - amount) + foreground.greenF() * amount,
                                background.blueF() * (1 - amount) + foreground.blueF() * amount);
    };
    const auto track = isChecked() ? palette().color(group, QPalette::Highlight)
                                   : blend(isEnabled() ? 0.28 : 0.16);
    const auto thumb =
        palette().color(group, isChecked() ? QPalette::HighlightedText : QPalette::Button);
    const auto glyph =
        palette().color(group, isChecked() ? QPalette::Highlight : QPalette::ButtonText);
    const QRectF trackRect(4, 5, width() - 8, height() - 10);
    p.setPen(isChecked() ? QPen(Qt::NoPen) : QPen(blend(isEnabled() ? 0.52 : 0.3), 1));
    p.setBrush(track);
    p.drawRoundedRect(trackRect, trackRect.height() / 2, trackRect.height() / 2);
    p.setPen(Qt::NoPen);
    const qreal diameter = trackRect.height() - 8;
    const QRectF knob(isChecked() ? trackRect.right() - diameter - 4 : trackRect.left() + 4,
                      trackRect.top() + 4, diameter, diameter);
    p.setBrush(thumb);
    p.drawEllipse(knob);
    const auto center = knob.center();
    Icons::paint(p, isChecked() ? Icons::Pause : Icons::Play,
                 QRectF(center - QPointF(10, 10), QSizeF(20, 20)), glyph);
    if (hasFocus()) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(palette().color(group, QPalette::Highlight), 1, Qt::DashLine));
        p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 24, 24);
    }
}

RoundedMenu::RoundedMenu(QWidget* parent) : QMenu(parent) {
    setWindowFlag(Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setStyleSheet(R"(
        QMenu { color: palette(window-text); background: palette(window);
            border: 1px solid palette(mid); border-radius: 10px; padding: 5px; }
        QMenu::item { padding: 7px 20px; border-radius: 5px; }
        QMenu::item:selected { color: palette(highlighted-text); background: palette(highlight); }
        QMenu::item:disabled { color: palette(mid); }
        QMenu::separator { height: 1px; background: palette(mid); margin: 4px 8px; }
    )");
}

ThemedComboBox::ThemedComboBox(QWidget* parent) : QComboBox(parent) {
    setStyleSheet(R"(
        QComboBox { padding-right: 30px; }
        QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: top right;
            width: 28px; border: none; background: transparent; }
        QComboBox::down-arrow { image: none; }
    )");
}

void ThemedComboBox::paintEvent(QPaintEvent* event) {
    QComboBox::paintEvent(event);
    QStyleOptionComboBox option;
    initStyleOption(&option);
    const auto arrow =
        style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxArrow, this);
    const auto center = QRectF(arrow).center();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    Icons::paint(
        p, Icons::ChevronDown, QRectF(center - QPointF(10, 10), QSizeF(20, 20)),
        palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled, QPalette::Text));
}
