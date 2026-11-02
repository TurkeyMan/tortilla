#include "channel.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
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

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <semaphore>
#include <system_error>
#include <thread>

namespace tortilla::channel
{

namespace
{

// A whole selection asked at once can be long.
constexpr size_t k_max_line = 16 * 1024 * 1024;
constexpr size_t k_chunk = 64 * 1024;

// A line read in chunks: each chunk searched once for the newline, not the
// whole of what came before again.
struct line_reader
{
    std::string data;
    size_t end = std::string::npos;  // the newline, once seen

    bool done() const { return end != std::string::npos || data.size() >= k_max_line; }
    // Room for a chunk at the end; then got() says how much of it came.
    char* room()
    {
        data.resize(data.size() + k_chunk);
        return data.data() + data.size() - k_chunk;
    }
    void got(size_t n)
    {
        const size_t from = data.size() - k_chunk;
        data.resize(from + n);
        if (const size_t nl = std::string_view(data).find('\n', from); nl != std::string::npos)
            end = nl;
    }
    // The line without its newline, taken out.
    std::optional<std::string> take()
    {
        if (end == std::string::npos) return std::nullopt;
        data.resize(end);
        return std::move(data);
    }
};

using clock = std::chrono::steady_clock;
using answer_fn = std::function<std::string(std::string_view)>;

// What's left until `deadline`, rounded up: a fraction of a millisecond
// still waits one.
std::chrono::milliseconds left_until(clock::time_point deadline)
{
    return std::max(std::chrono::ceil<std::chrono::milliseconds>(deadline - clock::now()),
                    std::chrono::milliseconds(0));
}

// The connections being served: a slot taken before each is accepted, given
// back by its thread when it's done. Shared with those threads, which may
// still be giving theirs back as serve() returns.
using slots = std::counting_semaphore<k_serve_clients>;

// Runs `serve` on a thread of its own, which gives back its slot in `free`
// when it's done; inline if no thread can be had.
template <class Serve>
void serve_aside(const std::shared_ptr<slots>& free, Serve serve)
{
    try
    {
        std::thread(
            [free, serve]
            {
                serve();
                free->release();
            })
            .detach();
    }
    catch (const std::system_error&)
    {
        serve();
        free->release();
    }
}

// Before serve() returns with an error: the threads serving use `answer`,
// which is the caller's, so they finish first.
void drain(slots& free)
{
    for (int i = 0; i < k_serve_clients; ++i) free.acquire();
}

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

// Waits for an overlapped call to finish, at most until `deadline`: past
// it, the call is cancelled (and waited out, as `ov` and its buffer are the
// caller's) and this is false.
bool finish(HANDLE h, OVERLAPPED& ov, BOOL started, DWORD& done, clock::time_point deadline)
{
    done = 0;
    if (started) return GetOverlappedResult(h, &ov, &done, FALSE);
    if (GetLastError() != ERROR_IO_PENDING) return false;
    if (WaitForSingleObject(ov.hEvent, DWORD(left_until(deadline).count())) != WAIT_OBJECT_0)
    {
        CancelIoEx(h, &ov);
        GetOverlappedResult(h, &ov, &done, TRUE);
        return false;
    }
    return GetOverlappedResult(h, &ov, &done, FALSE);
}

// One connection to a pipe instance (made overlapped), from its line to
// its answer, each within k_serve_wait; then the instance is closed.
void serve_one(HANDLE h, const answer_fn& answer)
{
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent)
    {
        auto deadline = clock::now() + k_serve_wait;
        line_reader in;
        DWORD done = 0;
        bool ok = true;
        while (ok && !in.done())
        {
            char* room = in.room();
            ok = finish(h, ov, ReadFile(h, room, DWORD(k_chunk), nullptr, &ov), done, deadline) &&
                 done > 0;
            in.got(ok ? done : 0);
        }
        if (auto line = in.take())
        {
            std::string reply = answer(*line);
            reply += '\n';
            deadline = clock::now() + k_serve_wait;
            ok = finish(h, ov, WriteFile(h, reply.data(), DWORD(reply.size()), nullptr, &ov), done,
                        deadline) &&
                 done == reply.size();
            // Disconnecting throws away what the asker hasn't read yet, so
            // wait for it to hang up, which it does once it has the line
            // (FlushFileBuffers would wait for that too, but without a limit).
            char rest[256];
            while (ok)
                ok = finish(h, ov, ReadFile(h, rest, DWORD(sizeof rest), nullptr, &ov), done,
                            deadline) &&
                     done > 0;
        }
        CloseHandle(ov.hEvent);
    }
    DisconnectNamedPipe(h);
    CloseHandle(h);
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

// Waits until `fd` is ready for `events`, or has hung up or failed (which
// the read or write then finds), at most until `deadline`.
bool wait_for(int fd, short events, clock::time_point deadline)
{
    for (;;)
    {
        const auto left = left_until(deadline);
        if (left.count() <= 0) return false;
        pollfd p{fd, events, 0};
        const int r = poll(&p, 1, int(left.count()));
        if (r < 0 && errno == EINTR) continue;
        return r == 1 && (p.revents & (events | POLLHUP | POLLERR));
    }
}

// One connection (non-blocking), from its line to its answer, each within
// k_serve_wait; then it's closed.
void serve_one(int c, const answer_fn& answer)
{
    auto deadline = clock::now() + k_serve_wait;
    line_reader in;
    while (!in.done() && wait_for(c, POLLIN, deadline))
    {
        const ssize_t got = read(c, in.room(), k_chunk);
        in.got(got > 0 ? size_t(got) : 0);
        if (got == 0 || (got < 0 && errno != EAGAIN && errno != EINTR)) break;
    }
    if (auto line = in.take())
    {
        std::string reply = answer(*line);
        reply += '\n';
        deadline = clock::now() + k_serve_wait;
        for (size_t sent = 0; sent < reply.size() && wait_for(c, POLLOUT, deadline);)
        {
            // MSG_NOSIGNAL: an asker gone mustn't end the server with SIGPIPE.
            const ssize_t n = send(c, reply.data() + sent, reply.size() - sent, MSG_NOSIGNAL);
            if (n > 0)
                sent += size_t(n);
            else if (n == 0 || (errno != EAGAIN && errno != EINTR))
                break;
        }
    }
    close(c);
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

std::string address(std::string_view name)
{
    const std::wstring w = pipe_name(name);
    std::string s(size_t(WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0,
                                             nullptr, nullptr)),
                  '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), int(s.size()), nullptr,
                        nullptr);
    return s;
}

std::expected<void, std::string> serve(std::string_view name, const answer_fn& answer)
{
    const std::wstring pipe = pipe_name(name);
    auto make = [&]
    {
        return CreateNamedPipeW(pipe.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                                    PIPE_REJECT_REMOTE_CLIENTS,
                                PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, nullptr);
    };
    // One instance always waiting, so an asker never finds the name gone
    // (and starts another server) while one is being answered. An asker can
    // connect to it before it's waited on: so while every slot is taken one
    // asker queues there, and the rest wait for the pipe (ask() does).
    HANDLE waiting = make();
    if (waiting == INVALID_HANDLE_VALUE)
        return std::unexpected("CreateNamedPipe failed: " + std::to_string(GetLastError()));
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent)
    {
        CloseHandle(waiting);
        return std::unexpected("CreateEvent failed: " + std::to_string(GetLastError()));
    }
    auto free = std::make_shared<slots>(k_serve_clients);
    for (;;)
    {
        free->acquire();
        bool connected = ConnectNamedPipe(waiting, &ov);
        if (!connected)
        {
            const DWORD e = GetLastError();
            DWORD unused = 0;
            connected = e == ERROR_PIPE_CONNECTED ||
                        (e == ERROR_IO_PENDING && GetOverlappedResult(waiting, &ov, &unused, TRUE));
        }
        HANDLE h = waiting;
        waiting = make();
        if (waiting == INVALID_HANDLE_VALUE)
        {
            const DWORD e = GetLastError();
            CloseHandle(h);
            CloseHandle(ov.hEvent);
            free->release();
            drain(*free);
            return std::unexpected("CreateNamedPipe failed: " + std::to_string(e));
        }
        if (!connected)
        {
            CloseHandle(h);
            free->release();
            continue;
        }
        serve_aside(free, [h, &answer] { serve_one(h, answer); });
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
    line_reader in;
    while (ok && !in.done())
    {
        ResetEvent(ov.hEvent);
        char* room = in.room();
        ok = finish(h, ov, ReadFile(h, room, DWORD(k_chunk), nullptr, &ov), done, deadline) &&
             done > 0;
        in.got(ok ? done : 0);
    }
    CloseHandle(ov.hEvent);
    CloseHandle(h);
    if (auto line = in.take()) return std::move(*line);
    return std::unexpected(ask_error::failed);
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

std::string address(std::string_view name)
{
    return socket_path(name).string();
}

std::expected<void, std::string> serve(std::string_view name, const answer_fn& answer)
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
    auto free = std::make_shared<slots>(k_serve_clients);
    for (;;)
    {
        free->acquire();
        const int c = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (c < 0)
        {
            const int e = errno;
            free->release();
            if (e == EINTR || e == ECONNABORTED) continue;
            close(listener);
            drain(*free);
            return std::unexpected(std::string("accept: ") + std::strerror(e));
        }
        serve_aside(free, [c, &answer] { serve_one(c, answer); });
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
    auto wait = [&](short events) { return wait_for(s, events, deadline); };
    const std::string out = std::string(request) + "\n";
    for (size_t sent = 0; sent < out.size();)
    {
        if (!wait(POLLOUT)) return std::unexpected(ask_error::failed);
        const ssize_t n = send(s, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
        if (n <= 0 && errno != EAGAIN) return std::unexpected(ask_error::failed);
        if (n > 0) sent += size_t(n);
    }
    line_reader in;
    while (!in.done())
    {
        if (!wait(POLLIN)) return std::unexpected(ask_error::failed);
        const ssize_t n = read(s, in.room(), k_chunk);
        in.got(n > 0 ? size_t(n) : 0);
        if (n == 0) break;
        if (n < 0 && errno != EAGAIN) return std::unexpected(ask_error::failed);
    }
    if (auto line = in.take()) return std::move(*line);
    return std::unexpected(ask_error::failed);
}

#endif

}  // namespace tortilla::channel
