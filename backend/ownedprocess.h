#pragma once
#include <QProcess>
#include <QVariant>
#ifdef Q_OS_LINUX
#include <csignal>
#include <sys/prctl.h>
#include <unistd.h>

// Only async-signal-safe system calls belong in the post-fork child modifier.
inline void bindProcessToParent(QProcess& process) {
    const pid_t parent = getpid();
    process.setChildProcessModifier([parent] {
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != parent)
            _exit(125);
    });
}
#elif defined(Q_OS_WIN)
#include <memory>
#include <qt_windows.h>
#include <vector>

inline void bindProcessToParent(QProcess& process) {
    struct Job {
        HANDLE handle = nullptr;
        STARTUPINFOEXW startup{};
        std::vector<unsigned char> attributes;
        ~Job() {
            if (startup.lpAttributeList)
                DeleteProcThreadAttributeList(startup.lpAttributeList);
            if (handle)
                CloseHandle(handle);
        }
    };
    auto job = std::make_shared<Job>();
    job->handle = CreateJobObjectW(nullptr, nullptr); // Non-inheritable, held only by Sawfish.
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    job->attributes.resize(bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(job->attributes.data());
    bool ready = job->handle &&
                 SetInformationJobObject(job->handle, JobObjectExtendedLimitInformation, &limits,
                                         sizeof(limits)) &&
                 InitializeProcThreadAttributeList(attributes, 1, 0, &bytes);
    if (ready) {
        job->startup.lpAttributeList = attributes;
        ready = UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
                                          &job->handle, sizeof(job->handle), nullptr, nullptr);
    }
    process.setProperty("ownershipReady", ready);
    process.setCreateProcessArgumentsModifier([job, ready](QProcess::CreateProcessArguments* args) {
        if (!ready) {
            args->applicationName = L""; // Fail closed; never launch an unowned child.
            return;
        }
        job->startup.StartupInfo = *args->startupInfo;
        job->startup.StartupInfo.cb = sizeof(STARTUPINFOEXW);
        args->startupInfo = &job->startup.StartupInfo;
        args->flags |= EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW;
    });
}
#else
inline void bindProcessToParent(QProcess&) {}
#endif
