// Every log window in one process: the first `tortilla log` hosts them, and
// a later one hands its request over and ends (single_instance.h). A host
// that doesn't take it in time (hung, or ending) leaves the window to the
// process that asked, so no request is lost; and one it takes is opened
// only once its sender has confirmed it's leaving it to the host, so none
// is opened twice. TORTILLA_NO_LOG_HOST=1 turns it off: each log its own
// process, as other commands are.
#pragma once

#include "util/single_instance.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace tortilla
{

// What a `tortilla log` was asked: its path made absolute (log_request_path),
// so the host never reads it from its own folder.
struct log_request
{
    std::string path;
    std::string select;  // --select: the commit to open on
    bool all_branches = false;
    bool submodule = false;  // --submodule: the repository around the submodule at path
    bool welcome = false;    // nothing was given: the welcome window if there's no repository
    // In a log window of its own rather than a tab (log.openIn, as the
    // sender read it: the host read its settings as it started, and Settings
    // may have changed them since, in another process).
    bool new_window = false;

    bool operator==(const log_request&) const = default;
};

// The request as a message: each field its length (four bytes,
// little-endian) and its bytes, after a tag naming the format; the flags
// (all_branches, submodule, welcome, new_window) one field, a '0' or '1'
// each.
std::string encode(const log_request&);
// nullopt for anything that isn't a whole request in that format.
std::optional<log_request> decode(std::string_view);

// `path` as the asker's folder makes it, for a host in another folder:
// made absolute and nothing more. Not normalised: "link/.." is the folder
// above the link's target, not the folder the link is in, and a "." or a
// separator at the end is read as the asker would read it (every reader,
// log_window::open, repo_relative, use_repo_icon, makes it absolute first,
// which leaves an absolute path as it is). `path` itself if that fails.
std::string log_request_path(const std::string& path);

// The timing, per request. Only how soon a request falls back to its own
// window hangs on these: whatever times out where, the two-phase answer
// (single_instance.h) leaves each request opened once, by the host or by
// its sender.
//
// One attempt at handing a request over, all of it from its start: waiting
// behind other senders to reach the host, sending, and the host's answer.
constexpr auto k_log_forward_wait = std::chrono::milliseconds(2000);
// All attempts together, for a sender whose host isn't answering yet (one
// that started at the same moment, still loading its own log) or is ending
// (a sender that finds it gone hosts in its place).
constexpr auto k_log_start_wait = std::chrono::milliseconds(3000);
// How long the host's thread waits for the UI thread to say whether it
// takes a request; less than the asker waits, so a busy UI thread usually
// answers in time. Past it the request is turned away.
constexpr auto k_log_host_ui_wait = std::chrono::milliseconds(1500);

// Where a log handed over to the host opens (Settings > General > Log,
// log.openIn): a tab of the log window used last (log_window::
// open_forwarded), or a window of its own. The first `tortilla log`'s own
// log, the host's, has a window of its own either way.
enum class log_open_in
{
    tab,
    window,
};
log_open_in log_open_in_setting();
void set_log_open_in_setting(log_open_in);

// False when TORTILLA_NO_LOG_HOST says so.
bool log_host_enabled();

// The log windows' host's endpoint, for this user, session and display.
std::string log_host_endpoint();

// One attempt at handing the request to the host: true when it has taken
// it (and been told so), and this process has nothing left to do.
bool forward_to_log_host(const log_request&, const std::string& endpoint = log_host_endpoint(),
                         std::chrono::milliseconds wait = k_log_forward_wait);

class log_host;

// How a `tortilla log` begins: its request handed over (forwarded), or the
// endpoint claimed, this process the host (host set, not serving yet), or
// neither (both null: a log of its own, standalone).
struct log_start
{
    bool forwarded = false;
    std::unique_ptr<log_host> host;
};

// Hands the request over, or claims the endpoint when no one serves it.
// Lost a claim to a process starting at the same moment: tries that one
// again, until `budget` has gone (k_log_start_wait), then gives up.
log_start forward_or_claim(const log_request&, const std::string& endpoint = log_host_endpoint(),
                           std::chrono::milliseconds budget = k_log_start_wait);

class log_host
{
public:
    // Runs a function on the UI thread (rap::ui::post); safe from any thread.
    using post_fn = std::function<void(std::function<void()>)>;

    // The endpoint, this process's from now on; nullptr when another
    // process has it. Requests wait until serve().
    static std::unique_ptr<log_host> claim(const std::string& endpoint = log_host_endpoint());

    // Answers requests, once the UI is ready for them. Each is posted to the
    // UI thread, where `take` says whether it's taken (and holds whatever
    // opening it needs: the process mustn't end meanwhile). Each `take`
    // that says yes is followed, on the UI thread, by `open` once its
    // sender confirms, or `drop` if it doesn't (it opens its own).
    void serve(post_fn post, std::function<bool()> take,
               std::function<void(const log_request&)> open, std::function<void()> drop);

private:
    std::unique_ptr<single_instance::host> host_;
};

}  // namespace tortilla
