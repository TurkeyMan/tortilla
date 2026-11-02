#include "raw_client.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

#include <thread>

namespace tortilla::test
{

#ifdef _WIN32

namespace
{

HANDLE handle(intptr_t h)
{
    return reinterpret_cast<HANDLE>(h);
}

}  // namespace

raw_client::raw_client(const std::string& address)
    : handle_(reinterpret_cast<intptr_t>(INVALID_HANDLE_VALUE))
{
    std::wstring w(
        size_t(MultiByteToWideChar(CP_UTF8, 0, address.data(), int(address.size()), nullptr, 0)),
        L'\0');
    MultiByteToWideChar(CP_UTF8, 0, address.data(), int(address.size()), w.data(), int(w.size()));
    // A plain handle, not overlapped: this client blocks as a careless one would.
    for (int tries = 0; tries < 2; ++tries)
    {
        HANDLE h = CreateFileW(w.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               0, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            handle_ = reinterpret_cast<intptr_t>(h);
            return;
        }
        if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(w.c_str(), 5000)) return;
    }
}

raw_client::~raw_client()
{
    if (connected()) CloseHandle(handle(handle_));
}

bool raw_client::connected() const
{
    return handle(handle_) != INVALID_HANDLE_VALUE;
}

bool raw_client::send(std::string_view data)
{
    DWORD wrote = 0;
    return connected() &&
           WriteFile(handle(handle_), data.data(), DWORD(data.size()), &wrote, nullptr) &&
           wrote == data.size();
}

bool raw_client::let_go(std::chrono::milliseconds within)
{
    // Peeking doesn't block, and fails once the server has disconnected
    // (ERROR_BROKEN_PIPE or ERROR_PIPE_NOT_CONNECTED).
    const auto until = std::chrono::steady_clock::now() + within;
    while (connected())
    {
        DWORD available = 0;
        if (!PeekNamedPipe(handle(handle_), nullptr, 0, nullptr, &available, nullptr)) return true;
        if (std::chrono::steady_clock::now() >= until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

#else

raw_client::raw_client(const std::string& address)
    : handle_(-1)
{
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (address.size() >= sizeof addr.sun_path) return;
    std::memcpy(addr.sun_path, address.c_str(), address.size() + 1);
    const int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) return;
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0)
    {
        close(s);
        return;
    }
    handle_ = s;
}

raw_client::~raw_client()
{
    if (connected()) close(int(handle_));
}

bool raw_client::connected() const
{
    return handle_ >= 0;
}

bool raw_client::send(std::string_view data)
{
    return connected() &&
           ::send(int(handle_), data.data(), data.size(), MSG_NOSIGNAL) == ssize_t(data.size());
}

bool raw_client::let_go(std::chrono::milliseconds within)
{
    if (!connected()) return false;
    pollfd p{int(handle_), POLLIN, 0};
    if (poll(&p, 1, int(within.count())) != 1) return false;
    char c;
    const ssize_t n = recv(int(handle_), &c, 1, MSG_DONTWAIT);
    return n == 0 || (n < 0 && errno == ECONNRESET);
}

#endif

}  // namespace tortilla::test
