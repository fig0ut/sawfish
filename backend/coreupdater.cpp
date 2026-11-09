#include "coreupdater.h"
#include "app/controller.h"
#include "ownedprocess.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QSysInfo>
#include <QUrlQuery>

namespace {
constexpr qint64 MaxBinary = 256LL * 1024 * 1024;
QString normalizedVersion(QString value) {
    if (value.startsWith('v'))
        value.remove(0, 1);
    return value;
}
bool localUrl(const QUrl& url) {
    return url.scheme() == "http" && (url.host() == "127.0.0.1" || url.host() == "::1");
}
} // namespace

CoreUpdater::CoreUpdater(Controller* controller, QString directory, QUrl releases)
    : QObject(controller), m_controller(controller), m_directory(std::move(directory)),
      m_settings(m_directory + "/settings.ini", QSettings::IniFormat),
      m_releases(std::move(releases)) {
    m_channel = m_settings.value("core/channel").toString() == "prerelease" ? Prerelease : Stable;
    m_status = tr("Check this channel for a Rostra desktop release.");
    m_localTest = localUrl(m_releases);
    bindProcessToParent(m_probe);
    m_deadline.setSingleShot(true);
    connect(&m_deadline, &QTimer::timeout, this, [this] { fail(tr("Rostra update timed out.")); });
    connect(&m_probe, &QProcess::readyReadStandardOutput, this, [this] {
        m_output += m_probe.readAllStandardOutput();
        if (m_output.size() > 64 * 1024)
            fail(tr("The downloaded core returned too much output."));
    });
    connect(&m_probe, &QProcess::readyReadStandardError, this,
            [this] { m_probe.readAllStandardError(); });
    connect(&m_probe, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            fail(tr("Cannot run the downloaded core: %1").arg(m_probe.errorString()));
    });
    connect(&m_probe, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            &CoreUpdater::probeFinished);
    connect(controller, &Controller::changed, this, [this] {
        if (!m_queued && m_state == Installing) {
            m_queued = true;
            QTimer::singleShot(0, this, [this] {
                m_queued = false;
                advance();
            });
        }
    });
    connect(&controller->m_core, &DaemonProcess::unavailable, this,
            [this](const QString& error, bool) {
                if (m_state == Installing && (m_phase == Ready || m_phase == Resume))
                    fail(error);
            });
    connect(&controller->m_worker, &WorkerProcess::unavailable, this, [this](const QString& error) {
        if (m_state == Installing && (m_phase == Ready || m_phase == Resume))
            fail(error);
    });
}

CoreUpdater::~CoreUpdater() {
    abortWork();
    m_probe.kill();
    m_probe.waitForFinished(1000);
}

void CoreUpdater::recoverInterrupted(QSettings& settings) {
    settings.sync();
    if (!settings.contains("core/pendingPrevious"))
        return;
    settings.setValue("backend/executable", settings.value("core/pendingPrevious"));
    settings.remove("core/pendingPrevious");
    settings.sync();
}

QString CoreUpdater::platformAsset() {
#if defined(Q_OS_WIN)
    const QString os = "windows", suffix = ".exe";
#elif defined(Q_OS_LINUX)
    const QString os = "linux", suffix;
#else
    return {};
#endif
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
    auto arch = QSysInfo::buildCpuArchitecture();
    if (arch == "x86_64")
        arch = "amd64";
    else if (arch == "arm64" || arch == "aarch64")
        arch = "arm64";
    else
        return {};
    return "rostra-desktop-" + os + "-" + arch + suffix;
#endif
}

CoreRelease CoreUpdater::parseRelease(const QJsonObject& release, Channel channel,
                                      const QString& assetName, bool allowLocalTest) {
    if (assetName.isEmpty() || release.value("draft").toBool() ||
        release.value("prerelease").toBool() != (channel == Prerelease))
        return {};
    const auto version = release.value("tag_name").toString();
    if (version.isEmpty() || version.size() > 128)
        return {};
    for (const auto& entry : release.value("assets").toArray()) {
        const auto asset = entry.toObject();
        if (asset.value("name").toString() != assetName ||
            asset.value("state").toString() != "uploaded")
            continue;
        const auto digest = asset.value("digest").toString();
        static const QRegularExpression sha("^sha256:[a-fA-F0-9]{64}$");
        const QUrl url(asset.value("browser_download_url").toString());
        const bool trusted = url.scheme() == "https" && url.host() == "github.com" &&
                             url.path().startsWith("/fig0ut/rostra/releases/download/") &&
                             url.userInfo().isEmpty() && url.port(-1) == -1;
        const qint64 size = asset.value("size").toInteger();
        if (!sha.match(digest).hasMatch() || size <= 0 || size > MaxBinary ||
            (!trusted && !(allowLocalTest && localUrl(url))))
            continue;
        return {version, digest.mid(7).toLower(), url, size};
    }
    return {};
}

void CoreUpdater::setState(State state, const QString& message) {
    m_state = state;
    m_status = message;
    emit changed();
}
void CoreUpdater::setChannel(Channel channel) {
    if (busy() || channel == m_channel)
        return;
    m_channel = channel;
    m_release = {};
    m_settings.setValue("core/channel", channel == Prerelease ? "prerelease" : "stable");
    m_settings.sync();
    setState(Idle, tr("Check this channel for a Rostra desktop release."));
}

void CoreUpdater::fetch(const QUrl& url, qint64 limit, std::function<void(QByteArray)> complete) {
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "Sawfish/0.1");
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setTransferTimeout(30000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    auto* reply = m_network.get(request);
    reply->setReadBufferSize(256 * 1024);
    m_reply = reply;
    auto body = std::make_shared<QByteArray>();
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, body, limit] {
        if (m_reply != reply)
            return;
        body->append(reply->readAll());
        if (body->size() > limit)
            fail(tr("The release response exceeds the size limit."));
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, body, limit, complete] {
        reply->deleteLater();
        if (m_reply != reply)
            return;
        m_reply = nullptr;
        if (reply->error() != QNetworkReply::NoError) {
            fail(tr("Cannot fetch Rostra releases: %1").arg(reply->errorString()));
            return;
        }
        body->append(reply->readAll());
        if (body->size() > limit) {
            fail(tr("The release response exceeds the size limit."));
            return;
        }
        complete(*body);
    });
}

void CoreUpdater::check() {
    if (busy() || m_controller->shuttingDown())
        return;
    if (platformAsset().isEmpty()) {
        fail(tr("Core downloads are not available for this platform."));
        return;
    }
    m_release = {};
    m_phase = None;
    m_releasePublished.clear();
    setState(Checking, tr("Checking Rostra releases…"));
    m_deadline.start(60000);
    fetchPage(1);
}

void CoreUpdater::fetchPage(int page) {
    auto url = m_releases;
    QUrlQuery query;
    query.addQueryItem("per_page", "100");
    query.addQueryItem("page", QString::number(page));
    url.setQuery(query);
    fetch(url, 5 * 1024 * 1024, [this, page](QByteArray data) {
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(data, &error);
        if (error.error != QJsonParseError::NoError || !doc.isArray()) {
            fail(tr("The release server returned invalid data."));
            return;
        }
        auto releases = doc.array();
        for (const auto& entry : releases) {
            const auto release = entry.toObject();
            auto candidate = parseRelease(release, m_channel, platformAsset(), m_localTest);
            const auto published = release.value("published_at").toString();
            if (candidate.valid() && (!m_release.valid() || published > m_releasePublished)) {
                m_release = candidate;
                m_releasePublished = published;
            }
        }
        // GitHub's list order need not match publication order (an old tag can
        // receive a newly published release), so compare across pages as well.
        if (releases.size() == 100 && page < 5) {
            fetchPage(page + 1);
            return;
        }
        m_deadline.stop();
        if (!m_release.valid()) {
            setState(Idle, tr("No compatible desktop download is published in this channel yet."));
            return;
        }
        m_settings.sync();
        if (m_settings.value("core/installedDigest").toString() == m_release.digest &&
            QFileInfo(m_controller->executable()).absoluteFilePath() ==
                m_settings.value("core/installedPath").toString()) {
            setState(Idle, tr("Rostra %1 is already installed.").arg(m_release.version));
            return;
        }
        setState(Available, tr("Rostra %1 is available.").arg(m_release.version));
    });
}

void CoreUpdater::command(const std::function<void()>& action) {
    {
        QScopedValueRollback<bool> allow(m_controller->m_coreUpdating, false);
        action();
    }
    emit m_controller->changed();
}

void CoreUpdater::install() {
    if (m_state != Available || !m_release.valid() || m_controller->busy() ||
        !m_controller->managedBackend() || m_controller->importing() ||
        m_controller->shuttingDown() || m_controller->state() == Controller::Starting ||
        m_controller->state() == Controller::Stopping ||
        (m_controller->m_core.running() && !m_controller->online()))
        return;
    m_controller->m_coreUpdating = true;
    m_previous = m_controller->executable();
    m_resume = m_controller->state() == Controller::Running;
    m_profileName = m_controller->activeName();
    m_profileId = m_controller->activeId();
    m_config =
        m_resume
            ? m_controller->m_activeContent
            : (m_controller->selectedProfile() ? m_controller->selectedProfile()->content : "{}");
    m_failure.clear();
    m_activated = false;
    m_phase = None;
    m_progress = 0;
    m_received = 0;
    m_hash.reset();
    const auto directory = m_directory + "/cores/" + m_release.digest;
    if (!QDir().mkpath(directory)) {
        fail(tr("Cannot create the core version directory."));
        return;
    }
    QFile::setPermissions(directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    m_candidate = directory + "/" + platformAsset();
    m_file = std::make_unique<QSaveFile>(m_candidate);
    if (!m_file->open(QIODevice::WriteOnly)) {
        fail(tr("Cannot write the downloaded core."));
        return;
    }
    QNetworkRequest request(m_release.url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "Sawfish/0.1");
    request.setTransferTimeout(30000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    auto* reply = m_network.get(request);
    reply->setReadBufferSize(256 * 1024);
    m_reply = reply;
    setState(Downloading, tr("Downloading Rostra %1…").arg(m_release.version));
    emit m_controller->changed();
    m_deadline.start(10 * 60 * 1000);
    auto consume = [this, reply] {
        if (m_reply != reply || !m_file)
            return;
        const auto bytes = reply->readAll();
        m_received += bytes.size();
        if (m_received > m_release.size || m_file->write(bytes) != bytes.size()) {
            fail(tr("Invalid download size or disk write failure."));
            return;
        }
        m_hash.addData(bytes);
        m_progress = int(m_received * 100 / m_release.size);
        emit changed();
    };
    connect(reply, &QNetworkReply::readyRead, this, consume);
    connect(reply, &QNetworkReply::finished, this, [this, reply, consume] {
        reply->deleteLater();
        if (m_reply != reply)
            return;
        consume();
        if (m_reply != reply)
            return;
        m_reply = nullptr;
        if (reply->error() != QNetworkReply::NoError) {
            fail(tr("Core download failed: %1").arg(reply->errorString()));
            return;
        }
        if (m_received != m_release.size ||
            QString::fromLatin1(m_hash.result().toHex()) != m_release.digest) {
            fail(tr("The core download failed SHA-256 verification."));
            return;
        }
        if (!m_file->commit()) {
            fail(tr("Cannot save the verified core."));
            return;
        }
        m_file.reset();
        if (!QFile::setPermissions(m_candidate,
                                   QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
            fail(tr("Cannot make the core executable."));
            return;
        }
        probe();
    });
}

void CoreUpdater::probe() {
    setState(Verifying, tr("Checking core compatibility and configuration…"));
    m_phase = Info;
    m_output.clear();
    m_deadline.start(30000);
    m_probe.setWorkingDirectory(QFileInfo(m_candidate).absolutePath());
    m_probe.start(m_candidate, {"version"});
}

void CoreUpdater::probeFinished(int code, QProcess::ExitStatus status) {
    if (m_state != Verifying)
        return;
    if (code != 0 || status != QProcess::NormalExit) {
        fail(tr("The downloaded core failed compatibility or configuration checks."));
        return;
    }
    if (m_phase == Info) {
        m_output += m_probe.readAllStandardOutput();
        const auto info = QJsonDocument::fromJson(m_output).object();
        if (info.value("api_version").toInt() != 4 ||
            normalizedVersion(info.value("version").toString()) !=
                normalizedVersion(m_release.version)) {
            fail(tr("The downloaded core is incompatible with this Sawfish version."));
            return;
        }
        m_phase = Config;
        m_output.clear();
        m_deadline.start(30000);
        m_probe.start(m_candidate, {"check"});
        m_probe.write(m_config.toUtf8());
        m_probe.closeWriteChannel();
        return;
    }
    activate();
}

void CoreUpdater::activate() {
    // Persist recovery before interrupting a working session. If saving fails,
    // the current connection remains untouched.
    m_settings.setValue("core/pendingPrevious", m_previous);
    m_settings.sync();
    if (m_settings.status() != QSettings::NoError) {
        m_settings.remove("core/pendingPrevious");
        fail(tr("Cannot save the update recovery record."));
        return;
    }
    m_activated = true;
    m_phase = Stop;
    m_deadline.start(45000);
    setState(Installing, tr("Restarting Rostra…"));
    if (m_controller->state() == Controller::Running)
        command([this] { m_controller->disconnectService(); });
    advance();
}

void CoreUpdater::advance() {
    if (m_state != Installing || m_controller->shuttingDown())
        return;
    if (m_phase == Stop) {
        if (m_controller->m_operation != Controller::None)
            return;
        if (m_controller->state() == Controller::Stopping ||
            m_controller->state() == Controller::Starting)
            return;
        if (m_controller->state() == Controller::Running) {
            fail(tr("Could not stop the current connection for the update."));
            return;
        }
        m_phase = Ready;
        command([this] { m_controller->configure(m_candidate, {}); });
        return;
    }
    if (m_phase == Ready || m_phase == Rollback) {
        if (!m_controller->online() || !m_controller->m_worker.isReady())
            return;
        if (m_resume && m_controller->state() == Controller::Running) {
            finish(m_failure.isEmpty());
            return;
        }
        if (m_controller->state() != Controller::Idle)
            return;
        if (m_resume) {
            QString error;
            const auto runtime = m_controller->runtimeConfiguration(m_config, &error);
            if (!error.isEmpty()) {
                fail(error);
                return;
            }
            m_phase = Resume;
            // Restore exactly the session we checked, even if it was restored
            // by the daemon and does not correspond to the selected profile.
            m_controller->m_pendingContent = m_config;
            m_controller->m_pendingName = m_profileName;
            m_controller->m_pendingProfile = m_profileId;
            m_controller->m_operation = Controller::Start;
            m_controller->m_pendingId = ++m_controller->m_nextId;
            m_controller->m_rpc.startService(m_controller->m_pendingId, runtime);
        } else
            finish(m_failure.isEmpty());
    } else if (m_phase == Resume) {
        if (m_controller->state() == Controller::Running &&
            m_controller->m_operation == Controller::None)
            finish(m_failure.isEmpty());
        else if (m_controller->state() == Controller::Failed ||
                 (m_controller->messageIsError() && m_controller->m_operation == Controller::None))
            fail(tr("The updated core could not restore the connection."));
    }
}

void CoreUpdater::fail(const QString& message) {
    if (m_reply) {
        auto reply = m_reply;
        m_reply = nullptr;
        reply->abort();
        reply->deleteLater();
    }
    m_file.reset();
    m_deadline.stop();
    const bool activated = m_activated;
    m_phase = None; // Ignore late probe results.
    if (m_probe.state() != QProcess::NotRunning) {
        m_probe.kill();
    }
    if (activated && m_failure.isEmpty() && !m_controller->shuttingDown()) {
        m_failure = message;
        m_phase = Rollback;
        setState(Installing, tr("Update failed; restoring the previous core…"));
        m_controller->m_operation = Controller::None;
        m_controller->m_pendingId = 0;
        m_controller->m_state = Controller::Unavailable;
        command([this] { m_controller->configure(m_previous, {}); });
        m_deadline.start(45000);
        return;
    }
    if (m_activated)
        recoverInterrupted(m_settings);
    m_controller->m_coreUpdating = false;
    m_activated = false;
    setState(Failed, message);
    m_controller->notify(message, true);
    emit m_controller->changed();
}

void CoreUpdater::finish(bool success) {
    m_deadline.stop();
    m_phase = None;
    m_activated = false;
    m_settings.remove("core/pendingPrevious");
    if (success) {
        m_settings.setValue("core/previousPath", m_previous);
        m_settings.setValue("core/installedDigest", m_release.digest);
        m_settings.setValue("core/installedPath", m_candidate);
    }
    m_settings.sync();
    m_controller->m_coreUpdating = false;
    const auto message =
        success ? tr("Rostra %1 installed.").arg(m_release.version)
                : tr("Update failed; the previous core was restored. %1").arg(m_failure);
    setState(success ? Idle : Failed, message);
    m_controller->notify(message, !success);
    emit m_controller->changed();
}

void CoreUpdater::cancel() {
    // Once switching starts, finish activation or rollback as one transaction.
    // Application shutdown is the exception; startup will recover the journal.
    if (m_state == Installing && !m_controller->shuttingDown())
        return;
    abortWork();
    emit m_controller->changed();
}

void CoreUpdater::abortWork() {
    if (m_reply) {
        auto reply = m_reply;
        m_reply = nullptr;
        reply->abort();
        reply->deleteLater();
    }
    m_deadline.stop();
    m_phase = None;
    m_file.reset();
    if (m_probe.state() != QProcess::NotRunning)
        m_probe.kill();
    if (m_activated)
        recoverInterrupted(m_settings);
    m_activated = false;
    m_controller->m_coreUpdating = false;
    if (busy())
        setState(Idle, tr("Core update cancelled."));
}
