#include "rpcclient.h"
#include "daemon/managed_service.grpc.pb.h"
#include "daemon/started_service.grpc.pb.h"
#include "experimental/boxdd/desktop_service.grpc.pb.h"
#include <QDir>
#include <QFileInfo>
#include <algorithm>
#include <chrono>
#include <deque>
#include <grpcpp/grpcpp.h>
#include <mutex>
#include <optional>
#include <utility>

namespace {
constexpr int SupportedApi = 4;
constexpr size_t MaxPendingLogs = 3000;
constexpr qsizetype MaxMessageLength = 16384;
constexpr qsizetype MaxPendingCharacters = 2 * 1024 * 1024;
QString errorText(const grpc::Status& s) {
    return QString::fromStdString(s.error_message());
}
bool isUncertain(const grpc::Status& s) {
    return s.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED ||
           s.error_code() == grpc::StatusCode::UNAVAILABLE ||
           s.error_code() == grpc::StatusCode::CANCELLED;
}
} // namespace

struct RpcClient::Session {
    std::shared_ptr<grpc::Channel> channel;
    std::mutex mutex;
    bool cancelled = false;
    std::vector<std::weak_ptr<grpc::ClientContext>> contexts;
    explicit Session(std::shared_ptr<grpc::Channel> existing) : channel(std::move(existing)) {}
    explicit Session(const QString& socket, const ChannelFactory& factory) {
        if (factory) {
            channel = factory(socket);
            return;
        }
        grpc::ChannelArguments args;
        args.SetMaxReceiveMessageSize(4 * 1024 * 1024);
        channel =
            grpc::CreateCustomChannel("unix:" + QDir::fromNativeSeparators(socket).toStdString(),
                                      grpc::InsecureChannelCredentials(), args);
    }
    std::shared_ptr<grpc::ClientContext> context(int seconds = 0) {
        auto result = std::make_shared<grpc::ClientContext>();
        if (seconds)
            result->set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(seconds));
        std::lock_guard<std::mutex> guard(mutex);
        contexts.erase(std::remove_if(contexts.begin(), contexts.end(),
                                      [](const auto& entry) { return entry.expired(); }),
                       contexts.end());
        contexts.push_back(result);
        if (cancelled)
            result->TryCancel();
        return result;
    }
    void cancel() {
        std::lock_guard<std::mutex> guard(mutex);
        cancelled = true;
        for (const auto& entry : contexts)
            if (auto ctx = entry.lock())
                ctx->TryCancel();
    }
};

struct RpcClient::Mailbox {
    struct Event {
        enum Kind { Ready, Lost, Finished } kind;
        QString text;
        quint64 id = 0;
        bool success = false, uncertain = false, retryable = true;
    };
    std::mutex mutex;
    std::deque<Event> events;
    std::optional<std::pair<int, QString>> status;
    std::optional<TrafficSample> traffic;
    std::optional<OutboundGroups> groups;
    std::optional<ConnectionList> connections;
    std::deque<LogLine> logs;
    qsizetype characters = 0;
    bool resetLogs = false, lost = false;
    void event(Event value) {
        std::lock_guard<std::mutex> guard(mutex);
        events.push_back(std::move(value));
    }
    void failure(const grpc::Status& result) {
        std::lock_guard<std::mutex> guard(mutex);
        if (lost)
            return;
        lost = true;
        const bool retryable = result.error_code() != grpc::StatusCode::PERMISSION_DENIED &&
                               result.error_code() != grpc::StatusCode::UNAUTHENTICATED &&
                               result.error_code() != grpc::StatusCode::UNIMPLEMENTED;
        events.push_back(
            {Event::Lost,
             result.ok() ? QStringLiteral("The daemon closed the stream.") : errorText(result), 0,
             false, false, retryable});
    }
    void finish(quint64 id, const grpc::Status& result) {
        event({Event::Finished, errorText(result), id, result.ok(), isUncertain(result)});
    }
};

RpcClient::RpcClient(QObject* parent, ChannelFactory factory)
    : QObject(parent), m_channelFactory(std::move(factory)) {
    m_pool.setMaxThreadCount(8);
    m_drainTimer.setInterval(50);
    connect(&m_drainTimer, &QTimer::timeout, this, &RpcClient::drain);
    m_drainTimer.start();
}

RpcClient::~RpcClient() {
    m_drainTimer.stop();
    stopGroups();
    stopConnections();
    if (m_daemon)
        m_daemon->cancel();
    if (m_worker)
        m_worker->cancel();
    m_pool.waitForDone();
}

void RpcClient::disconnectDaemon() {
    stopGroups();
    stopConnections();
    if (m_daemon)
        m_daemon->cancel();
    m_daemon.reset();
    m_daemonMail.reset();
}

void RpcClient::connectDaemon(const QString& socket) {
    disconnectDaemon();
    if (!QFileInfo(socket).isAbsolute() || socket.toUtf8().size() > 100) {
        emit daemonUnavailable(tr("Choose an absolute Unix socket path shorter than 101 bytes."),
                               false);
        return;
    }
    auto session = m_daemon = std::make_shared<Session>(socket, m_channelFactory);
    auto mail = m_daemonMail = std::make_shared<Mailbox>();
    auto* pool = &m_pool;
    m_pool.start([session, mail, pool] {
        auto desktopClient = desktop::DesktopService::NewStub(session->channel);
        auto started = rostra_rpc::StartedService::NewStub(session->channel);
        google::protobuf::Empty empty, response;
        desktop::DaemonInfo info;
        auto ctx = session->context(3);
        auto result = desktopClient->GetDaemonInfo(ctx.get(), empty, &info);
        if (!result.ok()) {
            mail->failure(result);
            return;
        }
        if (info.ownership() == desktop::DAEMON_OWNERSHIP_OTHER) {
            mail->event({Mailbox::Event::Lost,
                         QStringLiteral("The daemon belongs to another user. Takeover is not "
                                        "supported in this preview."),
                         0, false, false, false});
            return;
        }
        if (info.ownership() != desktop::DAEMON_OWNERSHIP_AVAILABLE &&
            info.ownership() != desktop::DAEMON_OWNERSHIP_CALLER) {
            mail->event({Mailbox::Event::Lost,
                         QStringLiteral("The daemon returned an unknown ownership state."), 0,
                         false, false, false});
            return;
        }
        ctx = session->context(3);
        result = desktopClient->ClaimService(ctx.get(), empty, &response);
        if (!result.ok()) {
            mail->failure(result);
            return;
        }
        rostra_rpc::Version version;
        ctx = session->context(3);
        result = started->GetVersion(ctx.get(), empty, &version);
        if (!result.ok()) {
            mail->failure(result);
            return;
        }
        if (version.apiversion() != SupportedApi) {
            mail->event({Mailbox::Event::Lost,
                         QStringLiteral("Unsupported daemon API %1 (this client expects %2).")
                             .arg(version.apiversion())
                             .arg(SupportedApi),
                         0, false, false, false});
            return;
        }
        mail->event({Mailbox::Event::Ready, QString::fromStdString(version.version())});
        pool->start([session, mail] {
            auto client = rostra_rpc::StartedService::NewStub(session->channel);
            auto context = session->context();
            auto reader = client->SubscribeServiceStatus(context.get(), google::protobuf::Empty{});
            rostra_rpc::ServiceStatus value;
            while (reader->Read(&value)) {
                std::lock_guard<std::mutex> guard(mail->mutex);
                mail->status = {int(value.status()), QString::fromStdString(value.errormessage())};
            }
            mail->failure(reader->Finish());
        });
        pool->start([session, mail] {
            auto client = rostra_rpc::StartedService::NewStub(session->channel);
            auto context = session->context();
            rostra_rpc::SubscribeStatusRequest request;
            request.set_interval(1000000000LL);
            auto reader = client->SubscribeStatus(context.get(), request);
            rostra_rpc::Status value;
            while (reader->Read(&value)) {
                std::lock_guard<std::mutex> guard(mail->mutex);
                mail->traffic =
                    TrafficSample{value.trafficavailable(), value.uplinktotal(),
                                  value.downlinktotal(), value.connectionsin(), value.memory()};
            }
            mail->failure(reader->Finish());
        });
        pool->start([session, mail] {
            auto client = rostra_rpc::StartedService::NewStub(session->channel);
            auto context = session->context();
            auto reader = client->SubscribeLog(context.get(), google::protobuf::Empty{});
            rostra_rpc::Log value;
            while (reader->Read(&value)) {
                std::lock_guard<std::mutex> guard(mail->mutex);
                if (value.reset()) {
                    mail->logs.clear();
                    mail->characters = 0;
                    mail->resetLogs = true;
                }
                for (const auto& line : value.messages()) {
                    auto message = QString::fromStdString(line.message()).left(MaxMessageLength);
                    mail->characters += message.size();
                    mail->logs.push_back({int(line.level()), std::move(message)});
                    while (mail->logs.size() > MaxPendingLogs ||
                           mail->characters > MaxPendingCharacters) {
                        mail->characters -= mail->logs.front().message.size();
                        mail->logs.pop_front();
                    }
                }
            }
            mail->failure(reader->Finish());
        });
    });
}

void RpcClient::setWorkerSocket(const QString& socket) {
    if (m_worker)
        m_worker->cancel();
    m_worker.reset();
    m_workerMail.reset();
    if (!socket.isEmpty()) {
        m_worker = std::make_shared<Session>(socket, m_channelFactory);
        m_workerMail = std::make_shared<Mailbox>();
    }
}

void RpcClient::runAction(const std::shared_ptr<Session>& session, quint64 id, Action action) {
    auto mail = session == m_worker ? m_workerMail : m_daemonMail;
    if (!session || !mail) {
        QTimer::singleShot(0, this, [this, id] {
            emit operationFinished(id, false, tr("The backend is not connected."), false);
        });
        return;
    }
    m_pool.start([session, mail, action = std::move(action)] { action(session, mail); });
}

void RpcClient::checkConfig(quint64 id, const QString& content) {
    runAction(m_worker, id, [id, content](auto session, auto mail) {
        auto client = desktop::ApplicationService::NewStub(session->channel);
        auto context = session->context(30);
        desktop::ConfigContent request;
        request.set_content(content.toStdString());
        google::protobuf::Empty response;
        mail->finish(id, client->CheckConfig(context.get(), request, &response));
    });
}

void RpcClient::startService(quint64 id, const QString& content) {
    runAction(m_daemon, id, [id, content](auto session, auto mail) {
        auto client = desktop::DesktopService::NewStub(session->channel);
        auto context = session->context(45);
        desktop::StartServiceRequest request;
        request.set_config_content(content.toStdString());
        google::protobuf::Empty response;
        mail->finish(id, client->StartService(context.get(), request, &response));
    });
}

void RpcClient::stopService(quint64 id) {
    runAction(m_daemon, id, [id](auto session, auto mail) {
        auto client = rostra_rpc::ManagedService::NewStub(session->channel);
        auto context = session->context(15);
        google::protobuf::Empty request, response;
        mail->finish(id, client->StopService(context.get(), request, &response));
    });
}

void RpcClient::stopGroups() {
    if (m_groups)
        m_groups->cancel();
    m_groups.reset();
    m_groupMail.reset();
}

void RpcClient::subscribeGroups() {
    stopGroups();
    if (!m_daemon)
        return;
    // This API rejects idle services. The controller subscribes only while running.
    auto session = m_groups = std::make_shared<Session>(m_daemon->channel);
    auto mail = m_groupMail = std::make_shared<Mailbox>();
    m_pool.start([session, mail] {
        auto client = rostra_rpc::StartedService::NewStub(session->channel);
        auto context = session->context();
        auto reader = client->SubscribeGroups(context.get(), google::protobuf::Empty{});
        rostra_rpc::Groups value;
        while (reader->Read(&value)) {
            OutboundGroups snapshot;
            for (const auto& group : value.group()) {
                OutboundGroup entry;
                entry.tag = QString::fromStdString(group.tag());
                entry.type = QString::fromStdString(group.type());
                entry.selectable = group.selectable() && entry.type != "loadbalance";
                // Load balancing chooses an outbound per connection, not one active member.
                if (entry.type != "loadbalance")
                    entry.selected = QString::fromStdString(group.selected());
                for (const auto& item : group.items())
                    entry.members.append({QString::fromStdString(item.tag()),
                                          QString::fromStdString(item.type()), item.urltesttime(),
                                          item.urltestdelay()});
                snapshot.append(std::move(entry));
            }
            std::lock_guard<std::mutex> guard(mail->mutex);
            mail->groups = std::move(snapshot);
        }
        mail->failure(reader->Finish());
    });
}

void RpcClient::selectOutbound(quint64 id, const QString& group, const QString& outbound) {
    runAction(m_daemon, id, [id, group, outbound](auto session, auto mail) {
        auto client = rostra_rpc::StartedService::NewStub(session->channel);
        auto context = session->context(10);
        rostra_rpc::SelectOutboundRequest request;
        request.set_grouptag(group.toStdString());
        request.set_outboundtag(outbound.toStdString());
        google::protobuf::Empty response;
        mail->finish(id, client->SelectOutbound(context.get(), request, &response));
    });
}

void RpcClient::testDelay(quint64 id, const QString& outbound) {
    runAction(m_daemon, id, [id, outbound](auto session, auto mail) {
        auto client = rostra_rpc::StartedService::NewStub(session->channel);
        auto context = session->context(10);
        rostra_rpc::URLTestRequest request;
        request.set_outboundtag(outbound.toStdString());
        google::protobuf::Empty response;
        mail->finish(id, client->URLTest(context.get(), request, &response));
    });
}

void RpcClient::stopConnections() {
    if (m_connections)
        m_connections->cancel();
    m_connections.reset();
    m_connectionMail.reset();
}

void RpcClient::subscribeConnections() {
    stopConnections();
    if (!m_daemon)
        return;
    auto session = m_connections = std::make_shared<Session>(m_daemon->channel);
    auto mail = m_connectionMail = std::make_shared<Mailbox>();
    m_pool.start([session, mail] {
        auto client = rostra_rpc::StartedService::NewStub(session->channel);
        auto context = session->context();
        rostra_rpc::SubscribeConnectionsRequest request;
        request.set_interval(1000000000LL);
        auto reader = client->SubscribeConnections(context.get(), request);
        rostra_rpc::ConnectionEvents events;
        ConnectionHistory history;
        while (reader->Read(&events)) {
            history.apply(events);
            auto snapshot = history.snapshot();
            std::lock_guard<std::mutex> guard(mail->mutex);
            mail->connections = std::move(snapshot);
        }
        mail->failure(reader->Finish());
    });
}

void RpcClient::closeConnection(quint64 id, const QString& connectionId) {
    runAction(m_daemon, id, [id, connectionId](auto session, auto mail) {
        auto client = rostra_rpc::StartedService::NewStub(session->channel);
        auto context = session->context(10);
        rostra_rpc::CloseConnectionRequest request;
        request.set_id(connectionId.toStdString());
        google::protobuf::Empty response;
        mail->finish(id, client->CloseConnection(context.get(), request, &response));
    });
}

void RpcClient::closeAllConnections(quint64 id) {
    runAction(m_daemon, id, [id](auto session, auto mail) {
        auto client = rostra_rpc::StartedService::NewStub(session->channel);
        auto context = session->context(10);
        google::protobuf::Empty request, response;
        mail->finish(id, client->CloseAllConnections(context.get(), request, &response));
    });
}

void RpcClient::drain() {
    // Copy both pointers: direct signal handlers may reconnect and replace them.
    const auto mailboxes = {m_daemonMail, m_workerMail, m_groupMail, m_connectionMail};
    for (const auto& mail : mailboxes) {
        if (!mail)
            continue;
        std::deque<Mailbox::Event> events;
        std::optional<std::pair<int, QString>> status;
        std::optional<TrafficSample> trafficValue;
        std::optional<OutboundGroups> groupsValue;
        std::optional<ConnectionList> connectionsValue;
        LogBatch lines;
        bool reset;
        {
            std::lock_guard<std::mutex> guard(mail->mutex);
            events.swap(mail->events);
            status.swap(mail->status);
            trafficValue.swap(mail->traffic);
            groupsValue.swap(mail->groups);
            connectionsValue.swap(mail->connections);
            for (auto& line : mail->logs)
                lines.push_back(std::move(line));
            mail->logs.clear();
            mail->characters = 0;
            reset = std::exchange(mail->resetLogs, false);
        }
        const bool daemonMail = mail == m_daemonMail;
        for (const auto& event : events) {
            if (mail != m_daemonMail && mail != m_workerMail && mail != m_groupMail &&
                mail != m_connectionMail)
                break;
            switch (event.kind) {
            case Mailbox::Event::Ready:
                emit daemonReady(event.text);
                break;
            case Mailbox::Event::Lost:
                if (mail == m_groupMail)
                    emit groupsUnavailable(event.text, event.retryable);
                else if (mail == m_connectionMail)
                    emit connectionsUnavailable(event.text, event.retryable);
                else
                    emit daemonUnavailable(event.text, event.retryable);
                break;
            case Mailbox::Event::Finished:
                emit operationFinished(event.id, event.success, event.text, event.uncertain);
                break;
            }
        }
        if (mail == m_groupMail && groupsValue)
            emit groups(std::move(*groupsValue));
        if (mail == m_connectionMail && connectionsValue)
            emit connections(std::move(*connectionsValue));
        if (!daemonMail || mail != m_daemonMail)
            continue;
        if (status)
            emit serviceStatus(status->first, status->second);
        if (mail != m_daemonMail)
            continue;
        if (trafficValue)
            emit traffic(*trafficValue);
        if (mail != m_daemonMail)
            continue;
        if (reset || !lines.isEmpty())
            emit logs(std::move(lines), reset);
    }
}
