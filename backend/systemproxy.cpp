#include "systemproxy.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QStandardPaths>

SystemProxy::SystemProxy(QObject* parent) : QObject(parent) {
    m_reason = tr("System proxy is available on GNOME and KDE Plasma on Linux.");
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &SystemProxy::readReplies);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError value) {
        if (value != QProcess::FailedToStart)
            return;
        m_ready = true;
        m_available = false;
        m_reason = tr("Could not start the system-proxy helper: %1").arg(m_process.errorString());
        emit error(m_reason);
        emit changed();
        emit stoppedSignal();
    });
    connect(&m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) {
                readReplies();
                m_ready = true;
                m_available = m_active = m_pending = false;
                if (!m_shutdown || code || status != QProcess::NormalExit) {
                    m_reason = tr(
                        "System-proxy helper stopped. Restart Sawfish to recover proxy settings.");
                    emit error(m_reason);
                }
                emit changed();
                emit stoppedSignal();
            });
}

SystemProxy::~SystemProxy() {
    disconnect(&m_process, nullptr, this, nullptr);
    m_process.closeWriteChannel();
    if (m_process.state() != QProcess::NotRunning)
        m_process.waitForFinished(10000);
}

void SystemProxy::initialize(const QString& directory, const QString& executable) {
#ifdef Q_OS_LINUX
    if (m_started)
        return;
    m_started = true;
    m_reason = tr("Checking desktop proxy settings…");
    const auto path = directory.isEmpty()
                          ? QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
                                "/sawfish/system-proxy"
                          : directory;
    m_process.setProcessChannelMode(QProcess::SeparateChannels);
    m_process.start(executable.isEmpty() ? QCoreApplication::applicationFilePath() : executable,
                    {"--system-proxy-helper", path});
    emit changed();
#else
    Q_UNUSED(directory)
    Q_UNUSED(executable)
#endif
}

void SystemProxy::setTarget(const ProxyEndpoint& endpoint) {
    m_target = endpoint;
    sendTarget();
}

void SystemProxy::sendTarget() {
    if (!m_ready || !m_available || m_pending || m_shutdown || stopped())
        return;
    const auto target = m_target.valid() ? m_target.json() : QJsonObject{};
    if (target == m_completed)
        return;
    m_sent = target;
    m_pending = true;
    m_process.write(QJsonDocument(QJsonObject{{"target", target}}).toJson(QJsonDocument::Compact) +
                    '\n');
    emit changed();
}

void SystemProxy::readReplies() {
    m_output += m_process.readAllStandardOutput();
    m_process.readAllStandardError();
    while (m_output.contains('\n')) {
        const auto end = m_output.indexOf('\n');
        const auto reply = QJsonDocument::fromJson(m_output.left(end)).object();
        m_output.remove(0, end + 1);
        if (reply.contains("ready")) {
            m_ready = true;
            m_available = reply["available"].toBool();
            m_reason = reply["error"].toString();
            if (!m_available && !m_reason.isEmpty())
                emit error(m_reason);
        } else {
            m_pending = false;
            m_active = reply["active"].toBool();
            m_reason = reply["error"].toString();
            // A failed target is not automatically retried; a new user intent
            // or a new connection is required.
            m_completed = m_sent;
            if (!m_reason.isEmpty())
                emit error(m_reason);
        }
        emit changed();
        sendTarget();
    }
}

void SystemProxy::shutdown() {
    m_shutdown = true;
    m_target = {};
    m_process.closeWriteChannel();
    if (stopped())
        emit stoppedSignal();
}
