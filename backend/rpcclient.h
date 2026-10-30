#pragma once
#include "connectionstate.h"
#include "groupstate.h"
#include <QObject>
#include <QThreadPool>
#include <QTimer>
#include <functional>
#include <memory>

namespace grpc {
class Channel;
}

struct LogLine {
    int level = 4;
    QString message;
    bool application = false; // Local origin metadata; never sent over RPC.
};
using LogBatch = QList<LogLine>;
Q_DECLARE_METATYPE(LogBatch)
struct TrafficSample {
    bool available = false;
    qint64 uploadTotal = 0;
    qint64 downloadTotal = 0;
    int connections = 0;
    quint64 memory = 0;
    double cpuPercent = 0;
    bool cpuAvailable = false;
};
Q_DECLARE_METATYPE(TrafficSample)

// RPCs run on a private pool. A bounded mailbox delivers snapshots on the GUI thread.
class RpcClient : public QObject {
    Q_OBJECT
  public:
    using ChannelFactory = std::function<std::shared_ptr<grpc::Channel>(const QString& socket)>;
    explicit RpcClient(QObject* parent = nullptr, ChannelFactory channelFactory = {});
    ~RpcClient() override;
    void connectDaemon(const QString& socket);
    void disconnectDaemon();
    void setWorkerSocket(const QString& socket);
    void checkConfig(quint64 id, const QString& content);
    void startService(quint64 id, const QString& content);
    void stopService(quint64 id);
    void subscribeGroups();
    void stopGroups();
    void selectOutbound(quint64 id, const QString& group, const QString& outbound);
    void testDelay(quint64 id, const QString& outbound);
    void subscribeConnections();
    void stopConnections();
    void closeConnection(quint64 id, const QString& connectionId);
    void closeAllConnections(quint64 id);
  signals:
    void daemonReady(QString version);
    void daemonUnavailable(QString message, bool retryable);
    void serviceStatus(int state, QString error);
    void traffic(TrafficSample sample);
    void logs(LogBatch lines, bool reset);
    void groups(OutboundGroups groups);
    void groupsUnavailable(QString message, bool retryable);
    void connections(ConnectionList snapshot);
    void connectionsUnavailable(QString message, bool retryable);
    void operationFinished(quint64 id, bool success, QString error, bool uncertain);

  private:
    struct Session;
    struct Mailbox;
    using Action = std::function<void(std::shared_ptr<Session>, std::shared_ptr<Mailbox>)>;
    void runAction(const std::shared_ptr<Session>& session, quint64 id, Action action);
    void drain();
    std::shared_ptr<Session> m_daemon, m_worker, m_groups, m_connections;
    std::shared_ptr<Mailbox> m_daemonMail, m_workerMail, m_groupMail, m_connectionMail;
    QThreadPool m_pool;
    QTimer m_drainTimer;
    ChannelFactory m_channelFactory;
};
