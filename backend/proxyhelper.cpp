#include "proxysettings.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLockFile>
#include <iostream>

int runSystemProxyHelper() {
    const auto args = QCoreApplication::arguments();
    if (args.size() != 3)
        return 2;
    auto reply = [](const QJsonObject& object) {
        const auto data = QJsonDocument(object).toJson(QJsonDocument::Compact);
        std::cout << data.constData() << std::endl;
    };
    const auto directory = args[2];
    if (!QDir().mkpath(directory) ||
        !QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                              QFileDevice::ExeOwner)) {
        reply({{"ready", true},
               {"available", false},
               {"recoveryFailed", true},
               {"error", "Cannot create private system-proxy recovery directory."}});
        return 1;
    }
    // One proxy owner per desktop user, even when Sawfish uses --data-dir.
    QLockFile lock(directory + "/owner.lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock(1500)) {
        reply({{"ready", true},
               {"available", false},
               {"error", "Another Sawfish instance is managing the desktop proxy."}});
        // Keep the pipe open so the client can distinguish unavailable from a crash.
        std::string ignored;
        while (std::getline(std::cin, ignored)) {
        }
        return 0;
    }
    ProxySettings settings(directory + "/recovery.json");
    QString problem;
    const bool recovered = settings.restore(&problem);
    const bool available = recovered && settings.detect(&problem);
    reply({{"ready", true},
           {"available", available},
           {"recoveryFailed", !recovered},
           {"error", problem}});
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.size() > 4096)
            break;
        const auto request = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
        const auto target = request["target"].toObject();
        problem.clear();
        bool active = false;
        if (!request.contains("target"))
            problem = "Invalid system-proxy request.";
        else if (target.isEmpty())
            settings.restore(&problem);
        else if (!available)
            problem = "Desktop proxy settings are unavailable.";
        else
            active = settings.apply(ProxyEndpoint::fromJson(target), &problem);
        reply({{"active", active}, {"error", problem}});
    }
    // The parent closed the pipe, exited, or was killed. Never inherit its
    // parent-death signal: this process must survive long enough to restore.
    problem.clear();
    if (!settings.restore(&problem)) {
        reply({{"active", false}, {"error", problem}});
        return 1; // Leave the durable journal for the next launch.
    }
    return 0;
}
