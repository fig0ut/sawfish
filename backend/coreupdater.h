#pragma once
#include <QCryptographicHash>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QSaveFile>
#include <QSettings>
#include <QTimer>
#include <functional>
#include <memory>

class Controller;
class QNetworkReply;

struct CoreRelease {
    QString version, digest;
    QUrl url;
    qint64 size = 0;
    bool valid() const {
        return !version.isEmpty();
    }
};

class CoreUpdater : public QObject {
    Q_OBJECT
  public:
    enum Channel { Stable, Prerelease };
    enum State { Idle, Checking, Available, Downloading, Verifying, Installing, Failed };
    CoreUpdater(Controller* controller, QString directory,
                QUrl releases = QUrl("https://api.github.com/repos/fig0ut/rostra/releases"));
    ~CoreUpdater() override;
    Channel channel() const {
        return m_channel;
    }
    void setChannel(Channel channel);
    State state() const {
        return m_state;
    }
    bool busy() const {
        return m_state == Checking || m_state == Downloading || m_state == Verifying ||
               m_state == Installing;
    }
    QString status() const {
        return m_status;
    }
    QString availableVersion() const {
        return m_release.version;
    }
    int progress() const {
        return m_progress;
    }
    void check();
    void install();
    void cancel();
    static QString platformAsset();
    static CoreRelease parseRelease(const QJsonObject& release, Channel channel,
                                    const QString& assetName, bool allowLocalTest = false);
    static void recoverInterrupted(QSettings& settings);
  signals:
    void changed();

  private:
    enum Phase { None, Info, Config, Stop, Ready, Resume, Rollback };
    void fetchPage(int page);
    void fetch(const QUrl& url, qint64 limit, std::function<void(QByteArray)> complete);
    void probe();
    void probeFinished(int code, QProcess::ExitStatus status);
    void activate();
    void advance();
    void fail(const QString& message);
    void finish(bool success);
    void command(const std::function<void()>& action);
    void abortWork();
    void setState(State state, const QString& message);
    Controller* m_controller;
    QString m_directory;
    QSettings m_settings;
    QUrl m_releases;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    std::unique_ptr<QSaveFile> m_file;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    QProcess m_probe;
    QTimer m_deadline;
    QByteArray m_output;
    CoreRelease m_release;
    Channel m_channel = Stable;
    State m_state = Idle;
    Phase m_phase = None;
    QString m_status, m_candidate, m_previous, m_config, m_failure, m_profileName, m_profileId,
        m_releasePublished;
    qint64 m_received = 0;
    int m_progress = 0;
    bool m_resume = false, m_queued = false, m_activated = false, m_localTest = false;
};
