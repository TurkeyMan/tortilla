#include "restart_explorer.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <restartmanager.h>
#include <tlhelp32.h>

#include <algorithm>
#include <filesystem>
#endif

namespace tortilla
{

#ifdef _WIN32

namespace
{

std::wstring wide(const std::string& s)
{
    std::wstring w(size_t(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0)),
                   L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
    return w;
}

// Every running explorer.exe, as the Restart Manager names a process: its
// id and start time (so a reused id isn't mistaken for it).
std::vector<RM_UNIQUE_PROCESS> explorers()
{
    std::vector<RM_UNIQUE_PROCESS> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W e{.dwSize = sizeof e};
    for (BOOL more = Process32FirstW(snap, &e); more; more = Process32NextW(snap, &e))
    {
        if (_wcsicmp(e.szExeFile, L"explorer.exe") != 0) continue;
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.th32ProcessID);
        if (!p) continue;
        FILETIME created, exited, kernel, user;
        if (GetProcessTimes(p, &created, &exited, &kernel, &user))
            out.push_back({e.th32ProcessID, created});
        CloseHandle(p);
    }
    CloseHandle(snap);
    return out;
}

}  // namespace

bool restart_explorer(const std::vector<std::string>& leftovers)
{
    DWORD session = 0;
    WCHAR key[CCH_RM_SESSION_KEY + 1]{};
    if (RmStartSession(&session, 0, key) != ERROR_SUCCESS) return false;
    std::vector<std::wstring> files;
    for (const std::string& f : leftovers) files.push_back(wide(f));
    std::vector<LPCWSTR> names;
    for (const std::wstring& f : files) names.push_back(f.c_str());
    std::vector<RM_UNIQUE_PROCESS> processes = explorers();
    bool ok = RmRegisterResources(session, UINT(names.size()), names.data(), UINT(processes.size()),
                                  processes.data(), 0, nullptr) == ERROR_SUCCESS &&
              RmShutdown(session, RmForceShutdown, nullptr) == ERROR_SUCCESS;
    if (ok)
        for (const std::wstring& f : files) DeleteFileW(f.c_str());
    // Started again whatever happened, so nobody is left without a taskbar.
    ok = RmRestart(session, 0, nullptr) == ERROR_SUCCESS && ok;
    RmEndSession(session);
    return ok;
}

std::vector<std::string> programs_using(const std::string& dir)
{
    std::vector<std::wstring> files;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(wide(dir), ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) files.push_back(it->path().wstring());
    std::vector<std::string> out;
    if (files.empty()) return out;
    DWORD session = 0;
    WCHAR key[CCH_RM_SESSION_KEY + 1]{};
    if (RmStartSession(&session, 0, key) != ERROR_SUCCESS) return out;
    std::vector<LPCWSTR> names;
    for (const std::wstring& f : files) names.push_back(f.c_str());
    UINT needed = 0, count = 0;
    DWORD reasons = 0;
    std::vector<RM_PROCESS_INFO> info;
    if (RmRegisterResources(session, UINT(names.size()), names.data(), 0, nullptr, 0, nullptr) ==
        ERROR_SUCCESS)
        for (int tries = 0; tries < 3; ++tries)
        {
            count = UINT(info.size());
            const DWORD r = RmGetList(session, &needed, &count, info.data(), &reasons);
            if (r == ERROR_SUCCESS) break;
            if (r != ERROR_MORE_DATA) break;
            info.resize(needed);
        }
    RmEndSession(session);
    for (UINT i = 0; i < count && i < info.size(); ++i)
    {
        const std::wstring& n = info[i].strAppName;
        std::string name(size_t(WideCharToMultiByte(CP_UTF8, 0, n.c_str(), int(n.size()), nullptr,
                                                    0, nullptr, nullptr)),
                         '\0');
        WideCharToMultiByte(CP_UTF8, 0, n.c_str(), int(n.size()), name.data(), int(name.size()),
                            nullptr, nullptr);
        if (!name.empty() && std::ranges::find(out, name) == out.end()) out.push_back(name);
    }
    return out;
}

#else

bool restart_explorer(const std::vector<std::string>&)
{
    return false;
}

std::vector<std::string> programs_using(const std::string&)
{
    return {};
}

#endif

}  // namespace tortilla
