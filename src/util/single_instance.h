// One process serving the rest: the first to claim an endpoint hosts it,
// and later ones send it their message and end once it's taken. A named
// pipe on Windows, a Unix socket in the user's runtime folder elsewhere;
// only the user's own processes, in their own session (and on Linux their
// own display), reach it.
//
// A message goes as its length (four bytes, little-endian) and its bytes;
// the host answers one byte, taken or not. Taken, the sender confirms with
// one byte of its own, and only then does the host act on the message: a
// sender that gave up before the answer reached it never confirms, so its
// message is either acted on by the host or left to the sender, never both.
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace tortilla::single_instance
{

// The endpoint called `name`, for this user: the pipe's name, or the
// socket's path, in UTF-8. Empty where there's no private place for it
// (a runtime folder someone else owns): then nothing hosts or is sent.
std::string endpoint(std::string_view name);

enum class sent
{
    taken,     // the host has it: this process is done
    declined,  // the host turned it away (it's ending)
    no_host,   // nothing serves the endpoint
    failed,    // a host that didn't answer in time, or isn't the user's
};

// Sends `message` to the host of `endpoint`, `timeout` covering all of it
// from this call: reaching the host (behind other senders), the message
// and the answer. Taken in time, the confirm is written at once, and only
// a confirm written whole is `taken`; anything less (an answer late or
// lost, a confirm that couldn't be written) is `failed`, and the host
// hasn't acted on it. On Windows the host may bring its windows to the
// front, as this process could have.
sent send(const std::string& endpoint, std::string_view message, std::chrono::milliseconds timeout);

// How long a host waits for a sender's confirm after answering "taken":
// longer than any sender's `timeout`, so a confirm written in time is read.
// A sender that gives up hangs up, which ends the wait at once.
constexpr auto k_confirm_wait = std::chrono::milliseconds(5000);

class host
{
public:
    // What a taken message leads to: called once, on the host's thread,
    // with whether its sender confirmed. True: act on it now. False: the
    // sender gave up (or went), and does it itself.
    using then_fn = std::function<void(bool confirmed)>;
    // Called on the host's own thread, a message at a time: an empty
    // function declines it, any other takes it. The sender is waiting
    // (send's timeout), so it's prompt.
    using handler = std::function<then_fn(std::string message)>;

    // Makes `endpoint` this process's, for senders to reach from now on;
    // nullptr when another process has it already (or it can't be made).
    // Two processes claiming at once: one gets it. Nothing is answered
    // until serve(): senders wait meanwhile, and give up at their timeout.
    // `confirm_wait`: how long a confirm is waited for (tests shorten it).
    static std::unique_ptr<host> claim(const std::string& endpoint,
                                       std::chrono::milliseconds confirm_wait = k_confirm_wait);

    // For tests, before serve(): run on the host's thread after it has
    // answered "taken" and before it reads the confirm, as a host thread
    // paused there (past the confirm's deadline) would be.
    void after_answer(std::function<void()>);

    // Answers senders with `handle`, on a thread of its own, until
    // destroyed. Once: a later call does nothing.
    void serve(handler handle);

    // Stops serving: a message being handled is finished first, and the
    // endpoint goes, so a later sender finds no host rather than a dead one.
    ~host();
    host(const host&) = delete;
    host& operator=(const host&) = delete;

private:
    struct impl;
    explicit host(std::unique_ptr<impl>);
    std::unique_ptr<impl> impl_;
};

}  // namespace tortilla::single_instance
