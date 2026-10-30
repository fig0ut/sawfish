#pragma once
#include "backend/daemonprocess.h"
#include "backend/processcpu.h"
#include "backend/rpcclient.h"
#include "backend/workerprocess.h"
#include "models/connectionmodel.h"
#include "models/groupmodel.h"
#include "models/logmodel.h"
#include "models/profilestore.h"
#include <QElapsedTimer>
#include <QHash>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QSettings>
class QNetworkReply;
class CoreUpdater;

class Controller : public QObject {
    Q_OBJECT
  public:
    enum State { Unavailable = -1, Idle = 0, Starting = 1, Running = 2, Stopping = 3, Failed = 4 };
    Q_ENUM(State)
    // The application leaves socket empty to own its core. Explicit endpoints are
    // retained for transport/integration tests, without adopting process ownership.
    Controller(QString dataDirectory, QString executable, QString socket, QObject* parent = nullptr,
               RpcClient::ChannelFactory channelFactory = {});
    ~Controller() override;
    void initialize();
    void configure(const QString& executable, const QString& socket);
    CoreUpdater* coreUpdater() const {
        return m_coreUpdater.get();
    }
    void retry();
    void retryNow();
    void shutdown();
    // UI notices share the toast/log channel without changing connection state.
    // Only errors produce a toast; informational notices still go to Logs.
    void notify(const QString& message, bool error = false);
    QVariantList dashboardLayout() const {
        return m_settings.value("home/widgets").toList();
    }
    bool hasDashboardLayout() const {
        return m_settings.contains("home/widgets");
    }
    void saveDashboardLayout(const QVariantList& layout);
    QByteArray connectionsHeader() const {
        return m_settings.value("connections/header").toByteArray();
    }
    void saveConnectionsHeader(const QByteArray& state);
    void saveLogRetention(int lines);
    ConnectionModel* connectionModel() {
        return &m_connections;
    }
    bool connectionsReady() const {
        return m_connectionsReady;
    }
    QString connectionsError() const {
        return m_connectionsError;
    }
    bool canCloseConnections() const {
        return m_online && m_state == Running && m_connectionsReady && !busy() && !m_closeRequest;
    }
    void refreshConnections();
    void closeConnection(const QString& id);
    void closeAllConnections();
    bool shuttingDown() const {
        return m_shuttingDown;
    }
    bool managedBackend() const {
        return m_managed;
    }
    qint64 coreProcessId() const {
        return m_core.processId();
    }
    bool canConfigure() const {
        return !busy() && m_state != Running && m_state != Starting && m_state != Stopping;
    }
    bool importProfile(const QString& file);
    void importProfileUrl(const QString& url);
    void updateProfile(const QString& id);
    bool updatingProfile(const QString& id) const {
        return importing() && m_downloadProfile == id;
    }
    bool importing() const {
        return !m_download.isNull();
    }
    bool saveProfile(const QString& id, const QString& name, const QString& url,
                     const QString& content);
    void deleteProfile(const QString& id);
    QString activeId() const {
        return m_activeId;
    }
    void selectProfile(const QString& id);
    void validateProfile();
    void connectService();
    void disconnectService();
    void refreshGroups();
    void testDelay(const QString& group, const QString& outbound);
    bool canTestDelay(const QString& group, const QString& outbound) const;
    bool testingDelay(const QString& outbound) const {
        return m_delayTests.contains(outbound);
    }
    QString delayError(const QString& outbound) const {
        return m_delayErrors.value(outbound);
    }
    bool connectionChanging() const;
    void selectOutbound(const QString& group, const QString& outbound);
    bool canSelectOutbound(const QString& group, const QString& outbound) const;
    bool selectingOutbound(const QString& group, const QString& outbound) const {
        return m_operation == Select && m_selectionGroup == group &&
               m_selectionOutbound == outbound;
    }
    GroupModel* groupModel() {
        return &m_groups;
    }
    bool groupsReady() const {
        return m_groupsReady;
    }
    QString groupsError() const {
        return m_groupsError;
    }
    bool canConnect() const;
    bool canDisconnect() const;
    bool canValidate() const;
    bool busy() const {
        return m_operation != None || m_shuttingDown || m_coreUpdating;
    }
    bool online() const {
        return m_online;
    }
    State state() const {
        return m_state;
    }
    QString stateText() const;
    QString message() const {
        return m_message;
    }
    bool messageIsError() const {
        return m_error;
    }
    QString workerStatus() const {
        return m_workerStatus;
    }
    QString profileError() const {
        return m_profileError;
    }
    QString daemonVersion() const {
        return m_version;
    }
    QString executable() const {
        return m_executable;
    }
    QString socket() const {
        return m_socket;
    }
    QString selectedId() const {
        return m_selected;
    }
    const Profile* selectedProfile() const {
        return m_profiles.find(m_selected);
    }
    QString activeName() const {
        return m_activeName;
    }
    ProfileStore* profiles() {
        return &m_profiles;
    }
    LogModel* logModel() {
        return &m_logs;
    }
    TrafficSample trafficSample() const {
        return m_traffic;
    }
    double uploadRate() const {
        return m_uploadRate;
    }
    double downloadRate() const {
        return m_downloadRate;
    }
  signals:
    void notification(const QString& message, bool error);
    void changed();
    void profileChanged();
    void trafficChanged();
    void connectionsChanged();
    void shutdownFinished();

  private:
    friend class CoreUpdater;
    std::unique_ptr<CoreUpdater> m_coreUpdater;
    bool m_coreUpdating = false;
    enum Operation {
        None,
        Validate,
        ValidateForStart,
        ValidateForRestart,
        Start,
        Stop,
        Select,
        Delete
    };
    void beginValidation(bool start, bool restart = false);
    void queueValidation(const QString& id);
    void processValidation();
    void imported(const QString& id);
    void downloadProfile(const QString& url, const QString& id = {});
    void removeProfile(const QString& id);
    void completed(quint64 id, bool success, const QString& error, bool uncertain);
    void announce(QString text, bool error = false);
    void lost(const QString& message, bool retryable);
    void resetTraffic();
    void resetGroups();
    void resetConnections();
    void stopCoreForShutdown();
    void finishShutdown();
    RpcClient m_rpc;
    WorkerProcess m_worker;
    DaemonProcess m_core;
    ProfileStore m_profiles;
    LogModel m_logs;
    GroupModel m_groups;
    ConnectionModel m_connections;
    QSettings m_settings;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_download;
    QString m_downloadProfile;
    struct DelayTest {
        quint64 id;
        QString group, target;
        qint64 previousTime;
        int previousDelay;
    };
    QHash<QString, DelayTest> m_delayTests;
    QHash<QString, QString> m_delayErrors;
    QTimer m_retry;
    QTimer m_groupRetry;
    QTimer m_connectionRetry;
    bool m_connectionsReady = false;
    QString m_connectionsError;
    quint64 m_closeRequest = 0;
    QTimer m_startupDeadline, m_shutdownDeadline;
    QElapsedTimer m_sampleTimer;
    ProcessCpu m_processCpu;
    QString m_executable, m_socket, m_selected, m_activeName;
    QString m_activeId, m_activeContent, m_pendingProfile, m_deleteId;
    QStringList m_validationQueue;
    QString m_validationProfile, m_validationContent;
    quint64 m_validationId = 0;
    QString m_message, m_workerStatus, m_version, m_pendingContent, m_pendingName, m_profileError;
    QString m_groupsError;
    QString m_selectionGroup, m_selectionOutbound;
    State m_state = Unavailable;
    Operation m_operation = None;
    TrafficSample m_traffic;
    double m_uploadRate = 0, m_downloadRate = 0;
    quint64 m_nextId = 0, m_pendingId = 0;
    bool m_online = false, m_error = false, m_initialized = false;
    bool m_groupsReady = false;
    bool m_managed = false, m_shuttingDown = false, m_shutdownCoreRequested = false;
    bool m_shutdownEmitted = false, m_reconfigure = false;
    quint64 m_shutdownId = 0;
    int m_retrySeconds = 1;
};
