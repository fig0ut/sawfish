#pragma once
#include <QElapsedTimer>
#include <QLockFile>
#include <QObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>

class DaemonProcess : public QObject {
    Q_OBJECT
  public:
    explicit DaemonProcess(QString dataDirectory, QObject* parent = nullptr);
    ~DaemonProcess() override;
    void start(const QString& executable);
    void stop();
    void stopAndWait();
    void resetAttempts();
    bool running() const {
        return m_process.state() != QProcess::NotRunning;
    }
    qint64 processId() const {
        return m_process.processId();
    }
    QString socket() const {
        return m_runtime ? m_runtime->filePath("control.sock") : QString{};
    }
  signals:
    void started(QString socket);
    void unavailable(QString error, bool retryable);
    void stopped();

  private:
    QProcess m_process;
    QTimer m_stopDeadline;
    QElapsedTimer m_attemptWindow;
    QString m_directory;
    std::unique_ptr<QLockFile> m_lock;
    std::unique_ptr<QTemporaryDir> m_runtime;
    QByteArray m_errors;
    int m_attempts = 0;
    bool m_stopping = false;
};
