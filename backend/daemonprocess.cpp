#include "daemonprocess.h"
#include "ownedprocess.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

DaemonProcess::DaemonProcess(QString directory, QObject* parent)
    : QObject(parent), m_directory(std::move(directory)) {
    bindProcessToParent(m_process);
    m_stopDeadline.setSingleShot(true);
    m_stopDeadline.setInterval(5000);
    connect(&m_stopDeadline, &QTimer::timeout, &m_process, &QProcess::kill);
    connect(&m_process, &QProcess::started, this, [this] {
        if (m_stopping)
            stop();
        else
            emit started(socket());
    });
    connect(&m_process, &QProcess::readyReadStandardError, this,
            [this] { m_errors = (m_errors + m_process.readAllStandardError()).right(8192); });
    m_process.setStandardOutputFile(QProcess::nullDevice());
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        m_stopDeadline.stop();
        m_runtime.reset();
        m_lock.reset();
        if (!m_stopping)
            emit unavailable(tr("Cannot start Rostra: %1").arg(m_process.errorString()), false);
        emit stopped();
    });
    connect(&m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) {
                m_stopDeadline.stop();
                m_errors = (m_errors + m_process.readAllStandardError()).right(8192);
                m_runtime.reset();
                m_lock.reset();
                if (!m_stopping)
                    emit unavailable(tr("Rostra stopped (%1). %2")
                                         .arg(code)
                                         .arg(QString::fromUtf8(m_errors).trimmed()),
                                     true);
                emit stopped();
            });
}

DaemonProcess::~DaemonProcess() {
    stopAndWait();
}

void DaemonProcess::resetAttempts() {
    m_attempts = 0;
    m_attemptWindow.restart();
}

void DaemonProcess::start(const QString& executable) {
    if (running())
        return;
#if !defined(Q_OS_LINUX) && !defined(Q_OS_WIN)
    emit unavailable(tr("Automatic core management is supported on Linux and Windows."), false);
    return;
#endif
    if (!m_attemptWindow.isValid() || m_attemptWindow.elapsed() >= 60000)
        resetAttempts();
    if (m_attempts >= 4) {
        emit unavailable(tr("Rostra repeatedly stopped. Check the configuration and choose Retry."),
                         false);
        return;
    }
    if (!QFileInfo(executable).isFile() || !QFileInfo(executable).isExecutable()) {
        emit unavailable(tr("Rostra is missing. Select its executable in Settings → Advanced."),
                         false);
        return;
    }
    if (!QDir().mkpath(m_directory) ||
        !QFile::setPermissions(m_directory,
                               QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
        emit unavailable(tr("Cannot create Rostra's private state directory."), false);
        return;
    }
    m_lock = std::make_unique<QLockFile>(m_directory + "/owner.lock");
    m_lock->setStaleLockTime(0);
    if (!m_lock->tryLock()) {
        m_lock.reset();
        emit unavailable(tr("Another Sawfish instance is using this core state directory."), false);
        return;
    }
    // A fresh, private endpoint never replaces a separately running daemon's socket.
    m_runtime = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/sawfish-core-XXXXXX");
    if (!m_runtime->isValid() || socket().toUtf8().size() > 100) {
        m_runtime.reset();
        m_lock.reset();
        emit unavailable(tr("Cannot create Rostra's private runtime directory."), false);
        return;
    }
    ++m_attempts;
    m_stopping = false;
    m_errors.clear();
    m_process.setProgram(QFileInfo(executable).absoluteFilePath());
    m_process.setArguments(
        {"run", "-D", QFileInfo(m_directory).absoluteFilePath(), "--socket", socket()});
#ifdef Q_OS_WIN
    m_process.setArguments({"run", "--owned", "-D", QFileInfo(m_directory).absoluteFilePath(),
                            "--socket", QDir::fromNativeSeparators(socket()), "--parent-pid",
                            QString::number(QCoreApplication::applicationPid())});
#endif
    m_process.setWorkingDirectory(m_directory);
    m_process.start(QIODevice::ReadWrite);
}

void DaemonProcess::stop() {
    m_stopping = true;
    if (!running()) {
        emit stopped();
        return;
    }
#ifdef Q_OS_WIN
    m_process.closeWriteChannel();
#else
    m_process.terminate();
#endif
    m_stopDeadline.start();
}

void DaemonProcess::stopAndWait() {
    m_stopping = true;
    m_stopDeadline.stop();
    if (running()) {
#ifdef Q_OS_WIN
        m_process.closeWriteChannel();
#else
        m_process.terminate();
#endif
        if (!m_process.waitForFinished(5000)) {
            m_process.kill();
            m_process.waitForFinished(1000);
        }
    }
    m_runtime.reset();
    m_lock.reset();
}
