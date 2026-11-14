#include "log_host.h"

#include <rapport/sys.h>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace tortilla
{

namespace
{

// Names the format: a message from another version is turned away (and its
// sender opens the window itself) rather than misread.
constexpr std::string_view k_tag = "tortilla-log/2";

void put(std::string& out, std::string_view field)
{
    const auto n = uint32_t(field.size());
    for (int shift = 0; shift < 32; shift += 8) out += char((n >> shift) & 0xff);
    out += field;
}

// The fields in order, or nullopt if `in` isn't made only of them.
std::optional<std::vector<std::string_view>> fields(std::string_view in)
{
    std::vector<std::string_view> out;
    while (!in.empty())
    {
        if (in.size() < 4) return std::nullopt;
        uint32_t n = 0;
        for (int i = 3; i >= 0; --i) n = n << 8 | static_cast<unsigned char>(in[size_t(i)]);
        in.remove_prefix(4);
        if (n > in.size()) return std::nullopt;
        out.push_back(in.substr(0, n));
        in.remove_prefix(n);
    }
    return out;
}

}  // namespace

std::string encode(const log_request& r)
{
    std::string out;
    put(out, k_tag);
    put(out, r.path);
    put(out, r.select);
    const char flags[] = {r.all_branches ? '1' : '0', r.submodule ? '1' : '0',
                          r.welcome ? '1' : '0'};
    put(out, std::string_view(flags, sizeof flags));
    return out;
}

std::optional<log_request> decode(std::string_view in)
{
    const auto f = fields(in);
    if (!f || f->size() != 4 || (*f)[0] != k_tag) return std::nullopt;
    const std::string_view flags = (*f)[3];
    if (flags.size() != 3) return std::nullopt;
    for (char c : flags)
        if (c != '0' && c != '1') return std::nullopt;
    return log_request{.path = std::string((*f)[1]),
                       .select = std::string((*f)[2]),
                       .all_branches = flags[0] == '1',
                       .submodule = flags[1] == '1',
                       .welcome = flags[2] == '1'};
}

std::string log_request_path(const std::string& path)
{
    std::error_code ec;
    const std::filesystem::path at = std::filesystem::absolute(rap::sys::path_from_utf8(path), ec);
    return ec ? path : rap::sys::path_to_utf8(at);
}

bool log_host_enabled()
{
    const char* off = std::getenv("TORTILLA_NO_LOG_HOST");
    return !off || !*off || std::string_view(off) == "0";
}

std::string log_host_endpoint()
{
    return single_instance::endpoint("log");
}

bool forward_to_log_host(const log_request& r, const std::string& endpoint,
                         std::chrono::milliseconds wait)
{
    return single_instance::send(endpoint, encode(r), wait) == single_instance::sent::taken;
}

log_start forward_or_claim(const log_request& r, const std::string& endpoint,
                           std::chrono::milliseconds budget)
{
    using clock = std::chrono::steady_clock;
    if (endpoint.empty()) return {};
    const std::string message = encode(r);
    const auto give_up = clock::now() + budget;
    for (;;)
    {
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(give_up - clock::now());
        // Not taken, nothing opened there: trying again can't open it twice.
        switch (single_instance::send(endpoint, message,
                                      std::clamp(left, std::chrono::milliseconds(1), k_log_forward_wait)))
        {
        case single_instance::sent::taken:
            return {.forwarded = true};
        case single_instance::sent::no_host:
            // No one's: this process's, unless another claims it first.
            if (auto host = log_host::claim(endpoint)) return {.host = std::move(host)};
            break;
        case single_instance::sent::declined:  // ending: soon gone, and then ours
        case single_instance::sent::failed:    // not answering yet
            break;
        }
        if (clock::now() + std::chrono::milliseconds(50) >= give_up) return {};
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

std::unique_ptr<log_host> log_host::claim(const std::string& endpoint)
{
    auto host = single_instance::host::claim(endpoint);
    if (!host) return nullptr;
    auto h = std::unique_ptr<log_host>(new log_host);
    h->host_ = std::move(host);
    return h;
}

void log_host::serve(post_fn post, std::function<bool()> take,
                     std::function<void(const log_request&)> open, std::function<void()> drop)
{
    // On the host's thread: the request goes to the UI thread, and the
    // answer waits for that to take it. Taken there, under the lock, or
    // given up on here, never both: a UI thread that comes to it late
    // takes nothing, and the asker has been told no.
    auto handle = [post = std::move(post), take = std::move(take), open = std::move(open),
                   drop = std::move(drop)](std::string message) -> single_instance::host::then_fn
    {
        auto request = decode(message);
        if (!request) return {};
        struct state
        {
            std::mutex m;
            std::condition_variable cv;
            bool decided = false, taken = false, abandoned = false;
        };
        auto s = std::make_shared<state>();
        post(
            [s, take]
            {
                {
                    std::lock_guard lock(s->m);
                    if (s->abandoned) return;
                    s->decided = true;
                    s->taken = take();
                }
                s->cv.notify_all();
            });
        {
            std::unique_lock lock(s->m);
            if (!s->cv.wait_for(lock, k_log_host_ui_wait, [&] { return s->decided; }))
            {
                s->abandoned = true;
                return {};
            }
            if (!s->taken) return {};
        }
        // Opened once the asker confirms, which it does at once unless it
        // has given up meanwhile (and opens its own): then only what `take`
        // held goes. Opening runs after the answer, as it can take a while
        // (a repository on a slow disk, a question about trusting it).
        return [post, open, drop, r = std::move(*request)](bool confirmed)
        {
            if (confirmed)
                post([open, r] { open(r); });
            else
                post(drop);
        };
    };
    host_->serve(std::move(handle));
}

}  // namespace tortilla
