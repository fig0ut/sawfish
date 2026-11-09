#pragma once
#include "proxyprofile.h"
#include <QObject>
#include <QProcess>

class SystemProxy : public QObject {
    Q_OBJECT
  public:
    explicit SystemProxy(QObject* parent = nullptr);
    ~SystemProxy() override;
    void initialize(const QString& directory = {}, const QString& executable = {});
    void setTarget(const ProxyEndpoint& endpoint);
    void shutdown();
    bool available() const {
        return m_available;
    }
    bool active() const {
        return m_active;
    }
    bool busy() const {
        return m_pending || (m_started && !m_ready);
    }
    bool stopped() const {
        return m_process.state() == QProcess::NotRunning;
    }
    QString reason() const {
        return m_reason;
    }
  signals:
    void changed();
    void error(const QString& message);
    void stoppedSignal();

  private:
    void sendTarget();
    void readReplies();
    QProcess m_process;
    QByteArray m_output;
    ProxyEndpoint m_target;
    QJsonObject m_sent, m_completed;
    bool m_started = false, m_ready = false, m_available = false, m_active = false;
    bool m_pending = false, m_shutdown = false;
    QString m_reason;
};
