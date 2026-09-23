#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <userenv.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr wchar_t ProfileName[] = L"MountainPlanner.SelectorSandbox.Probe";

int ProbeNetwork() {
    HANDLE token = nullptr;
    DWORD isContainer = 0;
    DWORD returned = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) ||
        !GetTokenInformation(token, TokenIsAppContainer, &isContainer, sizeof(isContainer), &returned)) {
        if (token) CloseHandle(token);
        return 20;
    }
    CloseHandle(token);
    if (!isContainer) return 21;

    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data)) return 22;
    SOCKET connection = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (connection == INVALID_SOCKET) {
        const int error = WSAGetLastError();
        WSACleanup();
        return error == WSAEACCES ? 0 : 23;
    }
    u_long nonblocking = 1;
    ioctlsocket(connection, FIONBIO, &nonblocking);
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(443);
    InetPtonW(AF_INET, L"1.1.1.1", &target.sin_addr);
    const int result = connect(connection, reinterpret_cast<sockaddr*>(&target), sizeof(target));
    int error = result == SOCKET_ERROR ? WSAGetLastError() : 0;
    if (error == WSAEWOULDBLOCK) {
        fd_set writable{};
        FD_ZERO(&writable);
        FD_SET(connection, &writable);
        timeval timeout{2, 0};
        const int selected = select(0, nullptr, &writable, nullptr, &timeout);
        if (selected == 1) {
            int length = sizeof(error);
            if (getsockopt(connection, SOL_SOCKET, SO_ERROR,
                reinterpret_cast<char*>(&error), &length) != 0) error = WSAGetLastError();
        } else {
            error = selected == 0 ? WSAETIMEDOUT : WSAGetLastError();
        }
    }
    closesocket(connection);
    WSACleanup();
    if (result == SOCKET_ERROR && error == WSAEACCES) return 0;
    if (result == SOCKET_ERROR && error == WSAETIMEDOUT) return 25;
    if (result == SOCKET_ERROR && error == WSAECONNREFUSED) return 26;
    return 24;
}

int LaunchProbe(const wchar_t* executable, const std::wstring& arguments, DWORD timeoutMilliseconds) {
    PSID profileSid = nullptr;
    HRESULT result = CreateAppContainerProfile(ProfileName, ProfileName,
        L"Mountain Planner selector network-denial feasibility probe", nullptr, 0, &profileSid);
    if (result == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
        result = DeriveAppContainerSidFromAppContainerName(ProfileName, &profileSid);
    }
    if (FAILED(result) || !profileSid) {
        std::fwprintf(stderr, L"AppContainer profile failed: 0x%08lx\n", static_cast<unsigned long>(result));
        return 30;
    }

    SIZE_T attributesSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributesSize);
    std::vector<unsigned char> attributesBuffer(attributesSize);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributesBuffer.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributesSize)) {
        FreeSid(profileSid);
        return 31;
    }
    SECURITY_CAPABILITIES capabilities{};
    capabilities.AppContainerSid = profileSid;
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
        &capabilities, sizeof(capabilities), nullptr, nullptr)) {
        DeleteProcThreadAttributeList(attributes);
        FreeSid(profileSid);
        return 32;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation,
        &limits, sizeof(limits))) {
        if (job) CloseHandle(job);
        DeleteProcThreadAttributeList(attributes);
        FreeSid(profileSid);
        return 33;
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    std::wstring command = std::wstring(L"\"") + executable + L"\" " + arguments;
    const BOOL started = CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr,
        &startup.StartupInfo, &process);
    const DWORD launchError = started ? ERROR_SUCCESS : GetLastError();
    DeleteProcThreadAttributeList(attributes);
    FreeSid(profileSid);
    if (!started) {
        std::fwprintf(stderr, L"AppContainer launch failed: %lu\n", launchError);
        CloseHandle(job);
        return 34;
    }
    if (!AssignProcessToJobObject(job, process.hProcess)) {
        TerminateProcess(process.hProcess, 35);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(job);
        return 35;
    }
    ResumeThread(process.hThread);
    const DWORD waited = WaitForSingleObject(process.hProcess, timeoutMilliseconds);
    DWORD exitCode = 36;
    if (waited == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(job);
    return static_cast<int>(exitCode);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && wcscmp(argv[1], L"--probe") == 0) return ProbeNetwork();
    if (argc == 2 && wcscmp(argv[1], L"--cleanup") == 0) {
        const HRESULT result = DeleteAppContainerProfile(ProfileName);
        std::printf("selector_sandbox_cleanup=0x%08lx\n", static_cast<unsigned long>(result));
        return SUCCEEDED(result) ? 0 : 42;
    }
    if ((argc == 4 || argc == 5) && wcscmp(argv[1], L"--launch-cef") == 0) {
        const std::wstring arguments = std::wstring(L"\"") + argv[3] + L"\""
            + (argc == 5 && wcscmp(argv[4], L"--blank") == 0 ? L" --blank" : L"");
        const int result = LaunchProbe(argv[2], arguments, 25000);
        std::printf("selector_cef_probe_exit=%d\n", result);
        return result;
    }
    wchar_t executable[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, executable, MAX_PATH)) return 40;
    const int result = LaunchProbe(executable, L"--probe", 10000);
    std::printf("selector_sandbox_probe_exit=%d\n", result);
    return result;
}
