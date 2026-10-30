#include "controller.h"
#include "backend/coreupdater.h"
#include <QDir>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <algorithm>

Controller::Controller(QString directory, QString executable, QString socket, QObject* parent,
                       RpcClient::ChannelFactory factory)
    : QObject(parent), m_rpc(nullptr, std::move(factory)), m_core(directory + "/core"),
      m_profiles(directory + "/profiles"),
      m_settings(directory + "/settings.ini", QSettings::IniFormat),
      m_executable(executable.isEmpty() ? m_settings.value("backend/executable").toString()
                                        : executable),
      m_socket(socket), m_managed(socket.isEmpty()) {
    m_selected = m_settings.value("profile/selected").toString();
    m_logs.setRetentionLimit(m_settings.value("logs/retention", LogModel::MaxLines).toInt());
    m_retry.setSingleShot(true);
    connect(&m_retry, &QTimer::timeout, this, &Controller::retry);
    m_groupRetry.setSingleShot(true);
    m_groupRetry.setInterval(2000);
    connect(&m_groupRetry, &QTimer::timeout, this, &Controller::refreshGroups);
    m_connectionRetry.setSingleShot(true);
    m_connectionRetry.setInterval(2000);
    connect(&m_connectionRetry, &QTimer::timeout, this, &Controller::refreshConnections);
    connect(&m_rpc, &RpcClient::connections, this, [this](ConnectionList entries) {
        if (m_shuttingDown || !m_online || m_state != Running)
            return;
        m_connectionsReady = true;
        m_connectionsError.clear();
        m_connections.replace(std::move(entries));
        emit connectionsChanged();
    });
    connect(&m_rpc, &RpcClient::connectionsUnavailable, this,
            [this](const QString& error, bool retryable) {
                if (m_shuttingDown || !m_online || m_state != Running)
                    return;
                m_connectionsReady = false;
                m_connectionsError = error;
                m_rpc.stopConnections();
                if (retryable)
                    m_connectionRetry.start();
                emit connectionsChanged();
            });
    m_startupDeadline.setSingleShot(true);
    m_startupDeadline.setInterval(15000);
    m_shutdownDeadline.setSingleShot(true);
    m_shutdownDeadline.setInterval(3000);
    connect(&m_shutdownDeadline, &QTimer::timeout, this, &Controller::stopCoreForShutdown);
    connect(&m_startupDeadline, &QTimer::timeout, this, [this] {
        lost(tr("Rostra did not become ready within 15 seconds. Choose Retry to try again."),
             false);
        m_core.stop();
    });
    connect(&m_core, &DaemonProcess::started, this, [this](const QString& socket) {
        if (m_shuttingDown)
            return;
        m_socket = socket;
        m_startupDeadline.start();
        retry();
    });
    connect(&m_core, &DaemonProcess::unavailable, this,
            [this](const QString& error, bool retryable) {
                m_startupDeadline.stop();
                lost(error, retryable);
            });
    connect(&m_core, &DaemonProcess::stopped, this, [this] {
        if (m_shuttingDown)
            finishShutdown();
        else if (m_reconfigure) {
            m_reconfigure = false;
            retry();
        }
    });
    connect(&m_worker, &WorkerProcess::stopped, this, &Controller::finishShutdown);
    connect(&m_worker, &WorkerProcess::ready, this, [this](const QString& socket) {
        if (m_shuttingDown)
            return;
        m_rpc.setWorkerSocket(socket);
        m_workerStatus = tr("Ready");
        processValidation();
        emit changed();
    });
    connect(&m_worker, &WorkerProcess::unavailable, this, [this](const QString& error) {
        if (m_shuttingDown)
            return;
        m_rpc.setWorkerSocket({});
        m_workerStatus = error;
        if (m_validationId) {
            m_profiles.setValidation(m_validationProfile, m_validationContent, Profile::Pending);
            m_validationQueue.prepend(m_validationProfile);
            m_validationId = 0;
        }
        if (m_operation == Validate || m_operation == ValidateForStart ||
            m_operation == ValidateForRestart) {
            m_operation = None;
            m_pendingId = 0;
        }
        announce(error, true);
    });
    connect(&m_rpc, &RpcClient::daemonReady, this, [this](const QString& version) {
        if (m_shuttingDown)
            return;
        m_startupDeadline.stop();
        m_online = true;
        m_version = version;
        m_retrySeconds = 1;
        announce(tr("Ready."));
    });
    connect(&m_rpc, &RpcClient::daemonUnavailable, this, &Controller::lost);
    connect(&m_rpc, &RpcClient::serviceStatus, this, [this](int state, const QString& error) {
        if (m_shuttingDown)
            return;
        if (state < Idle || state > Failed) {
            lost(tr("Unknown service state from daemon."), false);
            return;
        }
        const auto old = m_state;
        m_state = State(state);
        if (m_state != Running || old != Running)
            resetTraffic();
        if (m_state != Running)
            resetGroups();
        else if (old != Running) {
            refreshGroups();
            refreshConnections();
        }
        if (m_state == Running && m_activeName.isEmpty())
            m_activeName = tr("Existing daemon session");
        if (m_state == Idle || m_state == Failed) {
            m_activeName.clear();
            m_activeId.clear();
            m_activeContent.clear();
        }
        if (!error.isEmpty())
            announce(error, true);
        else
            emit changed();
    });
    connect(&m_rpc, &RpcClient::traffic, this, [this](TrafficSample sample) {
        if (m_state != Running || m_shuttingDown)
            return;
        const auto elapsed = m_sampleTimer.isValid() ? m_sampleTimer.elapsed() : 0;
        if (elapsed > 0 && sample.available && m_traffic.available &&
            sample.uploadTotal >= m_traffic.uploadTotal &&
            sample.downloadTotal >= m_traffic.downloadTotal) {
            m_uploadRate = (sample.uploadTotal - m_traffic.uploadTotal) * 1000.0 / elapsed;
            m_downloadRate = (sample.downloadTotal - m_traffic.downloadTotal) * 1000.0 / elapsed;
        } else {
            m_uploadRate = 0;
            m_downloadRate = 0;
        }
        m_sampleTimer.restart();
        const auto cpu = m_processCpu.sample(m_core.processId());
        sample.cpuPercent = cpu.value_or(0);
        sample.cpuAvailable = cpu.has_value();
        m_traffic = sample;
        emit trafficChanged();
    });
    connect(&m_rpc, &RpcClient::logs, &m_logs, &LogModel::append);
    connect(&m_rpc, &RpcClient::groups, this, [this](OutboundGroups groups) {
        if (m_shuttingDown || !m_online || m_state != Running)
            return;
        m_groupsReady = true;
        m_groupsError.clear();
        for (auto it = m_delayTests.begin(); it != m_delayTests.end();) {
            bool found = false, finished = false, failed = false;
            for (const auto& group : groups) {
                if (group.tag != it->group)
                    continue;
                for (const auto& member : group.members)
                    if (member.tag == it.key()) {
                        found = true;
                        if (member.testTime != it->previousTime ||
                            member.delay != it->previousDelay) {
                            finished = true;
                            failed = failed || member.testTime <= 0 || member.delay <= 0;
                        }
                    }
            }
            if (!found || finished) {
                if (found && failed)
                    m_delayErrors.insert(
                        it.key(),
                        tr("Rostra could not measure this outbound’s delay. Click to try again."));
                else
                    m_delayErrors.remove(it.key());
                it = m_delayTests.erase(it);
            } else
                ++it;
        }
        m_groups.replace(std::move(groups));
        emit changed();
    });
    connect(&m_rpc, &RpcClient::groupsUnavailable, this,
            [this](const QString& error, bool retryable) {
                if (m_shuttingDown)
                    return;
                resetGroups();
                m_groupsError = error;
                if (retryable && m_online && m_state == Running)
                    m_groupRetry.start();
                emit changed();
            });
    connect(&m_rpc, &RpcClient::operationFinished, this, &Controller::completed);
    m_coreUpdater = std::make_unique<CoreUpdater>(this, directory);
}

Controller::~Controller() {
    m_shuttingDown = true;
    m_coreUpdater.reset();
    if (m_download) {
        disconnect(m_download, nullptr, this, nullptr);
        m_download->abort();
    }
    m_retry.stop();
    m_groupRetry.stop();
    m_startupDeadline.stop();
    m_shutdownDeadline.stop();
    disconnect(&m_worker, nullptr, this, nullptr);
    disconnect(&m_core, nullptr, this, nullptr);
    m_rpc.disconnectDaemon();
    m_rpc.setWorkerSocket({});
    m_worker.stop();
    m_core.stopAndWait();
}

void Controller::initialize() {
    if (m_initialized || m_shuttingDown)
        return;
    m_initialized = true;
    if (!m_profiles.load(&m_profileError))
        announce(m_profileError, true);
    if (!selectedProfile()) {
        m_selected = m_profiles.at(0) ? m_profiles.at(0)->id : QString{};
    }
    for (int row = 0; row < m_profiles.rowCount(); ++row)
        queueValidation(m_profiles.at(row)->id);
    emit profileChanged();
    m_workerStatus = tr("Starting…");
    m_worker.start(m_executable);
    retry();
}

void Controller::configure(const QString& executable, const QString& socket) {
    if (!canConfigure())
        return;
    m_executable = executable;
    if (!m_managed)
        m_socket = socket;
    m_settings.setValue("backend/executable", executable);
    m_settings.remove("backend/socket");
    m_settings.sync();
    m_rpc.setWorkerSocket({});
    m_validationId = 0;
    m_validationQueue.clear();
    for (int row = 0; row < m_profiles.rowCount(); ++row) {
        const auto* profile = m_profiles.at(row);
        m_profiles.setValidation(profile->id, profile->content, Profile::Pending);
        m_validationQueue.append(profile->id);
    }
    m_workerStatus = tr("Starting…");
    m_worker.start(executable);
    m_retrySeconds = 1;
    m_core.resetAttempts();
    if (m_managed) {
        m_retry.stop();
        m_startupDeadline.stop();
        m_rpc.disconnectDaemon();
        m_online = false;
        m_state = Unavailable;
        m_reconfigure = true;
        m_core.stop();
    } else
        retry();
}

void Controller::retryNow() {
    if (m_shuttingDown || busy())
        return;
    m_core.resetAttempts();
    if (!m_worker.running()) {
        m_workerStatus = tr("Starting…");
        m_worker.start(m_executable);
    }
    retry();
}

void Controller::retry() {
    if (m_shuttingDown || m_reconfigure)
        return;
    if (m_operation != None) {
        m_retry.start(1000);
        return;
    }
    m_retry.stop();
    m_online = false;
    m_state = Unavailable;
    m_version.clear();
    m_activeName.clear();
    m_activeId.clear();
    m_activeContent.clear();
    resetTraffic();
    resetGroups();
    announce(tr("Preparing connection…"));
    if (m_managed) {
        if (!m_core.running()) {
            m_rpc.disconnectDaemon();
            m_core.start(m_executable);
            return;
        }
        m_socket = m_core.socket();
    }
    m_rpc.connectDaemon(m_socket);
}

void Controller::lost(const QString& message, bool retryable) {
    if (m_shuttingDown) {
        stopCoreForShutdown();
        return;
    }
    m_rpc.disconnectDaemon();
    m_online = false;
    m_state = Unavailable;
    m_version.clear();
    m_activeName.clear();
    m_activeId.clear();
    m_activeContent.clear();
    // Cancel the pending intent; late validation must never connect a different session.
    if (m_operation != Validate) {
        m_operation = None;
        m_pendingId = 0;
    }
    resetTraffic();
    resetGroups();
    if (m_managed && m_core.running() && m_startupDeadline.isActive() && retryable)
        announce(tr("Preparing connection…"));
    else
        announce(tr("Connection unavailable: %1").arg(message), true);
    if (retryable) {
        m_retry.start(m_retrySeconds * 1000);
        m_retrySeconds = std::min(m_retrySeconds * 2, 30);
    }
}

void Controller::shutdown() {
    if (m_shuttingDown)
        return;
    const bool needsStop =
        m_online && (m_state == Running || m_state == Starting || m_state == Stopping ||
                     m_state == Failed || m_operation == Start);
    m_shuttingDown = true;
    m_coreUpdater->cancel();
    if (m_download)
        m_download->abort();
    m_validationQueue.clear();
    m_validationId = 0;
    m_reconfigure = false;
    m_operation = None;
    m_pendingId = 0;
    m_retry.stop();
    m_startupDeadline.stop();
    resetGroups();
    resetTraffic();
    m_rpc.setWorkerSocket({});
    m_worker.shutdown();
    announce(tr("Stopping the connection and quitting…"));
    if (needsStop) {
        m_shutdownId = ++m_nextId;
        m_shutdownDeadline.start();
        m_rpc.stopService(m_shutdownId);
    } else
        stopCoreForShutdown();
}

void Controller::stopCoreForShutdown() {
    if (!m_shuttingDown || m_shutdownCoreRequested)
        return;
    m_shutdownCoreRequested = true;
    m_shutdownDeadline.stop();
    m_rpc.disconnectDaemon();
    m_online = false;
    m_state = Unavailable;
    m_core.stop();
    finishShutdown();
}

void Controller::finishShutdown() {
    if (!m_shuttingDown || !m_shutdownCoreRequested || m_shutdownEmitted || m_core.running() ||
        m_worker.running())
        return;
    m_shutdownEmitted = true;
    emit shutdownFinished();
}

void Controller::announce(QString text, bool error) {
    m_message = std::move(text);
    m_error = error;
    notify(m_message, error);
    emit changed();
}

void Controller::notify(const QString& message, bool error) {
    m_logs.appendApplication(message, error);
    if (error && !message.isEmpty())
        emit notification(message, true);
}

void Controller::saveDashboardLayout(const QVariantList& layout) {
    m_settings.setValue("home/widgets", layout);
    m_settings.sync();
    if (m_settings.status() != QSettings::NoError)
        notify(tr("Could not save the Home layout."), true);
}

void Controller::saveConnectionsHeader(const QByteArray& state) {
    m_settings.setValue("connections/header", state);
    m_settings.sync();
    if (m_settings.status() != QSettings::NoError)
        notify(tr("Could not save the Connections column layout."), true);
}

void Controller::saveLogRetention(int lines) {
    m_logs.setRetentionLimit(lines);
    m_settings.setValue("logs/retention", m_logs.retentionLimit());
    m_settings.sync();
    if (m_settings.status() != QSettings::NoError)
        notify(tr("Could not save log retention settings."), true);
}

void Controller::resetConnections() {
    m_connectionRetry.stop();
    m_rpc.stopConnections();
    m_connectionsReady = false;
    m_connectionsError.clear();
    m_closeRequest = 0;
    m_connections.replace({});
    emit connectionsChanged();
}

void Controller::refreshConnections() {
    if (m_shuttingDown || !m_online || m_state != Running)
        return;
    m_connectionRetry.stop();
    m_connectionsReady = false;
    m_rpc.subscribeConnections();
    emit connectionsChanged();
}

void Controller::closeConnection(const QString& id) {
    const auto* connection = m_connections.find(id);
    if (!canCloseConnections() || !connection || !connection->active())
        return;
    m_closeRequest = ++m_nextId;
    m_rpc.closeConnection(m_closeRequest, id);
    emit connectionsChanged();
}

void Controller::closeAllConnections() {
    if (!canCloseConnections() || m_connections.activeCount() == 0)
        return;
    m_closeRequest = ++m_nextId;
    m_rpc.closeAllConnections(m_closeRequest);
    emit connectionsChanged();
}

bool Controller::importProfile(const QString& file) {
    if (busy())
        return false;
    QString id, error;
    if (!m_profiles.importFile(file, &id, &error)) {
        announce(error, true);
        return false;
    }
    imported(id);
    return true;
}

void Controller::imported(const QString& id) {
    announce(tr("Profile imported. Checking configuration…"));
    queueValidation(id);
    if (!busy())
        selectProfile(id);
}

void Controller::importProfileUrl(const QString& value) {
    downloadProfile(value);
}

void Controller::updateProfile(const QString& id) {
    const auto* profile = m_profiles.find(id);
    if (!profile || profile->url.isEmpty() || busy() || importing())
        return;
    downloadProfile(profile->url, id);
}

void Controller::downloadProfile(const QString& value, const QString& id) {
    if (m_shuttingDown || importing())
        return;
    const auto url = value.trimmed();
    if (url.isEmpty() || !ProfileStore::validUrl(url)) {
        announce(tr("Enter a valid HTTP or HTTPS URL."), true);
        return;
    }
    const auto original = id.isEmpty() ? Profile{} : *m_profiles.find(id);
    m_downloadProfile = id;
    QNetworkRequest request{QUrl(url)};
    request.setTransferTimeout(15000);
    request.setMaximumRedirectsAllowed(5);
    auto* reply = m_network.get(request);
    m_download = reply;
    auto content = std::make_shared<QByteArray>();
    auto tooLarge = std::make_shared<bool>(false);
    reply->setReadBufferSize(ProfileStore::MaxConfigBytes + 1);
    auto read = [reply, content, tooLarge] {
        if (*tooLarge)
            return;
        content->append(reply->read(ProfileStore::MaxConfigBytes + 1 - content->size()));
        if (content->size() > ProfileStore::MaxConfigBytes ||
            reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() >
                ProfileStore::MaxConfigBytes) {
            *tooLarge = true;
            reply->abort();
        }
    };
    connect(reply, &QNetworkReply::readyRead, this, read);
    QTimer::singleShot(20000, reply, [reply] {
        if (!reply->isFinished())
            reply->abort();
    });
    connect(
        reply, &QNetworkReply::finished, this,
        [this, reply, content, tooLarge, url, read, original] {
            if (reply->isOpen())
                read();
            m_download.clear();
            m_downloadProfile.clear();
            reply->deleteLater();
            if (m_shuttingDown)
                return;
            if (*tooLarge) {
                announce(tr("The downloaded profile exceeds 2 MiB."), true);
                return;
            }
            if (reply->error() != QNetworkReply::NoError) {
                announce(tr("Could not download the profile: %1").arg(reply->errorString()), true);
                return;
            }
            if (!original.id.isEmpty()) {
                const auto* current = m_profiles.find(original.id);
                if (!current || current->content != original.content ||
                    current->url != original.url || current->name != original.name) {
                    announce(tr("The profile changed while downloading. Update again to use the "
                                "new content."),
                             true);
                    return;
                }
                if (busy()) {
                    announce(tr("The connection is changing. Update the profile again when it is "
                                "ready."),
                             true);
                    return;
                }
                if (QString::fromUtf8(*content).toUtf8() != *content || content->contains('\0')) {
                    announce(tr("The downloaded profile is not UTF-8 text."), true);
                    return;
                }
                saveProfile(original.id, original.name, original.url, QString::fromUtf8(*content));
                return;
            }
            auto name = QFileInfo(QUrl(url).path()).fileName();
            if (name.isEmpty())
                name = QUrl(url).host();
            QString id, error;
            if (!m_profiles.add(name, *content, url, &id, &error))
                announce(error, true);
            else
                imported(id);
        });
    announce(id.isEmpty() ? tr("Downloading profile…") : tr("Updating %1…").arg(original.name));
}

bool Controller::saveProfile(const QString& id, const QString& name, const QString& url,
                             const QString& content) {
    if (busy())
        return false;
    QString error;
    if (!m_profiles.update(id, name, url.trimmed(), content, &error)) {
        announce(error, true);
        return false;
    }
    if (id == m_activeId && content == m_activeContent)
        m_activeName = name.trimmed();
    emit profileChanged();
    announce(tr("Profile saved. Checking configuration…"));
    queueValidation(id);
    if (id == m_selected && id == m_activeId && m_state == Running && content != m_activeContent)
        beginValidation(true, true);
    return true;
}

void Controller::deleteProfile(const QString& id) {
    if (busy() || !m_profiles.find(id))
        return;
    if (id == m_activeId && m_state == Running) {
        m_deleteId = id;
        m_operation = Delete;
        m_pendingId = ++m_nextId;
        announce(tr("Disconnecting before deleting the profile…"));
        m_rpc.stopService(m_pendingId);
    } else
        removeProfile(id);
}

void Controller::removeProfile(const QString& id) {
    QString error;
    if (!m_profiles.remove(id, &error)) {
        announce(error, true);
        return;
    }
    m_validationQueue.removeAll(id);
    if (m_selected == id) {
        m_selected = m_profiles.at(0) ? m_profiles.at(0)->id : QString{};
        m_settings.setValue("profile/selected", m_selected);
    }
    emit profileChanged();
    announce(tr("Profile deleted."));
}

void Controller::selectProfile(const QString& id) {
    if (busy() || id == m_selected || !m_profiles.find(id))
        return;
    m_selected = id;
    m_settings.setValue("profile/selected", id);
    emit profileChanged();
    emit changed();
    if (m_online && m_state == Running)
        beginValidation(true, true);
}

bool Controller::canValidate() const {
    return selectedProfile() && m_worker.isReady() && !busy();
}

bool Controller::canConnect() const {
    return canValidate() && m_online && (m_state == Idle || m_state == Failed);
}

bool Controller::canDisconnect() const {
    return m_online && !busy() && (m_state == Running || m_state == Failed);
}

void Controller::validateProfile() {
    if (canValidate())
        beginValidation(false);
}

void Controller::connectService() {
    if (canConnect())
        beginValidation(true);
}

void Controller::queueValidation(const QString& id) {
    const auto* profile = m_profiles.find(id);
    if (!profile || m_shuttingDown)
        return;
    if (m_validationId && m_validationProfile == id && m_validationContent == profile->content)
        return;
    if (!m_validationQueue.contains(id))
        m_validationQueue.append(id);
    m_profiles.setValidation(id, profile->content, Profile::Pending);
    QTimer::singleShot(0, this, &Controller::processValidation);
}

void Controller::processValidation() {
    if (m_shuttingDown || m_validationId || !m_worker.isReady())
        return;
    while (!m_validationQueue.isEmpty()) {
        const auto id = m_validationQueue.takeFirst();
        const auto* profile = m_profiles.find(id);
        if (!profile)
            continue;
        m_validationProfile = id;
        m_validationContent = profile->content;
        m_validationId = ++m_nextId;
        m_profiles.setValidation(id, m_validationContent, Profile::Checking);
        if (m_validationContent.trimmed().isEmpty()) {
            const auto requestId = m_validationId;
            QTimer::singleShot(0, this, [this, requestId] {
                completed(requestId, false, tr("The configuration is empty."), false);
            });
        } else
            m_rpc.checkConfig(m_validationId, m_validationContent);
        break;
    }
}

void Controller::beginValidation(bool start, bool restart) {
    const auto* profile = selectedProfile();
    m_pendingContent = profile->content;
    m_pendingName = profile->name;
    m_pendingProfile = profile->id;
    if (!m_worker.isReady()) {
        announce(tr("Cannot check this profile yet: %1").arg(m_workerStatus), true);
        return;
    }
    m_operation = restart ? ValidateForRestart : start ? ValidateForStart : Validate;
    m_pendingId = ++m_nextId;
    announce(tr("Checking %1…").arg(profile->name));
    queueValidation(profile->id);
    // Prioritize an explicit connection request over other background checks.
    if (m_validationQueue.removeAll(profile->id))
        m_validationQueue.prepend(profile->id);
}

void Controller::disconnectService() {
    if (!canDisconnect())
        return;
    m_operation = Stop;
    m_pendingId = ++m_nextId;
    announce(tr("Disconnecting…"));
    m_rpc.stopService(m_pendingId);
}

void Controller::resetGroups() {
    m_delayTests.clear();
    m_delayErrors.clear();
    m_groupRetry.stop();
    m_rpc.stopGroups();
    m_groupsReady = false;
    m_groupsError.clear();
    if (m_groups.rowCount())
        m_groups.replace({});
}

void Controller::refreshGroups() {
    if (m_shuttingDown || !m_online || m_state != Running)
        return;
    m_groupRetry.stop();
    m_groupsReady = false;
    m_groupsError.clear();
    m_rpc.subscribeGroups();
    emit changed();
}

bool Controller::canSelectOutbound(const QString& groupTag, const QString& outbound) const {
    if (!m_online || m_state != Running || busy() || !m_groupsReady)
        return false;
    const auto* group = m_groups.find(groupTag);
    if (!group || !group->selectable || group->type == "loadbalance" || group->selected == outbound)
        return false;
    return std::any_of(group->members.begin(), group->members.end(),
                       [&](const auto& member) { return member.tag == outbound; });
}

void Controller::selectOutbound(const QString& group, const QString& outbound) {
    if (!canSelectOutbound(group, outbound))
        return;
    m_operation = Select;
    m_selectionGroup = group;
    m_selectionOutbound = outbound;
    m_pendingId = ++m_nextId;
    announce(tr("Switching %1 to %2…").arg(group, outbound));
    m_rpc.selectOutbound(m_pendingId, group, outbound);
}

bool Controller::canTestDelay(const QString& groupTag, const QString& outbound) const {
    if (!m_online || m_state != Running || busy() || !m_groupsReady ||
        m_delayTests.contains(outbound))
        return false;
    const auto* group = m_groups.find(groupTag);
    if (!group)
        return false;
    const auto target = group->type == "loadbalance" ? groupTag : outbound;
    for (const auto& test : m_delayTests)
        if (test.target == target)
            return false;
    return std::any_of(group->members.begin(), group->members.end(),
                       [&](const auto& member) { return member.tag == outbound; });
}

void Controller::testDelay(const QString& groupTag, const QString& outbound) {
    if (!canTestDelay(groupTag, outbound))
        return;
    const auto* group = m_groups.find(groupTag);
    for (const auto& member : group->members) {
        if (member.tag != outbound)
            continue;
        const auto id = ++m_nextId;
        // Load-balance cards expose group health, rather than URL-test history.
        // Rostra refreshes that health through URLTest on the group itself.
        const auto target = group->type == "loadbalance" ? groupTag : outbound;
        m_delayTests.insert(outbound, {id, groupTag, target, member.testTime, member.delay});
        m_delayErrors.remove(outbound);
        // URLTest acknowledges scheduling, not completion. The group stream
        // supplies measured results. Never report the acknowledgement as a ping.
        m_rpc.testDelay(id, target);
        QTimer::singleShot(15000, this, [this, outbound, id] {
            const auto it = m_delayTests.find(outbound);
            if (it == m_delayTests.end() || it->id != id)
                return;
            m_delayTests.erase(it);
            m_delayErrors.insert(outbound,
                                 tr("No new delay result was received. Click to try again."));
            emit changed();
        });
        emit changed();
        return;
    }
}

bool Controller::connectionChanging() const {
    return m_shuttingDown || m_state == Starting || m_state == Stopping ||
           m_operation == ValidateForStart || m_operation == ValidateForRestart ||
           m_operation == Start || m_operation == Stop || m_operation == Delete ||
           (!m_online && !m_error);
}

void Controller::completed(quint64 id, bool success, const QString& error, bool uncertain) {
    if (id == m_closeRequest && m_closeRequest) {
        m_closeRequest = 0;
        if (!success)
            notify(uncertain ? tr("Could not confirm closing the connection: %1").arg(error)
                             : tr("Could not close the connection: %1").arg(error),
                   true);
        else
            notify(tr("Connection close request accepted."));
        // Keep rows until the stream confirms closure; never replay an uncertain close.
        emit connectionsChanged();
        return;
    }
    if (m_shuttingDown) {
        if (id == m_shutdownId)
            stopCoreForShutdown();
        return;
    }
    for (auto it = m_delayTests.begin(); it != m_delayTests.end(); ++it) {
        if (it->id != id)
            continue;
        if (!success) {
            m_delayErrors.insert(it.key(), tr("Delay test failed: %1").arg(error));
            m_delayTests.erase(it);
            emit changed();
        }
        return;
    }
    if (id == m_validationId) {
        const auto profileId = m_validationProfile;
        const auto content = m_validationContent;
        m_validationId = 0;
        m_profiles.setValidation(profileId, content, success ? Profile::Valid : Profile::Invalid,
                                 error);
        const bool awaited = (m_operation == Validate || m_operation == ValidateForStart ||
                              m_operation == ValidateForRestart) &&
                             m_pendingProfile == profileId && m_pendingContent == content;
        if (awaited)
            completed(m_pendingId, success, error, false);
        else if (!success && m_profiles.find(profileId) &&
                 m_profiles.find(profileId)->content == content)
            announce(tr("%1 failed its check: %2").arg(m_profiles.find(profileId)->name, error),
                     true);
        QTimer::singleShot(0, this, &Controller::processValidation);
        return;
    }
    if (id != m_pendingId || m_operation == None)
        return;
    const auto operation = m_operation;
    m_operation = None;
    m_pendingId = 0;
    if (!success) {
        if (uncertain && (operation == Start || operation == Stop || operation == Select ||
                          operation == Delete)) {
            lost(tr("The operation could not be confirmed: %1. Reconnecting to check its result.")
                     .arg(error),
                 true);
        } else
            announce(tr("%1 failed: %2")
                         .arg(operation == Validate || operation == ValidateForStart ||
                                      operation == ValidateForRestart
                                  ? tr("Validation")
                              : operation == Select ? tr("Selection")
                                                    : tr("Operation"),
                              error),
                     true);
        return;
    }
    if (operation == ValidateForStart || operation == ValidateForRestart) {
        if (!m_online ||
            (operation == ValidateForRestart ? m_state != Running
                                             : (m_state != Idle && m_state != Failed))) {
            announce(
                tr("Service state changed during validation. Review its status before connecting."),
                true);
            return;
        }
        m_operation = Start;
        m_pendingId = ++m_nextId;
        resetGroups();
        announce((operation == ValidateForRestart ? tr("Switching to %1…") : tr("Starting %1…"))
                     .arg(m_pendingName));
        m_rpc.startService(m_pendingId, m_pendingContent);
    } else if (operation == Start) {
        // Fast reloads may coalesce STARTING/STOPPING out of the status stream.
        // A successful start still begins a fresh traffic/chart baseline.
        resetTraffic();
        m_activeName = m_pendingName;
        m_activeId = m_pendingProfile;
        m_activeContent = m_pendingContent;
        if (m_state == Idle || m_state == Failed)
            m_state = Starting;
        // A fast reload can coalesce STOPPING/STARTING into a single Running
        // status update. Always subscribe to the new instance after activation.
        if (m_state == Running) {
            refreshGroups();
            refreshConnections();
        }
        announce({});
    } else if (operation == Stop || operation == Delete) {
        if (m_state != Idle)
            m_state = Stopping;
        m_activeName.clear();
        m_activeId.clear();
        m_activeContent.clear();
        resetGroups();
        if (operation == Delete)
            removeProfile(m_deleteId);
        else
            announce({});
    } else if (operation == Select) {
        // Only the live group snapshot changes the displayed selection.
        announce(tr("Outbound selection accepted."));
    } else
        announce(tr("Configuration is valid."));
}

void Controller::resetTraffic() {
    resetConnections();
    m_traffic = {};
    m_uploadRate = 0;
    m_downloadRate = 0;
    m_sampleTimer.invalidate();
    m_processCpu.reset();
    emit trafficChanged();
}

QString Controller::stateText() const {
    if (m_shuttingDown)
        return tr("Quitting…");
    if (m_operation == ValidateForStart || m_operation == ValidateForRestart ||
        m_operation == Validate)
        return tr("Validating");
    if (m_operation == Start)
        return tr("Starting");
    if (m_operation == Stop || m_operation == Delete)
        return tr("Stopping");
    switch (m_state) {
    case Idle:
        return tr("Disconnected");
    case Starting:
        return tr("Starting");
    case Running:
        return tr("Connected");
    case Stopping:
        return tr("Stopping");
    case Failed:
        return tr("Service error");
    case Unavailable:
        return m_online  ? tr("Reading status")
               : m_error ? tr("Connection unavailable")
                         : tr("Preparing…");
    }
    return {};
}
