#include "dashboard.h"
#include "app/controller.h"
#include "controls.h"
#include "models/format.h"
#include <QDateTime>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QVBoxLayout>
#include <cmath>

namespace {
constexpr int Gap = 12, RowHeight = 96, Columns = 4, HistorySeconds = 60;
struct Sample {
    qint64 time;
    double upload, download, connections, cpu, memory;
};
} // namespace

struct Dashboard::Ui {
    Controller* controller;
    QScrollArea* scroll;
    QWidget* canvas;
    QLabel* empty;
    QList<DashboardCard*> cards;
    QList<Sample> samples;
    TrafficSample current;
    bool editing = false;
};

class DashboardCard : public QWidget {
  public:
    Dashboard* dashboard;
    Dashboard::Kind kind;
    QRect cells;
    SymbolButton* remove;
    bool dragging = false, resizing = false;
    QPoint origin;
    QRect original;
    DashboardCard(Dashboard* owner, Dashboard::Kind type, QRect location)
        : QWidget(owner->m_ui->canvas), dashboard(owner), kind(type), cells(location) {
        setObjectName("dashboardCard");
        setProperty("widgetKind", int(kind));
        setAccessibleName(Dashboard::title(kind));
        if (kind == Dashboard::Cpu)
            setAccessibleDescription(tr("Rostra process CPU usage. 100% is one CPU core. "
                                        "Available for the managed core on Linux."));
        else if (kind == Dashboard::Memory)
            setAccessibleDescription(tr("Memory usage reported by Rostra."));
        setToolTip(accessibleDescription());
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        remove = new SymbolButton(SymbolButton::Close, this);
        remove->setObjectName("removeWidgetButton");
        remove->setAccessibleName(tr("Remove %1 widget").arg(Dashboard::title(kind)));
        remove->setToolTip(remove->accessibleName());
        connect(remove, &QToolButton::clicked, owner, [owner, type] { owner->removeWidget(type); });
        remove->setVisible(owner->editing());
        show();
    }

  protected:
    void resizeEvent(QResizeEvent*) override {
        remove->move(width() - 42, 8);
    }
    void paintEvent(QPaintEvent*) override {
        auto& ui = *dashboard->m_ui;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const auto accent = palette().color(QPalette::Highlight);
        const auto text = palette().color(QPalette::Text);
        p.setBrush(palette().brush(QPalette::Base));
        p.setPen(
            QPen(palette().color(ui.editing || hasFocus() ? QPalette::Highlight : QPalette::Mid),
                 ui.editing ? 1.5 : 1, ui.editing ? Qt::DashLine : Qt::SolidLine));
        p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 12, 12);
        p.setPen(text);
        auto heading = font();
        heading.setBold(true);
        p.setFont(heading);
        p.drawText(QRect(16, 12, width() - (ui.editing ? 68 : 32), 24), Qt::AlignVCenter,
                   Dashboard::title(kind));
        p.setFont(font());
        const bool dark = palette().color(QPalette::Base).lightness() < 128;
        const auto secondary = QColor::fromHsv((qMax(0, accent.hsvHue()) + 140) % 360,
                                               qMax(120, accent.hsvSaturation()), dark ? 225 : 170);
        if (kind == Dashboard::TotalTraffic) {
            const auto total = double(ui.current.uploadTotal) + double(ui.current.downloadTotal);
            const double ratio = total > 0 ? ui.current.uploadTotal / total : 0.5;
            const int diameter = qMax(48, qMin(width() - 48, height() - 110));
            const QRectF ring((width() - diameter) / 2., 48, diameter, diameter);
            p.setPen(QPen(secondary, 12));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(ring);
            if (ratio > 0) {
                // Square caps extend both ends past the true ratio boundaries.
                p.setPen(QPen(accent, 12, Qt::SolidLine, Qt::FlatCap));
                p.drawArc(ring, 90 * 16, -qRound(ratio * 360 * 16));
            }
            p.setPen(text);
            auto valueFont = font();
            valueFont.setBold(true);
            valueFont.setPointSize(12);
            p.setFont(valueFont);
            p.drawText(ring, Qt::AlignCenter, formatBytes(total));
            p.setFont(font());
            p.setPen(accent);
            p.setBrush(accent);
            p.drawEllipse(QPointF(19, height() - 40), 4, 4);
            p.setPen(text);
            p.drawText(QRect(30, height() - 51, width() - 42, 22), Qt::AlignVCenter,
                       tr("Upload  %1").arg(formatBytes(ui.current.uploadTotal)));
            p.setPen(secondary);
            p.setBrush(secondary);
            p.drawEllipse(QPointF(19, height() - 19), 4, 4);
            p.setPen(text);
            p.drawText(QRect(30, height() - 30, width() - 42, 22), Qt::AlignVCenter,
                       tr("Download  %1").arg(formatBytes(ui.current.downloadTotal)));
        } else {
            auto valueOf = [this](const Sample& s) {
                return kind == Dashboard::Upload     ? s.upload
                       : kind == Dashboard::Download ? s.download
                       : kind == Dashboard::Traffic  ? s.upload
                       : kind == Dashboard::Cpu      ? s.cpu
                       : kind == Dashboard::Memory   ? s.memory
                                                     : s.connections;
            };
            auto formatted = [this](double value) {
                if (kind == Dashboard::Connections)
                    return QString::number(qRound64(value));
                if (kind == Dashboard::Cpu)
                    return QString::number(value, 'f', 1) + "%";
                return formatBytes(value) + (kind == Dashboard::Memory ? "" : "/s");
            };
            const auto value = ui.samples.isEmpty() ? 0 : valueOf(ui.samples.last());
            auto valueFont = font();
            valueFont.setPointSize(22);
            valueFont.setBold(true);
            p.setFont(valueFont);
            if (kind == Dashboard::Traffic) {
                valueFont.setPointSize(13);
                p.setFont(valueFont);
                p.setPen(accent);
                p.drawText(QRect(16, 44, width() - 32, 24), Qt::AlignVCenter,
                           tr("↑ %1").arg(formatted(value)));
                p.setPen(secondary);
                p.drawText(QRect(16, 70, width() - 32, 24), Qt::AlignVCenter,
                           tr("↓ %1").arg(
                               formatted(ui.samples.isEmpty() ? 0 : ui.samples.last().download)));
            } else {
                const bool unavailable = kind == Dashboard::Cpu &&
                                         ui.controller->state() == Controller::Running &&
                                         !ui.current.cpuAvailable;
                p.drawText(QRect(16, 44, width() - 32, 40), Qt::AlignVCenter,
                           unavailable ? tr("Unavailable") : formatted(value));
            }
            p.setFont(font());
            double peak = 0;
            for (const auto& s : ui.samples) {
                peak = qMax(peak, valueOf(s));
                if (kind == Dashboard::Traffic)
                    peak = qMax(peak, s.download);
            }
            const double maximum = qMax(peak, kind == Dashboard::Connections ? 1.
                                              : kind == Dashboard::Cpu       ? 100.
                                                                             : 1024.);
            const QRectF plot(16, 108, width() - 32, height() - 138);
            p.setPen(palette().color(QPalette::Mid));
            for (int row = 0; row < 3; ++row) {
                const qreal y = plot.top() + row * plot.height() / 2;
                p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            }
            p.setPen(text);
            if (kind != Dashboard::Traffic)
                p.drawText(QRectF(16, 85, width() - 32, 20), Qt::AlignLeft,
                           tr("Peak %1").arg(formatted(peak)));
            p.drawText(QRect(16, height() - 24, width() - 32, 18), Qt::AlignLeft, tr("60 s ago"));
            p.drawText(QRect(16, height() - 24, width() - 32, 18), Qt::AlignRight, tr("Now"));
            const auto now =
                ui.samples.isEmpty() ? QDateTime::currentMSecsSinceEpoch() : ui.samples.last().time;
            auto drawSeries = [&](auto readValue, const QColor& color) {
                QPainterPath line;
                // Prefill the unobserved part of the window with zero, so even
                // the first render spans the whole chart instead of growing in.
                const bool prefilled =
                    ui.samples.isEmpty() || ui.samples.first().time > now - 60000;
                if (prefilled)
                    line.moveTo(plot.bottomLeft());
                for (const auto& s : ui.samples) {
                    const QPointF point(plot.right() - (now - s.time) * plot.width() / 60000.,
                                        plot.bottom() - readValue(s) / maximum * plot.height());
                    if (prefilled && line.elementCount() == 1)
                        line.lineTo(point.x(), plot.bottom());
                    if (line.elementCount() == 0)
                        line.moveTo(point);
                    else
                        line.lineTo(point);
                }
                if (ui.samples.isEmpty())
                    line.lineTo(plot.bottomRight());
                p.setClipRect(plot.adjusted(-2, -2, 2, 2));
                auto fill = line;
                fill.lineTo(line.currentPosition().x(), plot.bottom());
                fill.lineTo(line.elementAt(0).x, plot.bottom());
                fill.closeSubpath();
                auto tint = color;
                tint.setAlpha(36);
                p.fillPath(fill, tint);
                p.setPen(QPen(color, 2));
                p.setBrush(Qt::NoBrush);
                p.drawPath(line);
                p.setBrush(color);
                p.drawEllipse(line.currentPosition(), 2.5, 2.5);
            };
            drawSeries(valueOf, accent);
            if (kind == Dashboard::Traffic)
                drawSeries([](const Sample& s) { return s.download; }, secondary);
            p.setClipping(false);
        }
        if (ui.editing) {
            p.setPen(QPen(accent, 2));
            for (int offset : {8, 13, 18})
                p.drawLine(width() - offset, height() - 6, width() - 6, height() - offset);
        }
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (!dashboard->editing() || event->button() != Qt::LeftButton)
            return;
        setFocus();
        resizing = event->position().x() >= width() - 24 && event->position().y() >= height() - 24;
        dragging = !resizing;
        origin = event->globalPosition().toPoint();
        original = geometry();
        raise();
        event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (!dashboard->editing())
            return;
        if (!dragging && !resizing) {
            setCursor(event->position().x() >= width() - 24 &&
                              event->position().y() >= height() - 24
                          ? Qt::SizeFDiagCursor
                          : Qt::OpenHandCursor);
            return;
        }
        const auto delta = event->globalPosition().toPoint() - origin;
        auto geometry = original;
        if (resizing)
            geometry.setSize(QSize(qMax(200, original.width() + delta.x()),
                                   qMax(204, original.height() + delta.y())));
        else
            geometry.moveTopLeft(original.topLeft() + delta);
        geometry.moveLeft(
            qBound(0, geometry.x(), qMax(0, parentWidget()->width() - geometry.width())));
        geometry.moveTop(qMax(0, geometry.y()));
        setGeometry(geometry);
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton || (!dragging && !resizing))
            return;
        dragging = resizing = false;
        dashboard->commit(this, geometry());
    }
    void keyPressEvent(QKeyEvent* event) override {
        if (!dashboard->editing())
            return QWidget::keyPressEvent(event);
        if (event->key() == Qt::Key_Delete) {
            dashboard->removeWidget(kind);
            return;
        }
        const int dx = (event->key() == Qt::Key_Right) - (event->key() == Qt::Key_Left);
        const int dy = (event->key() == Qt::Key_Down) - (event->key() == Qt::Key_Up);
        if (!dx && !dy)
            return QWidget::keyPressEvent(event);
        auto target = geometry();
        const int stepX = (parentWidget()->width() + Gap) / Columns;
        if (event->modifiers() & Qt::ShiftModifier)
            target.setSize(target.size() + QSize(dx * stepX, dy * (RowHeight + Gap)));
        else
            target.translate(dx * stepX, dy * (RowHeight + Gap));
        dashboard->commit(this, target);
    }
};

QString Dashboard::title(Kind kind) {
    switch (kind) {
    case Upload:
        return tr("Upload speed");
    case Download:
        return tr("Download speed");
    case Connections:
        return tr("Connections");
    case TotalTraffic:
        return tr("Total traffic");
    case Cpu:
        return tr("Core CPU usage");
    case Memory:
        return tr("Core memory usage");
    case Traffic:
        return tr("Traffic speed");
    }
    return {};
}
Dashboard::Dashboard(Controller* controller, QWidget* parent)
    : QWidget(parent), m_ui(std::make_unique<Ui>()) {
    auto& ui = *m_ui;
    ui.controller = controller;
    setObjectName("homeDashboard");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    ui.scroll = new QScrollArea;
    ui.scroll->setWidgetResizable(true);
    ui.scroll->setFrameShape(QFrame::NoFrame);
    ui.scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ui.canvas = new QWidget;
    ui.canvas->setMinimumWidth(400);
    ui.scroll->setWidget(ui.canvas);
    ui.scroll->viewport()->installEventFilter(this);
    layout->addWidget(ui.scroll);
    ui.empty = new QLabel(tr("No widgets. Choose Edit widgets, then + to add one."), ui.canvas);
    ui.empty->setWordWrap(true);
    ui.empty->setAlignment(Qt::AlignCenter);
    if (controller->hasDashboardLayout()) {
        for (const auto& item : controller->dashboardLayout()) {
            const auto entry = item.toMap();
            const int kind = entry.value("kind", -1).toInt();
            if (kind < Upload || kind > Traffic || contains(Kind(kind)))
                continue;
            const int span = qBound(2, entry.value("width", 2).toInt(), 4);
            ui.cards.append(
                new DashboardCard(this, Kind(kind),
                                  QRect(qBound(0, entry.value("x").toInt(), Columns - span),
                                        qBound(0, entry.value("y").toInt(), 64), span,
                                        qBound(2, entry.value("height", 2).toInt(), 4))));
        }
    } else {
        for (int kind = Upload; kind <= TotalTraffic; ++kind)
            ui.cards.append(
                new DashboardCard(this, Kind(kind), QRect(kind % 2 * 2, kind / 2 * 2, 2, 2)));
    }
    connect(controller, &Controller::trafficChanged, this, [this] {
        auto& ui = *m_ui;
        const auto next = ui.controller->trafficSample();
        if (ui.controller->state() != Controller::Running || ui.controller->shuttingDown() ||
            next.uploadTotal < ui.current.uploadTotal ||
            next.downloadTotal < ui.current.downloadTotal)
            ui.samples.clear();
        ui.current = next;
        if (ui.controller->state() == Controller::Running && !ui.controller->shuttingDown()) {
            const auto now = QDateTime::currentMSecsSinceEpoch();
            ui.samples.append({now, ui.controller->uploadRate(), ui.controller->downloadRate(),
                               double(next.connections), next.cpuPercent, double(next.memory)});
            while (ui.samples.size() > 120 ||
                   (ui.samples.size() > 1 && ui.samples[1].time <= now - HistorySeconds * 1000))
                ui.samples.removeFirst();
        }
        for (auto* card : ui.cards)
            card->update();
    });
    ui.current = controller->trafficSample();
    arrange();
}
Dashboard::~Dashboard() = default;
bool Dashboard::editing() const {
    return m_ui->editing;
}
bool Dashboard::contains(Kind kind) const {
    for (auto* card : m_ui->cards)
        if (card->kind == kind)
            return true;
    return false;
}
void Dashboard::setEditing(bool editing) {
    m_ui->editing = editing;
    for (auto* card : m_ui->cards) {
        card->remove->setVisible(editing);
        card->setCursor(editing ? Qt::OpenHandCursor : Qt::ArrowCursor);
        card->setToolTip(editing ? tr("Drag to move; drag the lower-right corner to resize. Arrow "
                                      "keys move; Shift+arrows resize; Delete removes.")
                                 : card->accessibleDescription());
        card->update();
    }
    if (!editing)
        persist();
}
void Dashboard::addWidget(Kind kind) {
    if (!editing() || contains(kind))
        return;
    int row = 0;
    for (auto* card : m_ui->cards)
        row = qMax(row, card->cells.y() + card->cells.height());
    m_ui->cards.append(new DashboardCard(this, kind, QRect(0, row, 2, 2)));
    arrange();
    persist();
    m_ui->scroll->ensureWidgetVisible(m_ui->cards.last());
}
void Dashboard::removeWidget(Kind kind) {
    if (!editing())
        return;
    for (auto* card : m_ui->cards)
        if (card->kind == kind) {
            m_ui->cards.removeOne(card);
            card->hide();
            card->deleteLater();
            break;
        }
    arrange();
    persist();
}
QVariantList Dashboard::savedLayout() const {
    QVariantList entries;
    for (auto* card : m_ui->cards)
        entries.append(QVariantMap{{"kind", int(card->kind)},
                                   {"x", card->cells.x()},
                                   {"y", card->cells.y()},
                                   {"width", card->cells.width()},
                                   {"height", card->cells.height()}});
    return entries;
}
void Dashboard::persist() {
    m_ui->controller->saveDashboardLayout(savedLayout());
    emit layoutChanged();
}
void Dashboard::arrange() {
    auto& ui = *m_ui;
    const int width = ui.scroll->viewport()->width();
    int bottom = 120;
    for (auto* card : ui.cards) {
        const auto& c = card->cells;
        card->setGeometry(c.x() * (width + Gap) / Columns, c.y() * (RowHeight + Gap),
                          c.width() * (width + Gap) / Columns - Gap,
                          c.height() * (RowHeight + Gap) - Gap);
        bottom = qMax(bottom, card->geometry().bottom() + 1);
    }
    ui.canvas->setMinimumHeight(bottom);
    ui.empty->setVisible(ui.cards.isEmpty());
    ui.empty->setGeometry(16, 16, width - 32, 100);
    // A changed vertical scrollbar changes the available column width.
    if (width != ui.scroll->viewport()->width())
        QTimer::singleShot(0, this, &Dashboard::arrange);
}
void Dashboard::commit(DashboardCard* card, const QRect& geometry) {
    const double step = (m_ui->scroll->viewport()->width() + Gap) / double(Columns);
    const int span = qBound(2, qRound((geometry.width() + Gap) / step), 4);
    card->cells = QRect(qBound(0, qRound(geometry.x() / step), Columns - span),
                        qBound(0, qRound(geometry.y() / double(RowHeight + Gap)), 40), span,
                        qBound(2, qRound((geometry.height() + Gap) / double(RowHeight + Gap)), 4));
    QList<DashboardCard*> placed{card};
    for (auto* other : m_ui->cards) {
        if (other == card)
            continue;
        bool collision;
        do {
            collision = false;
            for (auto* previous : placed)
                if (other->cells.intersects(previous->cells)) {
                    other->cells.moveTop(previous->cells.y() + previous->cells.height());
                    collision = true;
                    break;
                }
        } while (collision);
        placed.append(other);
    }
    arrange();
    persist();
}
void Dashboard::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    arrange();
    QTimer::singleShot(0, this, &Dashboard::arrange);
}
bool Dashboard::eventFilter(QObject* object, QEvent* event) {
    if (object == m_ui->scroll->viewport() && event->type() == QEvent::Resize)
        QTimer::singleShot(0, this, &Dashboard::arrange);
    return QWidget::eventFilter(object, event);
}
