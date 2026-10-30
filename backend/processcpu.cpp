#include "processcpu.h"
#include <QFile>
#ifdef Q_OS_LINUX
#include <unistd.h>
#endif

void ProcessCpu::reset() {
    m_timer.invalidate();
    m_pid = 0;
    m_ticks = m_start = 0;
}

std::optional<double> ProcessCpu::sample(qint64 pid) {
#ifdef Q_OS_LINUX
    QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
    const auto frequency = sysconf(_SC_CLK_TCK);
    if (pid <= 0 || frequency <= 0 || !stat.open(QIODevice::ReadOnly)) {
        reset();
        return {};
    }
    const auto bytes = stat.readAll();
    // The command name may contain spaces and parentheses. Fields after its
    // closing ')' start at field 3; utime/stime/starttime are fields 14/15/22.
    const auto end = bytes.lastIndexOf(')');
    const auto fields = bytes.mid(end + 2).simplified().split(' ');
    if (end < 0 || fields.size() < 20) {
        reset();
        return {};
    }
    bool userOk, systemOk, startOk;
    const auto ticks = fields[11].toULongLong(&userOk) + fields[12].toULongLong(&systemOk);
    const auto start = fields[19].toULongLong(&startOk);
    if (!userOk || !systemOk || !startOk) {
        reset();
        return {};
    }
    double percent = 0;
    if (m_timer.isValid() && pid == m_pid && start == m_start && ticks >= m_ticks) {
        const auto elapsed = m_timer.nsecsElapsed();
        if (elapsed > 0)
            percent = double(ticks - m_ticks) * 1e11 / (frequency * double(elapsed));
    }
    m_timer.start();
    m_pid = pid;
    m_start = start;
    m_ticks = ticks;
    return percent;
#else
    Q_UNUSED(pid);
    return {};
#endif
}
