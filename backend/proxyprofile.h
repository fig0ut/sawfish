#pragma once
#include <QJsonObject>
#include <QString>

struct ProxyEndpoint {
    QString host;
    int port = 0;
    bool http = false, socks = false;
    bool valid() const;
    QJsonObject json() const;
    static ProxyEndpoint fromJson(const QJsonObject& object);
};

struct ProxyProfile {
    QString content;
    ProxyEndpoint endpoint;
    QString error;
};

// Produces an in-memory configuration. The imported profile is never rewritten.
ProxyProfile prepareProxyProfile(const QString& content);
