#include "log_host_tests.h"

#include "git/repo.h"
#include "log/log_host.h"
#include "settings.h"
#include "test_support.h"
#include "ui/window_identities.h"
#include "util/repo_path.h"
#include "util/single_instance.h"

#include <rapport/sys.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <thread>
#include <vector>

namespace tortilla::test
{
namespace
{

using namespace std::chrono_literals;
namespace si = single_instance;

// An endpoint no other run (or the user's own Tortilla) is serving.
std::string test_endpoint(const char* what)
{
    std::random_device rd;
    return si::endpoint(std::string("log-test-") + what + "-" + std::to_string(rd()));
}

// A UI thread of its own, running what's posted to it in order, as
// rap::ui::post does.
struct fake_ui
{
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::function<void()>> queue;
    bool done = false;
    std::thread thread{[this] { run(); }};

    ~fake_ui()
    {
        {
            std::lock_guard lock(m);
            done = true;
        }
        cv.notify_all();
        thread.join();
    }
    void post(std::function<void()> f)
    {
        {
            std::lock_guard lock(m);
            queue.push_back(std::move(f));
        }
        cv.notify_all();
    }
    void run()
    {
        for (;;)
        {
            std::unique_lock lock(m);
            cv.wait(lock, [&] { return done || !queue.empty(); });
            if (queue.empty()) return;
            auto f = std::move(queue.front());
            queue.pop_front();
            lock.unlock();
            f();
        }
    }
};

void encoding_tests()
{
    const log_request r{.path = "C:\\Users\\\xc3\xa9t\xc3\xa9\\repo with spaces\n2",
                        .select = "",
                        .all_branches = true,
                        .submodule = false,
                        .welcome = true,
                        .new_window = true};
    const std::string m = encode(r);
    check(decode(m) == r, "log host: a request decodes as it was encoded");
    const log_request plain{};
    check(decode(encode(plain)) == plain, "log host: an empty request too");
    std::string with_nul = encode({.path = std::string("a\0b", 3)});
    check(decode(with_nul) && decode(with_nul)->path == std::string("a\0b", 3),
          "log host: any bytes in a field");

    bool none = true;
    for (size_t cut = 0; cut < m.size(); ++cut) none = none && !decode(m.substr(0, cut));
    check(none, "log host: a request cut short at any byte isn't one");
    check(!decode(m + "x") && !decode(m + std::string("\1\0\0\0x", 5)),
          "log host: nor one with anything after it");
    std::string other = m;
    check(other.substr(4, 14) == "tortilla-log/3", "log host: the format is tortilla-log/3");
    other[4 + 13] = '2';  // tortilla-log/3 -> tortilla-log/2, which had no new_window
    check(!decode(other), "log host: nor another format's");
    std::string flags = m;
    flags[flags.size() - 1] = 'y';
    check(!decode(flags), "log host: nor one whose flags aren't 0 or 1");
    check(!decode("") && !decode(std::string("\xff\xff\xff\xff", 4)),
          "log host: nor nothing, or a length past the end");
}

// log.openIn: a tab unless it says a window, which is all it keeps.
void open_in_setting_tests()
{
    settings& s = settings::get();
    const std::string was = s.value("log.openIn");
    s.remove("log.openIn");
    check(log_open_in_setting() == log_open_in::tab, "log host: new logs open in a tab, unset");
    set_log_open_in_setting(log_open_in::window);
    check(log_open_in_setting() == log_open_in::window && s.value("log.openIn") == "window",
          "log host: set to a window, kept as one");
    set_log_open_in_setting(log_open_in::tab);
    check(log_open_in_setting() == log_open_in::tab && s.value("log.openIn", "unset") == "unset",
          "log host: back to a tab, the default, nothing kept");
    s.set("log.openIn", "elsewhere");
    check(log_open_in_setting() == log_open_in::tab, "log host: a value it doesn't know: a tab");
    if (was.empty())
        s.remove("log.openIn");
    else
        s.set("log.openIn", was);
}

// The path handed over: absolute, and otherwise as given, so a link
// followed by ".." isn't folded away; a "." at the end is read as the asker
// would read it.
void path_tests(const std::string& repo)
{
    namespace fs = std::filesystem;
    const fs::path link_up = rap::sys::path_from_utf8(log_request_path("a/link/.."));
    std::error_code ec;
#ifdef _WIN32
    // Windows reads a path's ".." by its text, links or not (absolute() is
    // GetFullPathName, as every open): the folder the asker would open.
    const bool as_read = link_up == fs::absolute("a", ec);
#else
    const bool as_read = link_up.filename() == ".." && link_up.parent_path().filename() == "link";
#endif
    check(link_up.is_absolute() && as_read,
          "log host: a path's \"link/..\" is sent as the asker reads it, not folded by text");
    const std::string whole = log_request_path(repo);
    check(log_request_path(whole) == whole, "log host: an absolute path is sent as it is");
    const std::string dot = log_request_path(repo + "/.");
    auto r = repo::open(dot);
    check(r && repo_relative(r->workdir(), dot) == std::optional<std::string>(""),
          "log host: \"<repository>/.\" opens the repository, its whole log");
    // A folder that's there, then "..": the repository, opened as it is.
    fs::create_directories(rap::sys::path_from_utf8(repo + "/sub-real"), ec);
    auto r2 = repo::open(log_request_path(repo + "/sub-real/.."));
    check(r && r2 && r2->workdir() == r->workdir(),
          "log host: \"<repository>/sub/..\" (sub there) is the repository too");
    // One that isn't: never opened as it is (POSIX can't, through a folder
    // that's missing); the log walks up to what's there, and repo_relative
    // reads it as the whole repository.
    check(r && repo_relative(r->workdir(), log_request_path(repo + "/sub-none/..")) ==
                   std::optional<std::string>(""),
          "log host: \"<repository>/missing/..\" is the whole repository's log");
}

void transport_tests()
{
    const std::string at = test_endpoint("transport");
    check(!at.empty(), "single instance: the user has an endpoint");
    check(si::send(at, "hello", 500ms) == si::sent::no_host,
          "single instance: nothing served: no host, at once");

    std::mutex m;
    std::vector<std::string> got;
    std::vector<bool> confirmed;
    std::atomic<bool> take{true};
    std::atomic<int> delay_ms{0};
    auto handler = [&](std::string message) -> si::host::then_fn
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms.load()));
        if (!take) return {};
        return [&, message = std::move(message)](bool yes)
        {
            std::lock_guard lock(m);
            got.push_back(message);
            confirmed.push_back(yes);
        };
    };
    auto host = si::host::claim(at);
    check(host != nullptr, "single instance: the first claim hosts");
    check(si::host::claim(at) == nullptr, "single instance: a second claim doesn't");

    // Claimed, not serving yet: a sender waits, and gives up at its time.
    const auto start = std::chrono::steady_clock::now();
    const si::sent early = si::send(at, "early", 300ms);
    const auto took = std::chrono::steady_clock::now() - start;
    check(early == si::sent::failed && took >= 250ms && took < 1500ms,
          "single instance: claimed but not serving: the sender waits, then gives up");
    host->serve(handler);

    const std::string big(200 * 1024, 'x');
    check(si::send(at, "hello", 2000ms) == si::sent::taken &&
              si::send(at, "", 2000ms) == si::sent::taken &&
              si::send(at, big, 2000ms) == si::sent::taken,
          "single instance: messages taken");
    // The sender is done once its confirm is written; the host acts once
    // it has read it, a moment later.
    auto wait_for_confirmed = [&](size_t n)
    {
        for (int i = 0; i < 300; ++i)
        {
            {
                std::lock_guard lock(m);
                if (size_t(std::ranges::count(confirmed, true)) >= n) return;
            }
            std::this_thread::sleep_for(10ms);
        }
    };
    wait_for_confirmed(3);
    {
        std::lock_guard lock(m);
        // The early one, gone before serving began: never seen (a pipe
        // drops it), or seen and not confirmed (a socket's queue kept it).
        std::vector<std::string> acted;
        bool early_unconfirmed = true;
        for (size_t i = 0; i < got.size(); ++i)
            if (confirmed[i])
                acted.push_back(got[i]);
            else
                early_unconfirmed = early_unconfirmed && got[i] == "early";
        check(acted.size() == 3 && acted[0] == "hello" && acted[1].empty() && acted[2] == big &&
                  early_unconfirmed,
              "single instance: each whole, as sent, and confirmed; the one given up on isn't");
        if (!(acted.size() == 3 && early_unconfirmed))
            for (size_t i = 0; i < got.size(); ++i)
                std::printf("     got %zu bytes \"%.20s\", %s\n", got[i].size(), got[i].c_str(),
                            confirmed[i] ? "confirmed" : "not confirmed");
        got.clear();
        confirmed.clear();
    }
    take = false;
    check(si::send(at, "no", 2000ms) == si::sent::declined, "single instance: one turned away");

    // A sender that gives up before its answer: the host is told it wasn't
    // confirmed, and the sender that it failed.
    take = true;
    delay_ms = 600;
    const si::sent late = si::send(at, "late", 300ms);
    for (int i = 0; i < 200; ++i)
    {
        {
            std::lock_guard lock(m);
            if (!got.empty()) break;
        }
        std::this_thread::sleep_for(10ms);
    }
    delay_ms = 0;
    {
        std::lock_guard lock(m);
        const bool ok = late == si::sent::failed && got == std::vector<std::string>{"late"} &&
                        confirmed == std::vector<bool>{false};
        check(ok, "single instance: a sender gone before the answer: failed there, unconfirmed here");
        if (!ok)
            for (size_t i = 0; i < got.size(); ++i)
                std::printf("     got \"%.20s\", %s\n", got[i].c_str(),
                            confirmed[i] ? "confirmed" : "not confirmed");
        got.clear();
        confirmed.clear();
    }

    // Several at once: served one at a time, every one answered.
    std::atomic<int> taken{0};
    std::vector<std::thread> senders;
    for (int i = 0; i < 6; ++i)
        senders.emplace_back(
            [&, i]
            {
                if (si::send(at, "sender " + std::to_string(i), 3000ms) == si::sent::taken)
                    ++taken;
            });
    for (std::thread& t : senders) t.join();
    check(taken == 6, "single instance: senders at once all taken");

    host.reset();
    check(si::send(at, "after", 500ms) == si::sent::no_host,
          "single instance: a host gone leaves no host behind");
    host = si::host::claim(at);
    if (host) host->serve(handler);
    check(host && si::send(at, "again", 2000ms) == si::sent::taken,
          "single instance: and the next claim hosts again");
}

// A host whose thread pauses after answering, past the confirm's deadline:
// the confirm the sender wrote in time is still read, and the message acted
// on (the sender has gone, believing it taken).
void stalled_confirm_tests()
{
    const std::string at = test_endpoint("stalled");
    auto host = si::host::claim(at, 200ms);
    check(host != nullptr, "single instance: claims, with a short confirm wait");
    if (!host) return;
    std::mutex m;
    std::condition_variable cv;
    std::optional<bool> confirmed;
    host->after_answer([] { std::this_thread::sleep_for(700ms); });
    host->serve(
        [&](std::string) -> si::host::then_fn
        {
            return [&](bool yes)
            {
                {
                    std::lock_guard lock(m);
                    confirmed = yes;
                }
                cv.notify_all();
            };
        });
    const si::sent sent = si::send(at, "stalled", 2000ms);
    std::unique_lock lock(m);
    cv.wait_for(lock, 3s, [&] { return confirmed.has_value(); });
    check(sent == si::sent::taken && confirmed == true,
          "single instance: a host paused past the confirm's deadline still reads it, and acts");
}

// A log_host on a fake UI thread, counting what it takes, opens and drops.
struct served
{
    std::optional<fake_ui> ui{std::in_place};
    std::mutex m;
    std::map<std::string, int> opened;  // by the request's select
    std::atomic<int> takes{0}, drops{0};
    std::chrono::milliseconds open_takes{0};
    std::unique_ptr<log_host> host;

    void serve(std::function<bool()> take = [] { return true; })
    {
        host->serve([this](std::function<void()> f) { ui->post(std::move(f)); },
                    [this, take]
                    {
                        if (!take()) return false;
                        ++takes;
                        return true;
                    },
                    [this](const log_request& r)
                    {
                        std::this_thread::sleep_for(open_takes);
                        std::lock_guard lock(m);
                        ++opened[r.select];
                    },
                    [this] { ++drops; });
    }
    // Everything the host has posted, run.
    void finish()
    {
        host.reset();  // its thread joined: every answer's open or drop posted
        ui.reset();    // and run
    }
    int opened_of(const std::string& select)
    {
        std::lock_guard lock(m);
        auto it = opened.find(select);
        return it == opened.end() ? 0 : it->second;
    }
    int total_opened()
    {
        std::lock_guard lock(m);
        int n = 0;
        for (auto& [k, v] : opened) n += v;
        return n;
    }
};

void host_tests()
{
    const std::string at = test_endpoint("host");
    const log_request asked{.path = "/repos/a", .select = "abc123"};

    // Taken on the UI thread, then opened there.
    {
        fake_ui ui;
        std::mutex m;
        std::vector<log_request> opened;
        std::thread::id ui_thread, open_thread;
        ui.post([&] { ui_thread = std::this_thread::get_id(); });
        auto host = log_host::claim(at);
        check(host != nullptr, "log host: claims");
        host->serve([&](std::function<void()> f) { ui.post(std::move(f)); }, [] { return true; },
                    [&](const log_request& r)
                    {
                        std::lock_guard lock(m);
                        opened.push_back(r);
                        open_thread = std::this_thread::get_id();
                    },
                    [] {});
        check(forward_to_log_host(asked, at), "log host: a request forwarded is taken");
        for (int i = 0; i < 100; ++i)
        {
            {
                std::lock_guard lock(m);
                if (!opened.empty()) break;
            }
            std::this_thread::sleep_for(10ms);
        }
        {
            std::lock_guard lock(m);
            check(opened.size() == 1 && opened[0] == asked && open_thread == ui_thread,
                  "log host: and opened on the UI thread, as asked");
        }
        check(si::send(at, "not a request", 2000ms) == si::sent::declined,
              "log host: a message that isn't a request isn't taken");
    }

    // A host that's ending turns it away, and opens nothing.
    {
        served s;
        s.host = log_host::claim(at);
        check(s.host != nullptr, "log host: claims again");
        s.serve([] { return false; });
        check(!forward_to_log_host(asked, at), "log host: an ending host turns it away");
        s.finish();
        check(s.total_opened() == 0 && s.takes == 0, "log host: and opens nothing");
    }

    // A UI thread that's hung: the asker is told no in time to open the
    // window itself, and when the UI thread comes to it at last it takes
    // nothing, so there aren't two.
    {
        std::mutex m;
        std::vector<std::function<void()>> stuck;
        std::atomic<int> opened{0}, takes{0};
        auto host = log_host::claim(at);
        check(host != nullptr, "log host: claims a third time");
        if (host)
            host->serve(
                [&](std::function<void()> f)
                {
                    std::lock_guard lock(m);
                    stuck.push_back(std::move(f));
                },
                [&]
                {
                    ++takes;
                    return true;
                },
                [&](const log_request&) { ++opened; }, [] {});
        const auto start = std::chrono::steady_clock::now();
        const bool forwarded = forward_to_log_host(asked, at);
        const auto took = std::chrono::steady_clock::now() - start;
        check(!forwarded && took < k_log_forward_wait + 500ms,
              "log host: a hung UI thread's host doesn't take it, within the wait");
        host.reset();
        std::lock_guard lock(m);
        for (auto& f : stuck) f();
        check(stuck.size() == 1 && takes == 0 && opened == 0,
              "log host: nor takes or opens it when the UI thread wakes");
    }

    check(forward_to_log_host(asked, at) == false, "log host: no host, nothing forwarded");
}

// Requests queued behind a busy UI thread and a slow open (codex's case on
// #13: UI busy 1.35 s, the first open 1.1 s, a second request 100 ms after
// the first): each is opened exactly once, by the host if its sender was
// told it's taken, else by the sender alone.
void queued_tests()
{
    served s;
    const std::string at = test_endpoint("queued");
    s.host = log_host::claim(at);
    check(s.host != nullptr, "log host: claims for the queue");
    if (!s.host) return;
    s.open_takes = 1100ms;
    s.ui->post([] { std::this_thread::sleep_for(1350ms); });
    s.serve();
    // At 0 and 100 ms (codex's two), more while the first opens, and one
    // late enough to be taken after it.
    const std::vector<std::chrono::milliseconds> starts{0ms, 100ms, 200ms, 300ms, 1000ms, 2000ms};
    std::vector<char> taken(starts.size(), 0);
    std::vector<std::thread> senders;
    for (size_t i = 0; i < starts.size(); ++i)
        senders.emplace_back(
            [&, i]
            {
                std::this_thread::sleep_for(starts[i]);
                taken[i] = forward_to_log_host({.path = "/repos/q", .select = std::to_string(i)}, at);
            });
    for (std::thread& t : senders) t.join();
    s.finish();
    bool once = true;
    std::string seen;
    for (size_t i = 0; i < starts.size(); ++i)
    {
        const int n = s.opened_of(std::to_string(i));
        once = once && n == (taken[i] ? 1 : 0);
        seen += std::string(" ") + (taken[i] ? "taken/" : "own/") + std::to_string(n);
    }
    std::printf("     (queued, per request, the sender's answer/the host's opens:%s)\n", seen.c_str());
    check(once, "log host: queued behind a slow open, each request opened once, by host or sender");
    check(taken[0] && taken[starts.size() - 1],
          "log host: the first, and one after the slow open, are the host's");
    check(s.takes == s.total_opened() + s.drops,
          "log host: every request taken is opened or dropped, once");
}

// Starting together, and a host that isn't ready.
void start_tests()
{
    const log_request asked{.path = "/repos/s", .select = "s"};

    // Two at once: one claims and hosts once its UI is ready; the other
    // hands its request to it.
    {
        served s;
        const std::string at = test_endpoint("race");
        std::mutex m;
        int hosted = 0, forwarded = 0, neither = 0;
        auto start = [&]
        {
            log_start r = forward_or_claim(asked, at);
            if (r.host)
            {
                std::this_thread::sleep_for(400ms);  // loading its UI, opening its own log
                std::lock_guard lock(m);
                ++hosted;
                s.host = std::move(r.host);
                s.serve();
                return;
            }
            std::lock_guard lock(m);
            ++(r.forwarded ? forwarded : neither);
        };
        std::thread a(start), b(start);
        a.join();
        b.join();
        s.finish();
        check(hosted == 1 && forwarded == 1 && neither == 0 && s.total_opened() == 1,
              "log start: of two at once, one hosts and the other's log is its");
    }

    // A host that never gets ready: given up on within the budget, to open
    // its own; and what waited meanwhile isn't opened when it does serve.
    {
        served s;
        const std::string at = test_endpoint("unready");
        s.host = log_host::claim(at);
        const auto start = std::chrono::steady_clock::now();
        log_start r = forward_or_claim(asked, at, 1000ms);
        const auto took = std::chrono::steady_clock::now() - start;
        check(s.host && !r.forwarded && !r.host && took >= 900ms && took < 1600ms,
              "log start: a host that isn't ready is given up on within the budget");
        if (!s.host) return;
        s.serve();
        check(forward_to_log_host({.path = "/repos/s", .select = "later"}, at),
              "log start: once it serves, a later request is taken");
        s.finish();
        check(s.total_opened() == 1 && s.opened_of("later") == 1,
              "log start: and only that one is opened, not the one given up on");
    }

    // A host that's ending: once it's gone, the asker hosts in its place.
    {
        served s;
        const std::string at = test_endpoint("ending");
        s.host = log_host::claim(at);
        if (s.host) s.serve([] { return false; });
        std::thread end(
            [&]
            {
                std::this_thread::sleep_for(300ms);
                s.finish();
            });
        log_start r = forward_or_claim(asked, at);
        end.join();
        check(!r.forwarded && r.host != nullptr, "log start: an ending host's asker hosts after it");
    }

    const auto quick = std::chrono::steady_clock::now();
    log_start none = forward_or_claim(asked, "");
    check(!none.forwarded && !none.host && std::chrono::steady_clock::now() - quick < 500ms,
          "log start: no endpoint: neither, at once");
}

// Windows take their parent's repository, whichever was opened since; each
// repository's colour is held while any window, or new windows' default,
// has it.
void identity_tests()
{
    int released_a = 0, released_b = 0, released_c = 0;
    auto lease = [](int& released)
    { return std::shared_ptr<void>(new int(0), [&released](int* p) { delete p; ++released; }); };
    window_identities ids;
    // A's is the icon's own colours ("plain": no group of its own), B's a colour.
    ids.use(std::make_shared<repo_identity>(
        repo_identity{.repo = "/a", .app_id = "", .lease = lease(released_a)}));
    int wa = 0, wb = 0, wa_dialog = 0, wa_dialog2 = 0, wb_dialog = 0, wa_nested = 0, other = 0,
        loose = 0, later = 0;
    check(ids.created(&wa, nullptr)->repo == "/a", "identity: A's log is A's");
    ids.use(std::make_shared<repo_identity>(repo_identity{
        .repo = "/b", .app_id = "Tortilla.Repository.b", .lease = lease(released_b)}));
    check(released_a == 0, "identity: A's colour held by its window when B's opens");
    check(ids.created(&wb, nullptr)->repo == "/b", "identity: B's log is B's");
    const bool plain_a = [&]
    {
        const auto d = ids.created(&wa_dialog, &wa);
        return d->repo == "/a" && d->app_id.empty();
    }();
    check(plain_a,
          "identity: a dialog of A's log, after B's opened, is A's, plain (Tortilla's group)");
    check(ids.created(&wb_dialog, &wb)->app_id == "Tortilla.Repository.b",
          "identity: a dialog of B's is B's");
    check(ids.created(&wa_nested, &wa_dialog)->repo == "/a", "identity: a dialog's dialog too");
    check(ids.created(&loose, &other)->repo == "/b",
          "identity: a parent not made here: the latest repository's");
    check(ids.created(&wa_dialog2, &wa)->repo == "/a", "identity: and A's again");

    // A's colour goes with its last window; B's stays while new windows take it.
    for (int* w : {&wa_nested, &wa_dialog, &wa_dialog2}) ids.destroyed(w);
    check(released_a == 0 && ids.of(&wa) && ids.of(&wa)->repo == "/a",
          "identity: A's held while its log is open");
    ids.destroyed(&wa);
    check(released_a == 1 && !ids.of(&wa), "identity: and let go with it, once");
    for (int* w : {&wb, &wb_dialog, &loose}) ids.destroyed(w);
    check(released_b == 0, "identity: B's held while it's what new windows take");
    check(ids.created(&later, nullptr)->repo == "/b", "identity: (a window made now is B's)");
    ids.use(nullptr);
    check(released_b == 0, "identity: and while a window has it");
    ids.destroyed(&later);
    check(released_b == 1 && ids.size() == 0, "identity: then let go, once, and nothing's left");
    check(!ids.created(&wa, nullptr), "identity: no repository: none (Tortilla's own)");
    ids.destroyed(&wa);

    // Two windows of one repository share its colour: held until both close.
    int w1 = 0, w2 = 0;
    ids.use(std::make_shared<repo_identity>(repo_identity{.repo = "/c", .lease = lease(released_c)}));
    ids.created(&w1, nullptr);
    ids.created(&w2, nullptr);
    ids.use(nullptr);
    ids.destroyed(&w1);
    check(released_c == 0, "identity: a repository's colour held by its other window");
    ids.destroyed(&w2);
    check(released_c == 1, "identity: and let go with the last");
}

// A log's own actions open windows without a parent (Commit, Rebase, a log
// of the repository): in the log's scope they're its repository's, after
// another repository's log has opened (codex's case on #13: A, then B,
// then Commit from A's log). Through window_identities::created, as
// create_window makes every window (track_window, with a real window, in
// tracked_window_tests).
void identity_scope_tests()
{
    int released_a = 0;
    window_identities ids;
    auto a = std::make_shared<repo_identity>(repo_identity{
        .repo = "/a", .lease = std::shared_ptr<void>(new int(0), [&](int* p) { delete p; ++released_a; })});
    auto b = std::make_shared<repo_identity>(repo_identity{.repo = "/b", .app_id = "Tortilla.Repository.b"});
    int log_a = 0, log_b = 0, commit = 0, rebase = 0, progress = 0, follow_up = 0, after = 0,
        unknown = 0, stranger = 0, none_log = 0, from_none = 0;
    ids.use(a);
    ids.created(&log_a, nullptr);
    ids.use(b);
    ids.created(&log_b, nullptr);
    a.reset();

    // A's context menu, made while B is the latest: its actions, submenus'
    // too, open A's windows when chosen.
    std::string commit_repo, rebase_repo;
    rap::menu m;
    m.push_back({.label = "Commit", .action = [&] { commit_repo = ids.created(&commit, nullptr)->repo; }});
    rap::menu_item sub{.label = "More"};
    sub.children.push_back(
        {.label = "Rebase", .action = [&] { rebase_repo = ids.created(&rebase, nullptr)->repo; }});
    m.push_back(std::move(sub));
    m = in_identity_scope(ids, ids.recorded(&log_a), std::move(m));
    m[0].action();
    m[1].children[0].action();
    check(commit_repo == "/a" && rebase_repo == "/a",
          "identity scope: Commit and Rebase from A's log, after B's opened, are A's");
    check(ids.created(&after, nullptr)->repo == "/b",
          "identity scope: and outside the scope, the latest repository's again");

    // A named action with a scope of its own, and what it opens later: a
    // progress dialog (a child) whose follow-up opens a window without a
    // parent in the dialog's scope.
    {
        const identity_scope scope(ids, ids.recorded(&log_a));
        ids.created(&progress, &log_a);
    }
    {
        const identity_scope later(ids, ids.recorded(&progress));
        check(ids.created(&follow_up, nullptr)->repo == "/a",
              "identity scope: a follow-up opened later from A's progress dialog is A's");
    }

    // Nested: the inner scope wins, and the outer comes back.
    {
        const identity_scope outer(ids, ids.recorded(&log_b));
        {
            const identity_scope inner(ids, ids.recorded(&log_a));
            check(ids.created(&unknown, nullptr)->repo == "/a", "identity scope: the inner scope's");
        }
        check(ids.created(&unknown, nullptr)->repo == "/b", "identity scope: then the outer's again");
    }
    // An action whose window opens later, from a posted callback (Compare
    // change sets: a worker writes the patches, then the diff opens), after
    // the action's own scope has returned: re-entered, A's; not, the latest.
    {
        int diff = 0, bare = 0;
        std::string scoped_repo, bare_repo;
        std::deque<std::function<void()>> posted;
        {
            const identity_scope action(ids, ids.recorded(&log_a));
            posted.push_back(in_identity_scope(ids, ids.recorded(&log_a),
                                               [&] { scoped_repo = ids.created(&diff, nullptr)->repo; }));
            posted.push_back([&] { bare_repo = ids.created(&bare, nullptr)->repo; });
        }
        for (auto& f : posted) f();
        check(scoped_repo == "/a",
              "identity scope: a window a posted callback opens after the action returned is A's");
        check(bare_repo == "/b", "identity scope: (without the scope carried, it was the latest's)");
        posted.clear();
        ids.destroyed(&diff);
        ids.destroyed(&bare);
    }
    // A window not recorded opens no scope; a window of no repository's
    // scope gives Tortilla's own, not the latest repository's.
    {
        const identity_scope none(ids, ids.recorded(&stranger));
        check(ids.created(&unknown, nullptr)->repo == "/b",
              "identity scope: a window not recorded leaves things as they are");
    }
    ids.use(nullptr);
    ids.created(&none_log, nullptr);
    ids.use(b);
    {
        const identity_scope scope(ids, ids.recorded(&none_log));
        check(!ids.created(&from_none, nullptr), "identity scope: a window of none opens none");
    }

    // A's colour still goes with its last window (and the menu, closed:
    // its actions hold A's identity while it's open).
    m.clear();
    for (int* w : {&commit, &rebase, &progress, &follow_up}) ids.destroyed(w);
    check(released_a == 0, "identity scope: A's held by its log");
    ids.destroyed(&log_a);
    check(released_a == 1, "identity scope: and let go with it");
}

// create_window's record (track_window) goes with the real window: rapport
// destroys it, its on_destroyed lets the record go, and the repository's
// colour with the last of its windows, once each. A dialog takes its
// parent's.
void tracked_window_tests(rap::ui& ui)
{
    int released = 0;
    window_identities ids;
    ids.use(std::make_shared<repo_identity>(repo_identity{
        .repo = "/t", .lease = std::shared_ptr<void>(new int(0), [&](int* p) { delete p; ++released; })}));
    auto log = ui.create({.title = "log"});
    auto other = ui.create({.title = "other"});
    if (!log || !other)
    {
        check(false, "tracked window: the null backend makes windows");
        return;
    }
    check(track_window(ids, *log, nullptr)->repo == "/t", "tracked window: the log is its repository's");
    ids.use(nullptr);
    auto dialog = ui.create({.title = "dialog", .dialog = true}, log.get());
    if (!dialog)
    {
        check(false, "tracked window: the null backend makes dialogs");
        return;
    }
    const bool dialog_parents = [&]
    {
        auto id = track_window(ids, *dialog, log.get());
        return id && id->repo == "/t";
    }();
    check(dialog_parents,
          "tracked window: a dialog is its parent's, after the latest changed");
    check(!track_window(ids, *other, nullptr), "tracked window: one made now is of none");
    check(ids.size() == 3, "tracked window: all three recorded");

    const void* dialog_at = dialog.get();
    dialog.reset();
    check(ids.size() == 2 && !ids.recorded(dialog_at) && released == 0,
          "tracked window: a dialog destroyed goes; its repository's colour is its parent's still");
    const void* at = log.get();
    log.reset();
    check(!ids.recorded(at) && ids.size() == 1 && released == 1,
          "tracked window: the log destroyed goes, and lets its colour go, once");
    other.reset();
    check(ids.size() == 0 && released == 1, "tracked window: nothing left, nothing let go twice");
}

}  // namespace

void run_log_host_tests(rap::ui& ui, const std::string& repo)
{
    encoding_tests();
    open_in_setting_tests();
    path_tests(repo);
    transport_tests();
    stalled_confirm_tests();
    host_tests();
    queued_tests();
    start_tests();
    identity_tests();
    identity_scope_tests();
    tracked_window_tests(ui);
}

}  // namespace tortilla::test
