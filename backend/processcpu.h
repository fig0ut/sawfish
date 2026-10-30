#pragma once
#include <QElapsedTimer>
#include <optional>

// CPU time of the owned Rostra process. 100% represents one fully used CPU core.
class ProcessCpu {
  public:
    std::optional<double> sample(qint64 pid);
    void reset();

  private:
    QElapsedTimer m_timer;
    qint64 m_pid = 0;
    quint64 m_ticks = 0, m_start = 0;
};
