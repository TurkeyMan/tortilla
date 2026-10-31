#include "channel.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <cstring>
#include <filesystem>
#include <thread>

namespace tortilla::channel
{

namespace
{

constexpr size_t k_max_line = 64 * 1024;

#ifdef _WIN32

std::wstring wide(std::string_view s)
{
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
    return w;
}

// Per user and session, so another user's (or session's) server isn't asked.
std::wstring user_tag()
{
    wchar_t user[256];
    DWORD n = 256;
    if (!GetUserNameW(user, &n)) user[0] = 0;
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    return std::wstring(user) + L"-" + std::to_wstring(session);
}

std::wstring pipe_name(std::string_view name)
{
    return L"\\\\.\\pipe\\tortilla-" + wide(name) + L"-" + user_tag();
}

// Waits for an overlapped call to finish, at most until `deadline`.
bool finish(HANDLE h, OVERLAPPED& ov, BOOL started, DWORD& done,
            std::chrono::steady_clock::time_point deadline)
{
    if (started) return GetOverlappedResult(h, &ov, &done, FALSE);
    if (GetLastError() != ERROR_IO_PENDING) return false;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (WaitForSingleObject(ov.hEvent, DWORD(std::max<long long>(left.count(), 0))) !=
        WAIT_OBJECT_0)
    {
        CancelIo(h);
        GetOverlappedResult(h, &ov, &done, TRUE);
        return false;
    }
    return GetOverlappedResult(h, &ov, &done, FALSE);
}

#else

std::filesystem::path runtime_dir()
{
    namespace fs = std::filesystem;
    fs::path dir;
    if (const char* x = std::getenv("XDG_RUNTIME_DIR"); x && *x)
        dir = fs::path(x) / "tortilla";
    else
        dir = fs::temp_directory_path() / ("tortilla-" + std::to_string(getuid()));
    mkdir(dir.c_str(), 0700);
    return dir;
}

std::filesystem::path socket_path(std::string_view name)
{
    return runtime_dir() / (std::string(name) + ".sock");
}

#endif

}  // namespace

#ifdef _WIN32

bool hold_single(std::string_view name)
{
    const std::wstring mutex = L"Local\\tortilla-" + wide(name) + L"-" + user_tag();
    HANDLE h = CreateMutexW(nullptr, FALSE, mutex.c_str());
    if (!h) return false;
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(h);
        return false;
    }
    return true;  // kept until the process ends
}

std::expected<void, std::string> serve(std::string_view name,
                                       const std::function<std::string(std::string_view)>& answer)
{
    const std::wstring pipe = pipe_name(name);
    auto make = [&]
    {
        return CreateNamedPipeW(pipe.c_str(), PIPE_ACCESS_DUPLEX,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                                    PIPE_REJECT_REMOTE_CLIENTS,
                                PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, nullptr);
    };
    // One instance always waiting, so an asker never finds the name gone
    // (and starts another server) while one is being answered.
    HANDLE waiting = make();
    if (waiting == INVALID_HANDLE_VALUE)
        return std::unexpected("CreateNamedPipe failed: " + std::to_string(GetLastError()));
    for (;;)
    {
        const bool connected =
            ConnectNamedPipe(waiting, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
        HANDLE h = waiting;
        waiting = make();
        if (waiting == INVALID_HANDLE_VALUE)
            return std::unexpected("CreateNamedPipe failed: " + std::to_string(GetLastError()));
        if (connected)
        {
            std::string line;
            char buf[4096];
            DWORD got = 0;
            while (line.find('\n') == std::string::npos && line.size() < k_max_line &&
                   ReadFile(h, buf, sizeof buf, &got, nullptr) && got > 0)
                line.append(buf, got);
            if (size_t nl = line.find('\n'); nl != std::string::npos)
            {
                std::string reply = answer(std::string_view(line).substr(0, nl)) + "\n";
                DWORD wrote = 0;
                WriteFile(h, reply.data(), DWORD(reply.size()), &wrote, nullptr);
                FlushFileBuffers(h);
            }
            DisconnectNamedPipe(h);
        }
        CloseHandle(h);
    }
}

std::expected<std::string, ask_error> ask(std::string_view name, std::string_view request,
                                          std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const std::wstring pipe = pipe_name(name);
    HANDLE h = CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY &&
        WaitNamedPipeW(pipe.c_str(), DWORD(timeout.count())))
        h = CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                        FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return std::unexpected(GetLastError() == ERROR_FILE_NOT_FOUND ? ask_error::no_server
                                                                      : ask_error::failed);
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::string out = std::string(request) + "\n";
    DWORD done = 0;
    bool ok =
        finish(h, ov, WriteFile(h, out.data(), DWORD(out.size()), nullptr, &ov), done, deadline);
    out.clear();
    char buf[4096];
    while (ok && out.find('\n') == std::string::npos && out.size() < k_max_line)
    {
        ResetEvent(ov.hEvent);
        ok = finish(h, ov, ReadFile(h, buf, sizeof buf, nullptr, &ov), done, deadline) && done > 0;
        if (ok) out.append(buf, done);
    }
    CloseHandle(ov.hEvent);
    CloseHandle(h);
    const size_t nl = out.find('\n');
    if (!ok && nl == std::string::npos) return std::unexpected(ask_error::failed);
    return out.substr(0, nl);
}

#else

bool hold_single(std::string_view name)
{
    const std::string lock = (runtime_dir() / (std::string(name) + ".lock")).string();
    const int fd = open(lock.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0)
    {
        close(fd);
        return false;
    }
    return true;  // kept until the process ends
}

std::expected<void, std::string> serve(std::string_view name,
                                       const std::function<std::string(std::string_view)>& answer)
{
    const std::string path = socket_path(name).string();
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path)
        return std::unexpected("socket path too long: " + path);
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    const int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0) return std::unexpected(std::string("socket: ") + std::strerror(errno));
    unlink(path.c_str());  // a server before ours, gone (hold_single says so)
    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
        listen(listener, 64) != 0)
        return std::unexpected(std::string("bind ") + path + ": " + std::strerror(errno));
    for (;;)
    {
        const int c = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
        if (c < 0)
        {
            if (errno == EINTR) continue;
            return std::unexpected(std::string("accept: ") + std::strerror(errno));
        }
        // A stuck asker mustn't hold up the rest.
        timeval tv{0, 500 * 1000};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        std::string line;
        char buf[4096];
        for (ssize_t got; line.find('\n') == std::string::npos && line.size() < k_max_line &&
                          (got = read(c, buf, sizeof buf)) > 0;)
            line.append(buf, size_t(got));
        if (size_t nl = line.find('\n'); nl != std::string::npos)
        {
            const std::string reply = answer(std::string_view(line).substr(0, nl)) + "\n";
            (void)!write(c, reply.data(), reply.size());
        }
        close(c);
    }
}

std::expected<std::string, ask_error> ask(std::string_view name, std::string_view request,
                                          std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const std::string path = socket_path(name).string();
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path) return std::unexpected(ask_error::failed);
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    const int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (s < 0) return std::unexpected(ask_error::failed);
    struct closer
    {
        int fd;
        ~closer() { close(fd); }
    } close_it{s};
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 && errno != EINPROGRESS &&
        errno != EAGAIN)
        return std::unexpected(errno == ENOENT || errno == ECONNREFUSED ? ask_error::no_server
                                                                        : ask_error::failed);
    auto wait = [&](short events)
    {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        pollfd p{s, events, 0};
        return left.count() > 0 && poll(&p, 1, int(left.count())) == 1 && (p.revents & events);
    };
    const std::string out = std::string(request) + "\n";
    for (size_t sent = 0; sent < out.size();)
    {
        if (!wait(POLLOUT)) return std::unexpected(ask_error::failed);
        const ssize_t n = send(s, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
        if (n <= 0 && errno != EAGAIN) return std::unexpected(ask_error::failed);
        if (n > 0) sent += size_t(n);
    }
    std::string in;
    char buf[4096];
    while (in.find('\n') == std::string::npos && in.size() < k_max_line)
    {
        if (!wait(POLLIN)) return std::unexpected(ask_error::failed);
        const ssize_t n = read(s, buf, sizeof buf);
        if (n == 0) break;
        if (n < 0)
        {
            if (errno == EAGAIN) continue;
            return std::unexpected(ask_error::failed);
        }
        in.append(buf, size_t(n));
    }
    const size_t nl = in.find('\n');
    if (nl == std::string::npos) return std::unexpected(ask_error::failed);
    return in.substr(0, nl);
}

#endif

}  // namespace tortilla::channel
