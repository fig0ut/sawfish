#pragma once
#include <QCoreApplication>

inline QString desktopBackendPath() {
#ifdef Q_OS_WIN
    const auto name = "/rostra-daemon.exe";
#else
    const auto name = "/rostra-daemon";
#endif
    return qEnvironmentVariable("SAWFISH_TEST_BACKEND",
                                QCoreApplication::applicationDirPath() + name);
}
