// A connection to a channel's address (channel::address) that does only
// what it's told: for testing askers that misbehave, connecting and sending
// nothing, or asking and never reading the answer. Kept apart from the
// tests, so they don't take in <windows.h>.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace tortilla::test
{

class raw_client
{
public:
    // Connects, waiting a while for the pipe if every instance is taken.
    explicit raw_client(const std::string& address);
    ~raw_client();
    raw_client(const raw_client&) = delete;
    raw_client& operator=(const raw_client&) = delete;

    bool connected() const;
    // All of `data`, without waiting for the server to read it.
    bool send(std::string_view data);
    // Whether the server hangs up within this long: for a client that asked
    // nothing, so has nothing to read first.
    bool let_go(std::chrono::milliseconds within);

private:
    intptr_t handle_;
};

}  // namespace tortilla::test
