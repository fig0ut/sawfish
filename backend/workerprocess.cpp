#include "workerprocess.h"
#include "ownedprocess.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <utility>

WorkerProcess::WorkerProcess(QObject* parent) : QObject(parent) {
    bindProcessToParent(m_process);
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(10000);
    m_stopTimeout.setSingleShot(true);
    m_stopTimeout.setInterval(1000);
    connect(&m_stopTimeout, &QTimer::timeout, &m_process, &QProcess::kill);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        m_stopping = true;
        m_ready = false;
        m_process.kill();
        emit unavailable(tr("The configuration worker did not become ready within 10 seconds."));
    });
    connect(&m_process, &QProcess::readyReadStandardError, this,
            [this] { m_errors = (m_errors + m_process.readAllStandardError()).right(8192); });
    connect(&m_process, &QProcess::readyReadStandardOutput, this, [this] {
        m_output = (m_output + m_process.readAllStandardOutput()).right(8192);
        while (m_output.contains('\n')) {
            const auto end = m_output.indexOf('\n');
            const auto line = m_output.left(end).trimmed();
            m_output.remove(0, end + 1);
            if (line == "READY" && !m_ready && !m_stopping && m_directory) {
                m_ready = true;
                m_timeout.stop();
                emit ready(m_directory->filePath("worker.sock"));
            }
        }
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        m_timeout.stop();
        m_stopTimeout.stop();
        m_ready = false;
        if (m_restartRequested) {
            const auto executable = std::exchange(m_nextExecutable, {});
            start(executable);
            return;
        }
        if (m_stopping) {
            emit stopped();
            return;
        }
        emit unavailable(
            tr("Cannot start the configuration worker: %1").arg(m_process.errorString()));
        emit stopped();
    });
    connect(&m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) {
                m_ready = false;
                m_timeout.stop();
                m_stopTimeout.stop();
                if (m_restartRequested) {
                    const auto executable = std::exchange(m_nextExecutable, {});
                    start(executable);
                    return;
                }
                if (!m_stopping)
                    emit unavailable(tr("Configuration worker exited (%1). %2")
                                         .arg(code)
                                         .arg(QString::fromUtf8(m_errors).trimmed()));
                emit stopped();
            });
}

WorkerProcess::~WorkerProcess() {
    stop();
}

void WorkerProcess::shutdown() {
    m_stopping = true;
    m_ready = false;
    m_timeout.stop();
    m_restartRequested = false;
    m_nextExecutable.clear();
    if (!running()) {
        emit stopped();
        return;
    }
    m_process.closeWriteChannel();
    m_stopTimeout.start();
}

void WorkerProcess::stop() {
    m_stopping = true;
    m_ready = false;
    m_timeout.stop();
    m_stopTimeout.stop();
    m_nextExecutable.clear();
    m_restartRequested = false;
    if (m_process.state() != QProcess::NotRunning) {
        m_process.closeWriteChannel();
        if (!m_process.waitForFinished(1000)) {
            m_process.terminate();
            if (!m_process.waitForFinished(1000)) {
                m_process.kill();
                m_process.waitForFinished(1000);
            }
        }
    }
    m_directory.reset();
    m_stopping = false;
}

void WorkerProcess::start(const QString& executable) {
    if (m_process.state() != QProcess::NotRunning) {
        m_stopping = true;
        m_ready = false;
        m_timeout.stop();
        m_restartRequested = true;
        m_nextExecutable = executable;
        m_process.closeWriteChannel();
        m_stopTimeout.start();
        return;
    }
    stop();
    m_output.clear();
    m_errors.clear();
    if (!QFileInfo(executable).isExecutable() || !QFileInfo(executable).isFile()) {
        emit unavailable(tr("Select a Rostra desktop daemon executable in Settings."));
        return;
    }
    m_directory = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/sawfish-worker-XXXXXX");
    if (!m_directory->isValid() || m_directory->filePath("worker.sock").toUtf8().size() > 100) {
        emit unavailable(tr("Cannot create a private worker socket directory."));
        return;
    }
    m_process.setWorkingDirectory(m_directory->path());
    m_process.setProgram(QFileInfo(executable).absoluteFilePath());
    m_process.setArguments({"worker", "--socket", m_directory->filePath("worker.sock"),
                            "--parent-pid", QString::number(QCoreApplication::applicationPid())});
#ifdef Q_OS_WIN
    m_process.setArguments({"worker", "--owned", "--socket",
                            QDir::fromNativeSeparators(m_directory->filePath("worker.sock")),
                            "--parent-pid", QString::number(QCoreApplication::applicationPid())});
#endif
    m_process.start(QIODevice::ReadWrite);
    m_timeout.start();
}
