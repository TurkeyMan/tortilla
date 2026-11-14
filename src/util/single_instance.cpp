#include "single_instance.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#else
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstdint>
#include <thread>

namespace tortilla::single_instance
{

namespace
{

using clock = std::chrono::steady_clock;

// Far more than a request needs: a length past it is someone else's bytes.
constexpr uint32_t k_max_message = 1024 * 1024;
// How long the host gives a sender to send, and to hang up after its
// answer: one that stalls is let go.
constexpr auto k_serve_wait = std::chrono::milliseconds(1000);

// What's left until `deadline`, rounded up: a fraction of a millisecond
// still waits one.
std::chrono::milliseconds left_until(clock::time_point deadline)
{
    return std::max(std::chrono::ceil<std::chrono::milliseconds>(deadline - clock::now()),
                    std::chrono::milliseconds(0));
}

std::string frame(std::string_view message)
{
    const auto n = uint32_t(message.size());
    std::string out{char(n & 0xff), char((n >> 8) & 0xff), char((n >> 16) & 0xff),
                    char((n >> 24) & 0xff)};
    out += message;
    return out;
}

uint32_t length_of(const unsigned char* b)
{
    return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
}

}  // namespace

#ifdef _WIN32

namespace
{

std::wstring wide(std::string_view s)
{
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
    return w;
}

std::string narrow(std::wstring_view w)
{
    std::string s(size_t(WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0,
                                             nullptr, nullptr)),
                  '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), int(s.size()), nullptr,
                        nullptr);
    return s;
}

// A token's user, as TOKEN_USER (whose SID points into it).
std::unique_ptr<char[]> token_user(HANDLE token)
{
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (size == 0) return nullptr;
    auto buffer = std::make_unique<char[]>(size);
    if (!GetTokenInformation(token, TokenUser, buffer.get(), size, &size)) return nullptr;
    return buffer;
}

std::unique_ptr<char[]> process_user(HANDLE process)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return nullptr;
    auto user = token_user(token);
    CloseHandle(token);
    return user;
}

PSID sid_of(const std::unique_ptr<char[]>& user)
{
    return user ? reinterpret_cast<TOKEN_USER*>(user.get())->User.Sid : nullptr;
}

std::wstring sid_text(PSID sid)
{
    wchar_t* text = nullptr;
    if (!sid || !ConvertSidToStringSidW(sid, &text)) return {};
    std::wstring s = text;
    LocalFree(text);
    return s;
}

// An elevated process has endpoints of its own: a request from one isn't
// handed to an unelevated host (nor the other way).
bool elevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION e{};
    DWORD size = 0;
    const bool yes = GetTokenInformation(token, TokenElevation, &e, sizeof e, &size) && e.TokenIsElevated;
    CloseHandle(token);
    return yes;
}

// Whether process `pid` runs as this process's user: a pipe of our name
// made first by someone else isn't told anything.
bool same_user(ULONG pid)
{
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;
    const auto theirs = process_user(p);
    CloseHandle(p);
    const auto ours = process_user(GetCurrentProcess());
    return sid_of(theirs) && sid_of(ours) && EqualSid(sid_of(theirs), sid_of(ours));
}

// Waits for an overlapped call to finish, at most until `deadline`: past
// it, the call is cancelled (and waited out, as `ov` and its buffer are the
// caller's). True only when it finished: one that completed as it was
// cancelled did happen (a confirm written is a confirm sent).
bool finish(HANDLE h, OVERLAPPED& ov, BOOL started, DWORD& done, clock::time_point deadline)
{
    done = 0;
    if (started) return GetOverlappedResult(h, &ov, &done, FALSE);
    if (GetLastError() != ERROR_IO_PENDING) return false;
    if (WaitForSingleObject(ov.hEvent, DWORD(left_until(deadline).count())) != WAIT_OBJECT_0)
        CancelIoEx(h, &ov);
    return GetOverlappedResult(h, &ov, &done, TRUE);
}

// All of `size` bytes, read or written, by `deadline`.
bool read_all(HANDLE h, OVERLAPPED& ov, void* to, DWORD size, clock::time_point deadline)
{
    for (DWORD at = 0, done = 0; at < size; at += done)
        if (!finish(h, ov, ReadFile(h, static_cast<char*>(to) + at, size - at, nullptr, &ov), done,
                    deadline) ||
            done == 0)
            return false;
    return true;
}

bool write_all(HANDLE h, OVERLAPPED& ov, const std::string& bytes, clock::time_point deadline)
{
    DWORD done = 0;
    return finish(h, ov, WriteFile(h, bytes.data(), DWORD(bytes.size()), nullptr, &ov), done,
                  deadline) &&
           done == bytes.size();
}

}  // namespace

struct host::impl
{
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE stop = nullptr;  // set (manual reset) to end the thread
    handler handle;
    std::chrono::milliseconds confirm_wait = k_confirm_wait;
    std::function<void()> after_answer;  // tests: a thread paused after answering
    std::thread thread;

    ~impl()
    {
        if (stop) SetEvent(stop);
        if (thread.joinable()) thread.join();
        if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
        if (stop) CloseHandle(stop);
    }

    // One sender, from its message to its confirm (or its answer, turned
    // away); then it's let go.
    void serve_one(OVERLAPPED& ov)
    {
        const auto deadline = clock::now() + k_serve_wait;
        unsigned char size[4];
        if (!read_all(pipe, ov, size, 4, deadline)) return;
        const uint32_t n = length_of(size);
        if (n > k_max_message) return;
        std::string message(n, '\0');
        if (n && !read_all(pipe, ov, message.data(), n, deadline)) return;
        const host::then_fn then = handle(std::move(message));
        if (!then)
        {
            if (!write_all(pipe, ov, std::string(1, '\0'), clock::now() + k_serve_wait)) return;
            // Disconnecting throws away what the sender hasn't read yet, so
            // wait for it to hang up, which it does once it has the answer.
            char rest[64];
            DWORD done = 0;
            const auto hang_up = clock::now() + k_serve_wait;
            while (finish(pipe, ov, ReadFile(pipe, rest, sizeof rest, nullptr, &ov), done, hang_up) &&
                   done > 0)
            {
            }
            return;
        }
        // Taken: acted on only once the sender confirms. A sender that gave
        // up has hung up (or does at its timeout), which ends this at once.
        // A confirm already sent is read however late this gets to it: a
        // pipe's ReadFile takes what's there at once, deadline or not.
        unsigned char confirm = 0;
        if (!write_all(pipe, ov, std::string(1, '\1'), clock::now() + k_serve_wait)) return then(false);
        const auto confirm_by = clock::now() + confirm_wait;
        if (after_answer) after_answer();
        then(read_all(pipe, ov, &confirm, 1, confirm_by) && confirm == 1);
    }

    void run()
    {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) return;
        // One instance, reused: the name never goes while this serves, so a
        // sender never finds it missing (and starts a host of its own); one
        // arriving while another is served waits for the pipe.
        while (WaitForSingleObject(stop, 0) != WAIT_OBJECT_0)
        {
            ResetEvent(ov.hEvent);
            DWORD done = 0;
            bool connected = ConnectNamedPipe(pipe, &ov);
            if (!connected)
            {
                const DWORD e = GetLastError();
                connected = e == ERROR_PIPE_CONNECTED;
                if (e == ERROR_IO_PENDING)
                {
                    HANDLE both[] = {ov.hEvent, stop};
                    if (WaitForMultipleObjects(2, both, FALSE, INFINITE) != WAIT_OBJECT_0)
                    {
                        CancelIoEx(pipe, &ov);
                        GetOverlappedResult(pipe, &ov, &done, TRUE);
                        break;
                    }
                    connected = GetOverlappedResult(pipe, &ov, &done, FALSE);
                }
            }
            // A sender that connected before serving began and has given up
            // since: nothing to answer, and the next can come.
            const bool gone = !connected && GetLastError() == ERROR_NO_DATA;
            if (connected) serve_one(ov);
            DisconnectNamedPipe(pipe);
            // A failure that repeats mustn't spin.
            if (!connected && !gone) WaitForSingleObject(stop, 50);
        }
        CloseHandle(ov.hEvent);
    }
};

std::string endpoint(std::string_view name)
{
    // Per user and session: \\.\pipe is the whole machine's.
    const auto user = process_user(GetCurrentProcess());
    const std::wstring sid = sid_text(sid_of(user));
    if (sid.empty()) return {};
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    return narrow(L"\\\\.\\pipe\\tortilla-" + wide(name) + L"-" + sid + L"-" +
                  std::to_wstring(session) + (elevated() ? L"-elevated" : L""));
}

sent send(const std::string& endpoint, std::string_view message, std::chrono::milliseconds timeout)
{
    if (endpoint.empty()) return sent::no_host;
    const std::wstring pipe = wide(endpoint);
    const auto deadline = clock::now() + timeout;
    HANDLE h = INVALID_HANDLE_VALUE;
    for (;;)
    {
        // Identification only: a host can't act as this process.
        h = CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                        nullptr);
        if (h != INVALID_HANDLE_VALUE) break;
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND) return sent::no_host;
        // Busy with another sender: wait for it, while there's time.
        if (e != ERROR_PIPE_BUSY || left_until(deadline).count() == 0) return sent::failed;
        if (!WaitNamedPipeW(pipe.c_str(), DWORD(std::max<long long>(left_until(deadline).count(), 1))) &&
            GetLastError() == ERROR_FILE_NOT_FOUND)
            return sent::no_host;
    }
    ULONG pid = 0;
    if (!GetNamedPipeServerProcessId(h, &pid) || !same_user(pid))
    {
        CloseHandle(h);
        return sent::failed;
    }
    // The host's windows may come to the front, as this process's could.
    AllowSetForegroundWindow(pid);
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    sent result = sent::failed;
    unsigned char answer = 0;
    if (ov.hEvent && write_all(h, ov, frame(message), deadline) &&
        read_all(h, ov, &answer, 1, deadline))
    {
        if (!answer)
            result = sent::declined;
        // Taken in time: confirmed, or the host leaves it to this process.
        else if (left_until(deadline).count() > 0 && write_all(h, ov, std::string(1, '\1'), deadline))
            result = sent::taken;
    }
    if (ov.hEvent) CloseHandle(ov.hEvent);
    CloseHandle(h);
    return result;
}

std::unique_ptr<host> host::claim(const std::string& endpoint, std::chrono::milliseconds confirm_wait)
{
    if (endpoint.empty()) return nullptr;
    // Only this user: no one else connects, whatever the default would allow.
    const auto user = process_user(GetCurrentProcess());
    const std::wstring sid = sid_text(sid_of(user));
    if (sid.empty()) return nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW((L"D:P(A;;GA;;;" + sid + L")").c_str(),
                                                              SDDL_REVISION_1, &sd, nullptr))
        return nullptr;
    SECURITY_ATTRIBUTES sa{sizeof sa, sd, FALSE};
    // The first instance, or nothing: of two processes claiming at once,
    // the other's fails here.
    HANDLE pipe = CreateNamedPipeW(
        wide(endpoint).c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096,
        0, &sa);
    LocalFree(sd);
    if (pipe == INVALID_HANDLE_VALUE) return nullptr;
    // Senders can connect from here on; they're answered once serve() runs.
    auto i = std::make_unique<impl>();
    i->confirm_wait = confirm_wait;
    i->pipe = pipe;
    i->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!i->stop) return nullptr;
    return std::unique_ptr<host>(new host(std::move(i)));
}

#else

namespace
{

// The user's runtime folder, or one of theirs in /tmp: only theirs, or
// nothing (someone else made it first).
std::string private_dir()
{
    std::string dir;
    if (const char* x = std::getenv("XDG_RUNTIME_DIR"); x && *x)
        dir = std::string(x) + "/tortilla";
    else
        dir = "/tmp/tortilla-" + std::to_string(::getuid());
    ::mkdir(dir.c_str(), 0700);
    struct stat st{};
    if (::lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != ::getuid() ||
        (st.st_mode & 077) != 0)
        return {};
    return dir;
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
        const int r = ::poll(&p, 1, int(left.count()));
        if (r < 0 && errno == EINTR) continue;
        return r == 1 && (p.revents & (events | POLLHUP | POLLERR));
    }
}

// All of `size` bytes, read or written (non-blocking), by `deadline`. What's
// there already is taken first, whenever this is called: a thread that
// comes to it late (paused past the deadline) still reads a confirm that
// was sent in time, as a pipe's ReadFile does on Windows.
bool read_all(int fd, void* to, size_t size, clock::time_point deadline)
{
    for (size_t at = 0; at < size;)
    {
        const ssize_t n = ::read(fd, static_cast<char*>(to) + at, size - at);
        if (n > 0)
        {
            at += size_t(n);
            continue;
        }
        if (n == 0 || (errno != EAGAIN && errno != EINTR)) return false;
        if (errno == EAGAIN && !wait_for(fd, POLLIN, deadline))
        {
            // Past the deadline: one last look at what's queued.
            const ssize_t last = ::read(fd, static_cast<char*>(to) + at, size - at);
            if (last <= 0) return false;
            at += size_t(last);
        }
    }
    return true;
}

bool write_all(int fd, const std::string& bytes, clock::time_point deadline)
{
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;  // a peer gone mustn't end us with SIGPIPE
#else
    constexpr int flags = 0;
#endif
    // Tried before waiting, as read_all: room there now is used, however late.
    for (size_t at = 0; at < bytes.size();)
    {
        const ssize_t n = ::send(fd, bytes.data() + at, bytes.size() - at, flags);
        if (n > 0)
        {
            at += size_t(n);
            continue;
        }
        if (n == 0 || (errno != EAGAIN && errno != EINTR)) return false;
        if (errno == EAGAIN && !wait_for(fd, POLLOUT, deadline)) return false;
    }
    return true;
}

bool address_of(const std::string& path, sockaddr_un& addr)
{
    addr = {};
    addr.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof addr.sun_path) return false;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return true;
}

}  // namespace

struct host::impl
{
    std::string path;
    int lock = -1;      // held while this serves: the claim
    int listener = -1;
    int wake[2] = {-1, -1};  // written to end the thread
    handler handle;
    std::chrono::milliseconds confirm_wait = k_confirm_wait;
    std::function<void()> after_answer;  // tests: a thread paused after answering
    std::thread thread;

    ~impl()
    {
        if (wake[1] >= 0) (void)!::write(wake[1], "x", 1);
        if (thread.joinable()) thread.join();
        // Gone before the claim is let go: the next host binds its own.
        if (listener >= 0)
        {
            ::close(listener);
            ::unlink(path.c_str());
        }
        for (int fd : {wake[0], wake[1], lock})
            if (fd >= 0) ::close(fd);
    }

    void serve_one(int c)
    {
        const auto deadline = clock::now() + k_serve_wait;
        unsigned char size[4];
        if (!read_all(c, size, 4, deadline)) return;
        const uint32_t n = length_of(size);
        if (n > k_max_message) return;
        std::string message(n, '\0');
        if (n && !read_all(c, message.data(), n, deadline)) return;
        const host::then_fn then = handle(std::move(message));
        if (!then)
        {
            write_all(c, std::string(1, '\0'), clock::now() + k_serve_wait);
            return;
        }
        // Taken: acted on only once the sender confirms. A sender that gave
        // up has hung up (or does at its timeout), which ends this at once.
        // A confirm already sent is read however late this gets to it
        // (read_all takes what's queued before minding the deadline).
        unsigned char confirm = 0;
        if (!write_all(c, std::string(1, '\1'), clock::now() + k_serve_wait)) return then(false);
        const auto confirm_by = clock::now() + confirm_wait;
        if (after_answer) after_answer();
        then(read_all(c, &confirm, 1, confirm_by) && confirm == 1);
    }

    void run()
    {
        for (;;)
        {
            pollfd p[2] = {{listener, POLLIN, 0}, {wake[0], POLLIN, 0}};
            const int r = ::poll(p, 2, -1);
            if (r < 0 && errno == EINTR) continue;
            if (r < 0 || p[1].revents) return;
            const int c = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (c < 0) continue;
            serve_one(c);
            ::close(c);
        }
    }
};

std::string endpoint(std::string_view name)
{
    const std::string dir = private_dir();
    if (dir.empty()) return {};
    // Per display too: a host's windows open on its own.
    std::string display;
    if (const char* w = std::getenv("WAYLAND_DISPLAY"); w && *w)
        display = w;
    else if (const char* d = std::getenv("DISPLAY"); d && *d)
        display = d;
    for (char& c : display)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '.') c = '_';
    return dir + "/" + std::string(name) + (display.empty() ? "" : "-" + display) + ".sock";
}

sent send(const std::string& endpoint, std::string_view message, std::chrono::milliseconds timeout)
{
    sockaddr_un addr;
    if (!address_of(endpoint, addr)) return sent::no_host;
    const int s = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (s < 0) return sent::failed;
    struct closer
    {
        int fd;
        ~closer() { ::close(fd); }
    } close_it{s};
    const auto deadline = clock::now() + timeout;
    while (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0)
    {
        // A socket left by a host that's gone refuses: no host.
        if (errno == ENOENT || errno == ECONNREFUSED) return sent::no_host;
        if (errno == EINPROGRESS)
        {
            int error = 0;
            socklen_t size = sizeof error;
            if (!wait_for(s, POLLOUT, deadline) ||
                ::getsockopt(s, SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error != 0)
                return sent::failed;
            break;
        }
        // The host's queue is full (Linux says so at once): try again
        // while there's time, as a pipe's sender waits for it.
        if (errno != EAGAIN || left_until(deadline).count() == 0) return sent::failed;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    unsigned char answer = 0;
    if (!write_all(s, frame(message), deadline) || !read_all(s, &answer, 1, deadline))
        return sent::failed;
    if (!answer) return sent::declined;
    // Taken in time: confirmed, or the host leaves it to this process.
    if (left_until(deadline).count() == 0 || !write_all(s, std::string(1, '\1'), deadline))
        return sent::failed;
    return sent::taken;
}

std::unique_ptr<host> host::claim(const std::string& endpoint, std::chrono::milliseconds confirm_wait)
{
    sockaddr_un addr;
    if (!address_of(endpoint, addr)) return nullptr;
    auto i = std::make_unique<impl>();
    i->confirm_wait = confirm_wait;
    i->path = endpoint;
    // The lock beside the socket says who hosts: of two processes claiming
    // at once, the other's fails here. A host that died lets it go.
    const std::string lock = endpoint + ".lock";
    i->lock = ::open(lock.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (i->lock < 0 || ::flock(i->lock, LOCK_EX | LOCK_NB) != 0) return nullptr;
    if (::pipe2(i->wake, O_CLOEXEC) != 0) return nullptr;
    i->listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (i->listener < 0) return nullptr;
    ::unlink(endpoint.c_str());  // a host before ours, gone (the lock says so)
    if (::bind(i->listener, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
        ::listen(i->listener, 16) != 0)
        return nullptr;
    // Senders can connect from here on (the queue holds them); they're
    // answered once serve() runs.
    return std::unique_ptr<host>(new host(std::move(i)));
}

#endif

void host::after_answer(std::function<void()> f)
{
    if (!impl_->thread.joinable()) impl_->after_answer = std::move(f);
}

void host::serve(handler handle)
{
    if (impl_->thread.joinable() || !handle) return;
    impl_->handle = std::move(handle);
    impl_->thread = std::thread([p = impl_.get()] { p->run(); });
}

host::host(std::unique_ptr<impl> i)
    : impl_(std::move(i))
{
}

host::~host() = default;

}  // namespace tortilla::single_instance
