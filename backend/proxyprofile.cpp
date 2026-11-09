#include "proxyprofile.h"
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>

bool ProxyEndpoint::valid() const {
    return QHostAddress(host).isLoopback() && port > 0 && port <= 65535 && (http || socks);
}

QJsonObject ProxyEndpoint::json() const {
    return {{"host", host}, {"port", port}, {"http", http}, {"socks", socks}};
}

ProxyEndpoint ProxyEndpoint::fromJson(const QJsonObject& object) {
    return {object["host"].toString(), object["port"].toInt(), object["http"].toBool(),
            object["socks"].toBool()};
}

namespace {
// Rostra accepts JSON comments and trailing commas. Blank comments without
// changing string contents (URLs, escaped quotes, and comment-like passwords).
QByteArray jsonWithoutComments(QByteArray data) {
    bool quoted = false, escaped = false;
    for (qsizetype i = 0; i < data.size(); ++i) {
        const char ch = data[i];
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (ch == '\\')
                escaped = true;
            else if (ch == '"')
                quoted = false;
        } else if (ch == '"') {
            quoted = true;
        } else if (ch == '/' && i + 1 < data.size()) {
            if (data[i + 1] == '/') {
                while (i < data.size() && data[i] != '\n')
                    data[i++] = ' ';
            } else if (data[i + 1] == '*') {
                data[i++] = ' ';
                data[i++] = ' ';
                while (i + 1 < data.size() && !(data[i] == '*' && data[i + 1] == '/'))
                    data[i++] = ' ';
                if (i + 1 >= data.size())
                    return {}; // Unterminated comment.
                data[i++] = ' ';
                data[i] = ' ';
            }
        }
    }
    quoted = escaped = false;
    for (qsizetype i = 0; i < data.size(); ++i) {
        const char ch = data[i];
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (ch == '\\')
                escaped = true;
            else if (ch == '"')
                quoted = false;
        } else if (ch == '"') {
            quoted = true;
        } else if (ch == ',') {
            auto next = i + 1;
            while (next < data.size() && QByteArray(" \t\r\n").contains(data[next]))
                ++next;
            if (next < data.size() && (data[next] == ']' || data[next] == '}'))
                data[i] = ' ';
        }
    }
    return data;
}
} // namespace

ProxyProfile prepareProxyProfile(const QString& content) {
    ProxyProfile result;
    const auto document = QJsonDocument::fromJson(jsonWithoutComments(content.toUtf8()));
    if (!document.isObject()) {
        result.error = QStringLiteral("Could not read the profile's JSON configuration.");
        return result;
    }
    auto object = document.object();
    auto inbounds = object["inbounds"].toArray();
    int best = 100;
    bool modified = false;
    for (qsizetype i = 0; i < inbounds.size(); ++i) {
        auto inbound = inbounds[i].toObject();
        const auto type = inbound["type"].toString();
        const bool requested = inbound["set_system_proxy"].toBool();
        if ((type == "mixed" || type == "http") && requested) {
            inbound["set_system_proxy"] = false;
            modified = true;
        }
        if (type == "tun") {
            auto platform = inbound["platform"].toObject();
            auto proxy = platform["http_proxy"].toObject();
            if (proxy["enabled"].toBool()) {
                proxy["enabled"] = false;
                platform["http_proxy"] = proxy;
                inbound["platform"] = platform;
                modified = true;
            }
        }
        inbounds[i] = inbound;
        if (type != "mixed" && type != "http" && type != "socks")
            continue;
        if (!inbound["users"].toArray().isEmpty() ||
            inbound["tls"].toObject()["enabled"].toBool() ||
            !inbound["detour"].toString().isEmpty())
            continue;
        if (type == "http" && inbound.contains("version")) {
            const auto version = inbound["version"];
            if (version != QJsonValue(1) &&
                (!version.isArray() ||
                 (!version.toArray().isEmpty() && !version.toArray().contains(1))))
                continue; // Desktop clients need plain HTTP/1 CONNECT.
        }
        auto host = inbound["listen"].toString();
        if (host.isEmpty() || host == "0.0.0.0")
            host = "127.0.0.1";
        else if (host == "::")
            host = "::1";
        ProxyEndpoint endpoint{host, inbound["listen_port"].toInt(), type != "socks",
                               type != "http"};
        const int rank = (requested ? 0 : 3) + (type == "mixed" ? 0 : type == "http" ? 1 : 2);
        if (endpoint.valid() && rank < best) {
            best = rank;
            result.endpoint = endpoint;
        }
    }
    if (modified) {
        object["inbounds"] = inbounds;
        result.content = QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
    } else
        result.content = content;
    return result;
}
