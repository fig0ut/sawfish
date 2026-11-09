#include "proxysettings.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>

namespace {
const QString Missing = QStringLiteral("__sawfish_unset_f7ef826b__");
bool command(const QString& program, const QStringList& args, QString* output, QString* error) {
    QProcess process;
    process.start(program, args);
    if (!process.waitForStarted(2000) || !process.waitForFinished(3000)) {
        process.kill();
        process.waitForFinished(1000);
        *error = QStringLiteral("%1 did not respond.").arg(program);
        return false;
    }
    const auto stderrText = QString::fromUtf8(process.readAllStandardError()).trimmed();
    // gsettings can exit 0 despite a dconf write failure. Treat diagnostics as
    // failures and also read back every write before reporting success.
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() ||
        !stderrText.isEmpty()) {
        *error = QStringLiteral("%1 failed: %2").arg(program, stderrText.left(600));
        return false;
    }
    if (output) {
        *output = QString::fromUtf8(process.readAllStandardOutput());
        if (output->endsWith('\n'))
            output->chop(1);
    }
    return true;
}
} // namespace

ProxySettings::ProxySettings(QString journal, Runner runner)
    : m_journal(std::move(journal)), m_run(runner ? std::move(runner) : command) {}

bool ProxySettings::configure(const QString& desktop, QString* error) {
    if (desktop == "GNOME") {
        m_read = m_write = "gsettings";
    } else if (desktop == "KDE") {
        const auto version = QStandardPaths::findExecutable("kwriteconfig6").isEmpty() ? "5" : "6";
        m_read = "kreadconfig" + QString::fromLatin1(version);
        m_write = "kwriteconfig" + QString::fromLatin1(version);
    } else {
        *error = QStringLiteral("System proxy supports GNOME and KDE Plasma on Linux.");
        return false;
    }
    m_desktop = desktop;
    return true;
}

bool ProxySettings::detect(QString* error) {
    const auto desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP").toUpper().split(':');
    QString name;
    if (desktop.contains("KDE"))
        name = "KDE";
    else if (desktop.contains("GNOME") || desktop.contains("UNITY"))
        name = "GNOME";
    if (!configure(name, error))
        return false;
    QString value;
    return read(m_desktop == "GNOME" ? "org.gnome.system.proxy/mode" : "ProxyType", &value, error);
}

QJsonObject ProxySettings::desired(const ProxyEndpoint& endpoint) const {
    QJsonObject result;
    if (m_desktop == "GNOME") {
        result["org.gnome.system.proxy/mode"] = "'manual'";
        result["org.gnome.system.proxy/use-same-proxy"] = "false";
        result["org.gnome.system.proxy.http/use-authentication"] = "false";
        for (const auto& type :
             {QString("http"), QString("https"), QString("ftp"), QString("socks")}) {
            const bool enabled = type == "socks" ? endpoint.socks : type != "ftp" && endpoint.http;
            const auto schema = "org.gnome.system.proxy." + type;
            result[schema + "/host"] = enabled ? "'" + endpoint.host + "'" : "''";
            result[schema + "/port"] = QString::number(enabled ? endpoint.port : 0);
        }
    } else {
        result["ProxyType"] = "1";
        result["AuthMode"] = "0";
        for (const auto& type :
             {QString("http"), QString("https"), QString("ftp"), QString("socks")}) {
            const bool enabled = type == "socks" ? endpoint.socks : type != "ftp" && endpoint.http;
            QUrl url;
            url.setScheme(type == "socks" ? "socks" : "http");
            url.setHost(endpoint.host);
            url.setPort(endpoint.port);
            result[type + "Proxy"] = enabled ? url.toString() : QString{};
        }
    }
    return result;
}

bool ProxySettings::read(const QString& key, QString* value, QString* error) {
    if (m_desktop == "GNOME")
        return m_run(m_read, {"get", key.section('/', 0, 0), key.section('/', 1)}, value, error);
    return m_run(
        m_read,
        {"--file", "kioslaverc", "--group", "Proxy Settings", "--key", key, "--default", Missing},
        value, error);
}

bool ProxySettings::write(const QString& key, const QString& value, QString* error) {
    QStringList args;
    if (m_desktop == "GNOME") {
        QString writable;
        if (!m_run(m_write, {"writable", key.section('/', 0, 0), key.section('/', 1)}, &writable,
                   error))
            return false;
        if (writable != "true") {
            *error = QStringLiteral("Desktop policy prevents changing the system proxy.");
            return false;
        }
        args = {"set", key.section('/', 0, 0), key.section('/', 1), value};
    } else {
        args = {"--file", "kioslaverc", "--group", "Proxy Settings", "--key", key};
        if (value == Missing)
            args << "--delete";
        else
            args << "--" << value;
    }
    if (!m_run(m_write, args, nullptr, error))
        return false;
    QString actual;
    if (!read(key, &actual, error))
        return false;
    if (actual != value) {
        *error = QStringLiteral("The desktop did not accept the proxy setting %1.").arg(key);
        return false;
    }
    return true;
}

bool ProxySettings::reload(QString* error) {
    return m_desktop != "KDE" ||
           m_run("dbus-send",
                 {"--session", "--type=signal", "/KIO/Scheduler",
                  "org.kde.KIO.Scheduler.reparseSlaveConfiguration", "string:"},
                 nullptr, error);
}

QStringList ProxySettings::orderedKeys(const QJsonObject& values, bool restoring) const {
    auto keys = values.keys();
    const auto mode = m_desktop == "GNOME" ? "org.gnome.system.proxy/mode" : "ProxyType";
    keys.removeAll(mode);
    if (values.contains(mode)) {
        if (restoring)
            keys.prepend(mode);
        else
            keys.append(mode); // Activate only after all endpoints are ready.
    }
    return keys;
}

bool ProxySettings::active() const {
    return QFileInfo::exists(m_journal);
}

bool ProxySettings::apply(const ProxyEndpoint& endpoint, QString* error) {
    if (!endpoint.valid()) {
        *error = QStringLiteral("The profile needs a local HTTP, mixed, or SOCKS inbound with a "
                                "fixed port and no authentication or TLS.");
        return false;
    }
    if (!restore(error) || !detect(error))
        return false;
    const auto values = desired(endpoint);
    QJsonObject before;
    for (const auto& key : values.keys()) {
        QString value;
        if (!read(key, &value, error))
            return false;
        before[key] = value;
    }
    QSaveFile journal(m_journal);
    const auto data = QJsonDocument(QJsonObject{{"version", 1},
                                                {"desktop", m_desktop},
                                                {"before", before},
                                                {"applied", values}})
                          .toJson();
    if (!journal.open(QIODevice::WriteOnly) ||
        !journal.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        journal.write(data) != data.size() || !journal.commit()) {
        *error = QStringLiteral("Cannot save system-proxy recovery settings: %1")
                     .arg(journal.errorString());
        return false;
    }
    bool ok = true;
    for (const auto& key : orderedKeys(values, false)) {
        if (before[key] != values[key] && !write(key, values[key].toString(), error)) {
            ok = false;
            break;
        }
    }
    if (ok)
        ok = reload(error);
    if (!ok) {
        QString rollback;
        if (!restore(&rollback))
            *error += " Restore failed: " + rollback;
    }
    return ok;
}

bool ProxySettings::restore(QString* error) {
    if (!active())
        return true;
    QFile file(m_journal);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 65536) {
        *error = QStringLiteral("Cannot read system-proxy recovery settings.");
        return false;
    }
    const auto record = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    const auto before = record["before"].toObject(), applied = record["applied"].toObject();
    if (record["version"].toInt() != 1 || before.isEmpty() || before.keys() != applied.keys() ||
        !configure(record["desktop"].toString(), error)) {
        *error =
            QStringLiteral("Invalid system-proxy recovery settings; kept for manual recovery.");
        return false;
    }
    const auto allowed = desired({"127.0.0.1", 1, true, true});
    for (const auto& key : before.keys()) {
        if (!allowed.contains(key) || !before[key].isString() || !applied[key].isString()) {
            *error = QStringLiteral("Invalid key in system-proxy recovery settings.");
            return false;
        }
    }
    QStringList errors;
    QElapsedTimer deadline;
    deadline.start();
    for (const auto& key : orderedKeys(before, true)) {
        if (deadline.elapsed() > 5000) {
            errors << QStringLiteral(
                "Desktop proxy recovery timed out; it will be retried on the next launch.");
            break;
        }
        QString current, problem;
        if (!read(key, &current, &problem))
            errors << problem;
        else if (current == applied[key].toString() && current != before[key].toString() &&
                 !write(key, before[key].toString(), &problem))
            errors << problem;
        // Preserve settings the user or another application changed meanwhile.
    }
    QString problem;
    if (!reload(&problem))
        errors << problem;
    if (errors.isEmpty() && !QFile::remove(m_journal))
        errors << QStringLiteral("Cannot remove the system-proxy recovery record.");
    *error = errors.join('\n');
    return errors.isEmpty();
}
