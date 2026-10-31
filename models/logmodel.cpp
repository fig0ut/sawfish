#include "logmodel.h"
#include <QRegularExpression>
#include <utility>

namespace {
QString plainLog(QString message) {
    // Strip terminal control sequences before they reach display, search or copy.
    // Keep control characters as regex escapes. MSVC can drop a C++ \x9b
    // inside QStringLiteral, leaving an empty alternative that matches log text.
    static const QRegularExpression osc(
        QStringLiteral(R"(\x{1b}\][^\x{07}\x{1b}]*(?:\x{07}|\x{1b}\\))"));
    static const QRegularExpression csi(QStringLiteral(R"((?:\x{1b}\[|\x{9b})[0-?]*[ -/]*[@-~])"));
    // Some core log paths have already removed ESC but left the SGR payload.
    static const QRegularExpression sgr(QStringLiteral("\\[[0-9;:]*m"));
    static const QRegularExpression prefix(
        QStringLiteral("^(?:PANIC|FATAL|ERROR|WARN|INFO|DEBUG|TRACE)(?=\\[\\d+\\])"));
    message.remove(osc).remove(csi).remove(sgr).remove(prefix);
    message.remove(QChar(0x1b));
    return message;
}
} // namespace

int LogModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : int(m_lines.size());
}

QString LogModel::levelName(int level) {
    static const QStringList names = {"PANIC", "FATAL", "ERROR", "WARN", "INFO", "DEBUG", "TRACE"};
    return level >= 0 && level < names.size() ? names[level] : QStringLiteral("UNKNOWN");
}

QVariant LogModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_lines.size())
        return {};
    const auto& line = m_lines[index.row()];
    if (role == Qt::DisplayRole)
        return (line.application ? QStringLiteral("[APP] [%1] %2") : QStringLiteral("[%1] %2"))
            .arg(levelName(line.level), line.message);
    if (role == MessageRole || role == Qt::ToolTipRole)
        return line.application ? QStringLiteral("[APP] ") + line.message : line.message;
    if (role == LevelRole)
        return line.level;
    return {};
}

void LogModel::bound(LogBatch& lines) const {
    qsizetype chars = 0;
    qsizetype keep = 0;
    for (auto it = lines.rbegin(); it != lines.rend() && keep < m_retention; ++it) {
        it->message = it->message.left(16384);
        chars += it->message.size();
        if (chars > 2 * 1024 * 1024)
            break;
        ++keep;
    }
    if (keep < lines.size())
        lines.remove(0, lines.size() - keep);
}

void LogModel::append(LogBatch lines, bool reset) {
    for (auto& line : lines)
        line.message = plainLog(line.message.left(16384));
    if (m_paused) {
        if (reset) {
            m_pending.removeIf([](const LogLine& line) { return !line.application; });
            m_pendingReset = true;
        }
        m_pending.append(lines);
        bound(m_pending);
        return;
    }
    if (reset) {
        beginResetModel();
        // A daemon stream reset replaces its history, not Sawfish's own events.
        m_lines.removeIf([](const LogLine& line) { return !line.application; });
        endResetModel();
    }
    if (lines.isEmpty())
        return;
    bound(lines);
    // Eviction is bounded by both rows and text size, including retained rows.
    LogBatch combined = m_lines;
    combined.append(lines);
    bound(combined);
    const auto removed = m_lines.size() + lines.size() - combined.size();
    if (removed > 0) {
        beginRemoveRows({}, 0, int(removed - 1));
        m_lines.remove(0, removed);
        endRemoveRows();
    }
    beginInsertRows({}, int(m_lines.size()), int(m_lines.size() + lines.size() - 1));
    m_lines.append(lines);
    endInsertRows();
}

void LogModel::setRetentionLimit(int lines) {
    lines = qBound(MinimumRetention, lines, MaximumRetention);
    if (m_retention == lines)
        return;
    m_retention = lines;
    auto retained = m_lines;
    bound(retained);
    const auto removed = m_lines.size() - retained.size();
    if (removed > 0) {
        beginRemoveRows({}, 0, int(removed - 1));
        m_lines = std::move(retained);
        endRemoveRows();
    }
    bound(m_pending);
    emit retentionChanged(lines);
}

void LogModel::appendApplication(const QString& message, bool error) {
    if (!message.isEmpty())
        append({{error ? 2 : 4, message, true}});
}

void LogModel::clear() {
    beginResetModel();
    m_lines.clear();
    endResetModel();
    m_pending.clear();
    m_pendingReset = false;
}

void LogModel::setPaused(bool paused) {
    if (m_paused == paused)
        return;
    m_paused = paused;
    if (!paused) {
        auto pending = std::exchange(m_pending, {});
        const bool reset = std::exchange(m_pendingReset, false);
        append(std::move(pending), reset);
    }
}

LogFilter::LogFilter(QObject* parent) : QSortFilterProxyModel(parent) {
    setFilterCaseSensitivity(Qt::CaseInsensitive);
    setFilterRole(LogModel::MessageRole);
}

void LogFilter::setMaximumLevel(int level) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
    m_maxLevel = level;
    endFilterChange(Direction::Rows);
#else
    m_maxLevel = level;
    invalidateFilter();
#endif
}

bool LogFilter::filterAcceptsRow(int row, const QModelIndex& parent) const {
    return sourceModel()->index(row, 0, parent).data(LogModel::LevelRole).toInt() <= m_maxLevel &&
           QSortFilterProxyModel::filterAcceptsRow(row, parent);
}
