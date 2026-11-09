#pragma once
#include "proxyprofile.h"
#include <QJsonObject>
#include <QStringList>
#include <functional>

// Runs only in the helper, never on the GUI thread. The injectable command
// runner lets tests exercise rollback without changing the user's desktop.
class ProxySettings {
  public:
    using Runner = std::function<bool(const QString&, const QStringList&, QString*, QString*)>;
    explicit ProxySettings(QString journal, Runner runner = {});
    bool detect(QString* error);
    bool apply(const ProxyEndpoint& endpoint, QString* error);
    bool restore(QString* error);
    bool active() const;
    QString desktop() const {
        return m_desktop;
    }

  private:
    bool configure(const QString& desktop, QString* error);
    QJsonObject desired(const ProxyEndpoint& endpoint) const;
    bool read(const QString& key, QString* value, QString* error);
    bool write(const QString& key, const QString& value, QString* error);
    bool reload(QString* error);
    QStringList orderedKeys(const QJsonObject& values, bool restoring) const;
    QString m_journal, m_desktop, m_read, m_write;
    Runner m_run;
};

// Internal, unprivileged child. stdin EOF means the GUI exited (including SIGKILL).
int runSystemProxyHelper();
