// A local channel between Tortilla's processes, one line asked and one
// answered per connection: a named pipe on Windows, a Unix socket in the
// user's runtime folder elsewhere. Only the user's own processes reach it.
// The asking side is small enough for the file manager extension: no
// libgit2, no toolkit, and a time limit on every question.
#pragma once

#include <chrono>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace tortilla::channel
{

// How long a connection has to send its line, and again to take its
// answer: then it's let go, whether it has or not. The same on every
// platform, so an asker that stalls costs the same everywhere.
constexpr auto k_serve_wait = std::chrono::milliseconds(500);
// Connections served at once, each on its own thread: a stalled one holds
// up only itself, and only this many stalled at once hold up the rest
// (until k_serve_wait lets them go).
constexpr int k_serve_clients = 8;

// Serves `name` on this thread until the process ends: each request (a
// line, without its newline) gets `answer`'s line back. `answer` is called
// from several threads at once. An error if the channel can't be made.
std::expected<void, std::string> serve(std::string_view name,
                                       const std::function<std::string(std::string_view)>& answer);

enum class ask_error
{
    no_server,  // nothing serves it: start the server
    failed,     // busy, slow or broken: try again later
};

// Asks `name` one line, waiting at most `timeout` in all.
std::expected<std::string, ask_error> ask(std::string_view name, std::string_view request,
                                          std::chrono::milliseconds timeout);

// Where `name` is served, in UTF-8: the socket's path, or the pipe's name.
// For tests, which connect to it as a misbehaving asker would.
std::string address(std::string_view name);

// Whether this is the only process holding `name`'s lock, which it keeps
// until it ends: so only one server runs per user.
bool hold_single(std::string_view name);

}  // namespace tortilla::channel
