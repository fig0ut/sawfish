#pragma once
#include "daemon/managed_service.grpc.pb.h"
#include "daemon/started_service.grpc.pb.h"
#include "experimental/boxdd/desktop_service.grpc.pb.h"
#include <QSet>
#include <QString>
#include <QThread>
#include <atomic>
#include <condition_variable>
#include <grpcpp/grpcpp.h>
#include <mutex>

using Empty = google::protobuf::Empty;
using Status = grpc::Status;
using Context = grpc::ServerContext;

inline rostra_rpc::ConnectionEvents sampleConnections() {
    rostra_rpc::ConnectionEvents batch;
    batch.set_reset(true);
    for (int i = 0; i < 3; ++i) {
        auto* event = batch.add_events();
        event->set_id(i == 0 ? "browser" : i == 1 ? "terminal" : "closed");
        auto* c = event->mutable_connection();
        c->set_id(event->id());
        c->set_domain(i == 0   ? "docs.example.org"
                      : i == 1 ? "api.example.org"
                               : "archive.example.org");
        c->set_source("127.0.0.1:42000");
        c->set_destination("192.0.2.1:443");
        c->set_network("tcp");
        c->set_protocol("tls");
        c->set_inbound("mixed");
        c->set_outbound("direct-a");
        c->add_chainlist("proxy");
        c->add_chainlist("direct-a");
        c->set_rule("match => proxy");
        c->set_createdat(1792880000000LL + i * 1000);
        c->set_uplinktotal(1024);
        c->set_downlinktotal(4096);
        c->mutable_processinfo()->set_processpath(i == 0 ? "/usr/bin/firefox" : "/usr/bin/curl");
        c->mutable_processinfo()->set_processid(1200 + i);
        if (i == 2)
            c->set_closedat(1792880005000LL);
    }
    return batch;
}

inline rostra_rpc::Groups sampleGroups() {
    rostra_rpc::Groups groups;
    for (const auto& type : {"selector", "urltest", "loadbalance"}) {
        auto* group = groups.add_group();
        group->set_tag(type == std::string("selector") ? "proxy" : type);
        group->set_type(type);
        group->set_selectable(type == std::string("selector"));
        group->set_selected("direct-a");
        for (const auto& tag : {"direct-a", "direct-b"}) {
            auto* member = group->add_items();
            member->set_tag(tag);
            member->set_type("direct");
            member->set_urltesttime(1700000000);
            member->set_urltestdelay(25);
        }
    }
    return groups;
}

struct FakeState {
    std::mutex mutex;
    std::condition_variable changed;
    int state = 0, revision = 0;
    std::atomic<int> api{4}, claims{0}, starts{0}, stops{0}, checks{0}, activeStreams{0};
    std::atomic<bool> otherOwner{false}, unavailable{false}, failStreams{false};
    std::atomic<bool> uncertainStart{false}, holdCheck{false};
    std::atomic<int> selections{0}, groupSubscriptions{0}, delayTests{0};
    std::atomic<bool> rejectDelay{false}, holdDelay{false};
    QString lastDelayOutbound;
    std::atomic<bool> rejectSelection{false}, uncertainSelection{false}, holdSelection{false};
    std::atomic<bool> failGroups{false}, unsupportedGroups{false};
    rostra_rpc::Groups groups = sampleGroups();
    int groupRevision = 0;
    QString lastGroup, lastOutbound;
    QString lastConfig;
    qint64 uploaded = 100, downloaded = 0;
    int trafficConnections = 0, trafficRevision = 0;
    void setTraffic(qint64 upload, qint64 download, int connections) {
        std::lock_guard<std::mutex> lock(mutex);
        uploaded = upload;
        downloaded = download;
        trafficConnections = connections;
        ++trafficRevision;
        changed.notify_all();
    }
    std::vector<rostra_rpc::ConnectionEvents> connectionBatches;
    QSet<QString> activeConnections;
    std::atomic<int> connectionSubscriptions{0}, connectionCloses{0}, allConnectionCloses{0};
    std::atomic<bool> failConnections{false}, unsupportedConnections{false}, rejectClose{false};
    QString lastClosedConnection;
    void sendConnections(rostra_rpc::ConnectionEvents value) {
        std::lock_guard<std::mutex> lock(mutex);
        if (value.reset())
            activeConnections.clear();
        for (const auto& event : value.events()) {
            const auto id = QString::fromStdString(event.id());
            if (event.type() == rostra_rpc::CONNECTION_EVENT_CLOSED ||
                (event.has_connection() && event.connection().closedat()))
                activeConnections.remove(id);
            else if (event.has_connection())
                activeConnections.insert(id);
        }
        connectionBatches.push_back(std::move(value));
        changed.notify_all();
    }
    void setGroups(rostra_rpc::Groups value) {
        std::lock_guard<std::mutex> lock(mutex);
        groups = std::move(value);
        ++groupRevision;
        changed.notify_all();
    }
    void setState(int value) {
        std::lock_guard<std::mutex> lock(mutex);
        state = value;
        ++revision;
        changed.notify_all();
    }
    void awaitCancellation(Context* ctx) {
        std::unique_lock<std::mutex> lock(mutex);
        while (!ctx->IsCancelled() && !failStreams)
            changed.wait_for(lock, std::chrono::milliseconds(20));
    }
};
class FakeDesktop : public desktop::DesktopService::Service {
  public:
    FakeState& s;
    explicit FakeDesktop(FakeState& state) : s(state) {}
    Status GetDaemonInfo(Context*, const Empty*, desktop::DaemonInfo* info) override {
        if (s.unavailable)
            return {grpc::StatusCode::UNAVAILABLE, "offline"};
        info->set_version("test");
        info->set_ownership(s.otherOwner ? desktop::DAEMON_OWNERSHIP_OTHER
                                         : desktop::DAEMON_OWNERSHIP_AVAILABLE);
        return Status::OK;
    }
    Status ClaimService(Context*, const Empty*, Empty*) override {
        ++s.claims;
        return Status::OK;
    }
    Status StartService(Context*, const desktop::StartServiceRequest* request, Empty*) override {
        ++s.starts;
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            s.lastConfig = QString::fromStdString(request->config_content());
        }
        s.setState(2);
        if (s.uncertainStart)
            return {grpc::StatusCode::UNAVAILABLE, "response lost after start"};
        return Status::OK;
    }
};
class FakeManaged : public rostra_rpc::ManagedService::Service {
  public:
    FakeState& s;
    explicit FakeManaged(FakeState& state) : s(state) {}
    Status StopService(Context*, const Empty*, Empty*) override {
        ++s.stops;
        s.setState(0);
        return Status::OK;
    }
};
class FakeApplication : public desktop::ApplicationService::Service {
  public:
    FakeState& s;
    explicit FakeApplication(FakeState& state) : s(state) {}
    Status CheckConfig(Context* ctx, const desktop::ConfigContent* request, Empty*) override {
        ++s.checks;
        while (s.holdCheck && !ctx->IsCancelled())
            QThread::msleep(10);
        if (request->content() == "invalid")
            return {grpc::StatusCode::INVALID_ARGUMENT, "invalid configuration"};
        return Status::OK;
    }
};
class FakeStarted : public rostra_rpc::StartedService::Service {
  public:
    FakeState& s;
    explicit FakeStarted(FakeState& state) : s(state) {}
    Status SubscribeConnections(Context* ctx, const rostra_rpc::SubscribeConnectionsRequest*,
                                grpc::ServerWriter<rostra_rpc::ConnectionEvents>* writer) override {
        ++s.connectionSubscriptions;
        if (s.unsupportedConnections)
            return {grpc::StatusCode::UNIMPLEMENTED, "tracking unsupported"};
        ++s.activeStreams;
        rostra_rpc::ConnectionEvents initial;
        initial.set_reset(true);
        writer->Write(initial);
        size_t next = 0;
        while (!ctx->IsCancelled() && !s.failStreams && !s.failConnections) {
            rostra_rpc::ConnectionEvents batch;
            {
                std::unique_lock<std::mutex> lock(s.mutex);
                if (next >= s.connectionBatches.size()) {
                    s.changed.wait_for(lock, std::chrono::milliseconds(20));
                    continue;
                }
                batch = s.connectionBatches[next++];
            }
            if (!writer->Write(batch))
                break;
        }
        --s.activeStreams;
        return {grpc::StatusCode::UNAVAILABLE, "connection stream interrupted"};
    }
    Status CloseConnection(Context*, const rostra_rpc::CloseConnectionRequest* request,
                           Empty*) override {
        ++s.connectionCloses;
        if (s.rejectClose)
            return {grpc::StatusCode::PERMISSION_DENIED, "close denied"};
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            s.lastClosedConnection = QString::fromStdString(request->id());
        }
        rostra_rpc::ConnectionEvents batch;
        auto* event = batch.add_events();
        event->set_id(request->id());
        event->set_type(rostra_rpc::CONNECTION_EVENT_CLOSED);
        event->set_closedat(1800000001000LL);
        s.sendConnections(batch);
        return Status::OK;
    }
    Status CloseAllConnections(Context*, const Empty*, Empty*) override {
        ++s.allConnectionCloses;
        if (s.rejectClose)
            return {grpc::StatusCode::PERMISSION_DENIED, "close denied"};
        rostra_rpc::ConnectionEvents batch;
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            for (const auto& id : s.activeConnections) {
                auto* event = batch.add_events();
                event->set_id(id.toStdString());
                event->set_type(rostra_rpc::CONNECTION_EVENT_CLOSED);
                event->set_closedat(1800000001000LL);
            }
        }
        s.sendConnections(batch);
        return Status::OK;
    }
    Status GetVersion(Context*, const Empty*, rostra_rpc::Version* version) override {
        version->set_version("test");
        version->set_apiversion(s.api);
        return Status::OK;
    }
    Status SubscribeGroups(Context* ctx, const Empty*,
                           grpc::ServerWriter<rostra_rpc::Groups>* writer) override {
        ++s.groupSubscriptions;
        if (s.unsupportedGroups)
            return {grpc::StatusCode::UNIMPLEMENTED, "groups unsupported"};
        ++s.activeStreams;
        int revision = -1;
        while (!ctx->IsCancelled() && !s.failStreams && !s.failGroups) {
            rostra_rpc::Groups value;
            {
                std::unique_lock<std::mutex> lock(s.mutex);
                if (revision == s.groupRevision) {
                    s.changed.wait_for(lock, std::chrono::milliseconds(20));
                    continue;
                }
                revision = s.groupRevision;
                value = s.groups;
            }
            if (!writer->Write(value))
                break;
        }
        --s.activeStreams;
        return {grpc::StatusCode::UNAVAILABLE, "group stream interrupted"};
    }
    Status URLTest(Context* ctx, const rostra_rpc::URLTestRequest* request, Empty*) override {
        ++s.delayTests;
        while (s.holdDelay && !ctx->IsCancelled())
            QThread::msleep(10);
        if (ctx->IsCancelled())
            return {grpc::StatusCode::CANCELLED, "cancelled"};
        std::lock_guard<std::mutex> lock(s.mutex);
        s.lastDelayOutbound = QString::fromStdString(request->outboundtag());
        if (s.rejectDelay)
            return {grpc::StatusCode::NOT_FOUND, "outbound was removed"};
        return Status::OK; // Actual measurements arrive later through SubscribeGroups.
    }
    Status SelectOutbound(Context* ctx, const rostra_rpc::SelectOutboundRequest* request,
                          Empty*) override {
        ++s.selections;
        while (s.holdSelection && !ctx->IsCancelled())
            QThread::msleep(10);
        if (ctx->IsCancelled())
            return {grpc::StatusCode::CANCELLED, "cancelled"};
        std::lock_guard<std::mutex> lock(s.mutex);
        s.lastGroup = QString::fromStdString(request->grouptag());
        s.lastOutbound = QString::fromStdString(request->outboundtag());
        if (s.rejectSelection)
            return {grpc::StatusCode::NOT_FOUND, "outbound was removed"};
        for (auto& group : *s.groups.mutable_group()) {
            if (group.tag() != request->grouptag())
                continue;
            group.set_selected(request->outboundtag());
            ++s.groupRevision;
            s.changed.notify_all();
            if (s.uncertainSelection)
                return {grpc::StatusCode::UNAVAILABLE, "response lost after selection"};
            return Status::OK;
        }
        return {grpc::StatusCode::NOT_FOUND, "selector not found"};
    }
    Status SubscribeServiceStatus(Context* ctx, const Empty*,
                                  grpc::ServerWriter<rostra_rpc::ServiceStatus>* writer) override {
        ++s.activeStreams;
        int revision = -1;
        while (!ctx->IsCancelled() && !s.failStreams) {
            rostra_rpc::ServiceStatus value;
            {
                std::unique_lock<std::mutex> lock(s.mutex);
                if (revision == s.revision) {
                    s.changed.wait_for(lock, std::chrono::milliseconds(20));
                    continue;
                }
                revision = s.revision;
                value.set_status(rostra_rpc::ServiceStatus_Type(s.state));
            }
            if (!writer->Write(value))
                break;
        }
        --s.activeStreams;
        return {grpc::StatusCode::UNAVAILABLE, "stream interrupted"};
    }
    Status SubscribeStatus(Context* ctx, const rostra_rpc::SubscribeStatusRequest*,
                           grpc::ServerWriter<rostra_rpc::Status>* writer) override {
        ++s.activeStreams;
        int revision = -1;
        while (!ctx->IsCancelled() && !s.failStreams) {
            rostra_rpc::Status value;
            {
                std::unique_lock<std::mutex> lock(s.mutex);
                if (revision == s.trafficRevision) {
                    s.changed.wait_for(lock, std::chrono::milliseconds(20));
                    continue;
                }
                revision = s.trafficRevision;
                value.set_trafficavailable(true);
                value.set_uplinktotal(s.uploaded);
                value.set_downlinktotal(s.downloaded);
                value.set_connectionsin(s.trafficConnections);
                value.set_memory(32 * 1024 * 1024);
            }
            if (!writer->Write(value))
                break;
        }
        --s.activeStreams;
        return {grpc::StatusCode::UNAVAILABLE, "stream interrupted"};
    }
    Status SubscribeLog(Context* ctx, const Empty*,
                        grpc::ServerWriter<rostra_rpc::Log>* writer) override {
        ++s.activeStreams;
        rostra_rpc::Log value;
        value.set_reset(true);
        auto* line = value.add_messages();
        line->set_level(rostra_rpc::INFO);
        line->set_message("daemon log");
        writer->Write(value);
        s.awaitCancellation(ctx);
        --s.activeStreams;
        return {grpc::StatusCode::UNAVAILABLE, "stream interrupted"};
    }
};
