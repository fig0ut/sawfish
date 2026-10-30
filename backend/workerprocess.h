#pragma once
#include <QObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>
class WorkerProcess : public QObject {
    Q_OBJECT
  public:
    explicit WorkerProcess(QObject* parent = nullptr);
    ~WorkerProcess() override;
    void start(const QString& executable);
    void stop();
    void shutdown();
    bool running() const {
        return m_process.state() != QProcess::NotRunning;
    }
    bool isReady() const {
        return m_ready;
    }
  signals:
    void ready(QString socket);
    void unavailable(QString error);
    void stopped();

  private:
    QProcess m_process;
    QTimer m_timeout, m_stopTimeout;
    std::unique_ptr<QTemporaryDir> m_directory;
    QByteArray m_output, m_errors;
    QString m_nextExecutable;
    bool m_ready = false, m_stopping = false, m_restartRequested = false;
};
