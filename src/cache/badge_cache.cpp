#include "badge_cache.h"

#include <rapport/sys.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace tortilla
{

namespace fs = std::filesystem;
using clock = std::chrono::steady_clock;

namespace
{

// A change is loaded after things have been quiet this long (on top of the
// watch's own settling), so a checkout or build is one load, not hundreds.
constexpr auto k_quiet = std::chrono::milliseconds(500);
// A repository nobody has asked about for this long is let go, watch and all.
constexpr auto k_forget = std::chrono::minutes(10);
// How long where a folder's repository is (or that it has none) is believed.
constexpr auto k_root_memo = std::chrono::seconds(30);
// Paths remembered per repository, to say which changed: beyond this, a
// load reports none (the file manager redraws what it shows soon enough).
constexpr size_t k_max_asked = 100000;
// A repository's summary older than this is refreshed when its folder is
// shown (the stale one shown meanwhile).
constexpr auto k_summary_stale = std::chrono::minutes(2);

struct repo_entry
{
    fs::path root;
    std::shared_ptr<const repo_badges> badges;  // null until first loaded
    std::unordered_set<std::string> asked;      // repository-relative
    std::unique_ptr<rap::sys::directory_watch> watch;
    clock::time_point used;
};

std::string key(const fs::path& p)
{
    return rap::sys::path_to_utf8(p);
}

// `path` inside `root`, '/'-separated as git has it; "" for root itself.
std::string repo_relative(const fs::path& path, const fs::path& root)
{
    std::string rel = rap::sys::path_to_utf8(path.lexically_relative(root));
    std::ranges::replace(rel, '\\', '/');
    return rel == "." ? std::string() : rel;
}

// Under .git, what changes without changing any file's status: objects,
// logs, locks, fetch bookkeeping.
bool ignorable(const fs::path& root, const fs::path& changed)
{
    const std::string rel = repo_relative(changed, root);
    if (!rel.starts_with(".git/")) return false;
    const std::string_view in = std::string_view(rel).substr(5);
    return in.starts_with("objects/") || in.starts_with("logs/") || in.ends_with(".lock") ||
           in == "FETCH_HEAD" || in == "ORIG_HEAD" || in == "gitk.cache";
}

}  // namespace

struct badge_cache::impl
{
    std::function<void(const std::vector<fs::path>&)> changed;
    std::mutex m;
    std::condition_variable wake;
    std::unordered_map<std::string, std::shared_ptr<repo_entry>> repos;  // by root
    struct root_memo
    {
        std::optional<fs::path> root;
        clock::time_point at;
    };
    std::unordered_map<std::string, root_memo> roots;  // by folder
    std::map<std::string, clock::time_point> due;      // roots to load, and when
    // Each repository met: its top folder's badge, and when that was learnt
    // (seconds since the epoch, so it means something across runs).
    struct summary
    {
        badge b = badge::none;
        int64_t at = 0;
    };
    std::unordered_map<std::string, summary> summaries;    // by root
    std::map<std::string, clock::time_point> due_summary;  // roots to summarise
    fs::path summaries_file;
    std::atomic<clock::rep> last_ask{clock::now().time_since_epoch().count()};
    bool stopping = false;
    std::thread worker;

    // The working tree holding `folder` (itself, if it has a .git), or
    // nullopt; inside a .git folder there's none.
    std::optional<fs::path> root_of(const fs::path& folder)
    {
        const std::string k = key(folder);
        const auto now = clock::now();
        if (auto it = roots.find(k); it != roots.end() && now - it->second.at < k_root_memo)
            return it->second.root;
        std::optional<fs::path> found;
        std::error_code ec;
        for (fs::path d = folder; !d.empty(); d = d.parent_path())
        {
            if (d.filename() == ".git") break;
            if (fs::exists(d / ".git", ec))
            {
                found = d;
                break;
            }
            if (d == d.parent_path()) break;
        }
        if (roots.size() > 50000) roots.clear();
        roots[k] = {found, now};
        return found;
    }

    static int64_t seconds_now()
    {
        return std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    // "<badge> <seconds> <root>" a line each.
    void load_summaries()
    {
        std::ifstream in(summaries_file, std::ios::binary);
        for (std::string line; std::getline(in, line);)
        {
            int b = 0;
            long long at = 0;
            int used = 0;
            if (std::sscanf(line.c_str(), "%d %lld %n", &b, &at, &used) == 2 && used > 0 && b > 0 &&
                b <= int(badge::added))
                summaries[line.substr(size_t(used))] = {badge(b), at};
        }
    }

    // Written aside, then moved over the old: never half a file.
    void save_summaries() const
    {
        std::error_code ec;
        fs::create_directories(summaries_file.parent_path(), ec);
        const fs::path temp = summaries_file.string() + ".new";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            for (const auto& [root, s] : summaries)
                out << int(s.b) << ' ' << s.at << ' ' << root << '\n';
            if (!out) return;
        }
        fs::rename(temp, summaries_file, ec);
    }

    // Learns a repository's summary: true if it differs from what was known.
    bool learn(const std::string& root, badge b)
    {
        summary& s = summaries[root];
        const bool differs = s.b != b;
        s = {b, seconds_now()};
        save_summaries();
        return differs;
    }

    void schedule(const std::string& root, clock::time_point when)
    {
        auto [it, added] = due.emplace(root, when);
        if (!added) it->second = std::max(it->second, when);
        wake.notify_one();
    }

    void watch(repo_entry& e)
    {
        const std::string root = key(e.root);
        auto w = rap::sys::directory_watch::start(
            e.root, {.recursive = true, .settle = std::chrono::milliseconds(300)},
            [this, root, at = e.root](rap::sys::watch_batch batch)
            {
                bool matters = batch.overflow;
                for (const auto& ev : batch.events)
                    matters = matters || !ignorable(at, ev.path) ||
                              (!ev.old_path.empty() && !ignorable(at, ev.old_path));
                if (!matters) return;
                std::lock_guard lock(m);
                if (repos.contains(root)) schedule(root, clock::now() + k_quiet);
            });
        if (w) e.watch = std::move(*w);
    }

    state look(const fs::path& path)
    {
        last_ask = clock::now().time_since_epoch().count();
        std::error_code ec;
        const bool dir = fs::is_directory(path, ec);
        std::lock_guard lock(m);
        const auto root = root_of(dir ? path : path.parent_path());
        if (!root) return {};
        const std::string k = key(*root);
        const std::string rel = repo_relative(path, *root);
        // Its own folder, from outside: the summary, unless it's loaded.
        auto loaded = repos.find(k);
        if (rel.empty() && (loaded == repos.end() || !loaded->second->badges))
        {
            const auto known = summaries.find(k);
            if ((known == summaries.end() ||
                 seconds_now() - known->second.at >
                     std::chrono::duration_cast<std::chrono::seconds>(k_summary_stale).count()) &&
                !due_summary.contains(k) && loaded == repos.end())
            {
                due_summary.emplace(k, clock::now());
                wake.notify_one();
            }
            if (loaded != repos.end()) loaded->second->asked.emplace(rel);
            return {known != summaries.end() ? known->second.b : badge::none};
        }
        auto& e = repos[k];
        if (!e)
        {
            e = std::make_shared<repo_entry>();
            e->root = *root;
            watch(*e);
            schedule(k, clock::now());
        }
        e->used = clock::now();
        if (e->asked.size() < k_max_asked) e->asked.emplace(rel);
        if (!e->badges) return {};
        return {e->badges->of(rel), !due.contains(k), e->badges->untracked(rel)};
    }

    void run()
    {
        std::unique_lock lock(m);
        while (!stopping)
        {
            // Let go of repositories nobody asks about: their watches end
            // outside the lock, which a watch's callback may be waiting for.
            const auto now = clock::now();
            std::vector<std::shared_ptr<repo_entry>> gone;
            std::erase_if(repos,
                          [&](const auto& r)
                          {
                              if (now - r.second->used <= k_forget) return false;
                              gone.push_back(r.second);
                              return true;
                          });
            if (!gone.empty())
            {
                lock.unlock();
                gone.clear();
                lock.lock();
                continue;
            }
            auto next = std::ranges::min_element(due, {}, [](const auto& d) { return d.second; });
            const bool load_due = next != due.end() && next->second <= now;
            // Summaries wait for loads: a repository being worked in first.
            if (!load_due && !due_summary.empty())
            {
                const std::string root = due_summary.begin()->first;
                due_summary.erase(due_summary.begin());
                if (repos.contains(root)) continue;  // being loaded: that tells it
                lock.unlock();
                auto b = load_summary(root);
                lock.lock();
                if (b && learn(root, *b) && changed)
                {
                    lock.unlock();
                    changed({rap::sys::path_from_utf8(root)});
                    lock.lock();
                }
                continue;
            }
            if (next == due.end())
            {
                wake.wait_for(lock, std::chrono::minutes(1));
                continue;
            }
            if (!load_due)
            {
                wake.wait_until(lock, next->second);
                continue;
            }
            const std::string root = next->first;
            due.erase(next);
            auto entry = repos.contains(root) ? repos[root] : nullptr;
            if (!entry) continue;

            lock.unlock();
            auto loaded = load_badges(rap::sys::path_to_utf8(entry->root));
            lock.lock();
            if (!loaded) continue;  // not a working tree after all, or git failed
            auto fresh = std::make_shared<const repo_badges>(std::move(*loaded));
            std::vector<fs::path> differ;
            for (const std::string& rel : entry->asked)
                if ((entry->badges ? entry->badges->of(rel) : badge::none) != fresh->of(rel))
                    differ.push_back(rel.empty() ? entry->root
                                                 : entry->root / rap::sys::path_from_utf8(rel));
            const bool had = entry->badges != nullptr;
            const badge top = fresh->of("");
            if (top != badge::none) learn(root, top);
            entry->badges = std::move(fresh);
            // Its folder showed the summary until now.
            if (!had && entry->asked.contains("") &&
                std::ranges::find(differ, entry->root) == differ.end())
                differ.push_back(entry->root);
            if (!differ.empty() && changed)
            {
                lock.unlock();
                changed(differ);
                lock.lock();
            }
        }
    }
};

badge_cache::badge_cache(std::function<void(const std::vector<fs::path>&)> changed,
                         fs::path summaries)
    : impl_(std::make_unique<impl>())
{
    impl_->changed = std::move(changed);
    impl_->summaries_file =
        summaries.empty() ? rap::sys::cache_dir("tortilla") / "repositories" : summaries;
    impl_->load_summaries();
    impl_->worker = std::thread([i = impl_.get()] { i->run(); });
}

badge_cache::~badge_cache()
{
    {
        std::lock_guard lock(impl_->m);
        impl_->stopping = true;
    }
    impl_->wake.notify_one();
    impl_->worker.join();
    // The watches end outside the lock, their callbacks finding no repositories.
    decltype(impl_->repos) repos;
    {
        std::lock_guard lock(impl_->m);
        repos.swap(impl_->repos);
    }
}

badge badge_cache::ask(const fs::path& p)
{
    return impl_->look(p).b;
}

badge_cache::state badge_cache::look(const fs::path& p)
{
    return impl_->look(p);
}

std::chrono::steady_clock::duration badge_cache::idle() const
{
    return clock::now() - clock::time_point(clock::duration(impl_->last_ask.load()));
}

}  // namespace tortilla
