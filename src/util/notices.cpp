#include "notices.h"

#include <rapport/sys.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <fstream>
#include <iterator>
#include <random>
#include <sstream>
#include <system_error>

namespace tortilla
{
namespace
{

constexpr std::string_view header = "tortilla notice 1";

// One spelling per repository: absolute, its links resolved, no trailing
// separator, and on Windows any case.
std::string repo_key(const std::string& repo)
{
    std::error_code ec;
    std::filesystem::path p = rap::sys::path_from_utf8(repo);
    std::filesystem::path c = std::filesystem::weakly_canonical(p, ec);
    std::string key = rap::sys::path_to_utf8(ec ? p : c);
    std::ranges::replace(key, '\\', '/');
    while (key.size() > 1 && key.back() == '/') key.pop_back();
#ifdef _WIN32
    std::ranges::transform(key, key.begin(), [](char ch)
                           { return ch >= 'A' && ch <= 'Z' ? char(ch - 'A' + 'a') : ch; });
#endif
    return key;
}

// Sorts oldest first: the time in nanoseconds, padded, then a random part
// that two processes at once don't share.
std::string unique_name()
{
    static std::atomic<unsigned> counter{0};
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    std::random_device random;
    return std::format("{:020}-{:08x}{:08x}-{}", ns, random(), random(), counter++);
}

}  // namespace

std::filesystem::path notices_dir()
{
    return rap::sys::config_dir("tortilla") / "notices";
}

std::function<void(const std::filesystem::path&, const std::filesystem::path&, std::error_code&)>
    notice_rename = [](const std::filesystem::path& from, const std::filesystem::path& to,
                       std::error_code& ec) { std::filesystem::rename(from, to, ec); };

bool save_notice(const std::filesystem::path& dir, const std::string& repo, const std::string& text)
{
    std::error_code made;
    std::filesystem::create_directories(dir, made);  // a failure shows when writing
    const std::string name = unique_name();
    const std::filesystem::path aside = dir / (name + ".tmp"), in = dir / (name + ".notice");
    // Whatever failed, nothing half-written is left behind; tidying up never
    // makes a failure a success.
    auto give_up = [&]
    {
        std::error_code removed;
        std::filesystem::remove(aside, removed);
        return false;
    };
    {
        std::ofstream out(aside, std::ios::binary | std::ios::trunc);
        if (!out) return give_up();
        const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        out << header << '\n'
            << repo_key(repo) << '\n'
            << std::format("{:%F %T} UTC", now) << '\n'
            << text;
        if (!out.flush()) return give_up();
    }
    std::error_code renamed;
    notice_rename(aside, in, renamed);
    if (renamed) return give_up();
    return true;
}

std::vector<std::string> take_notices(const std::filesystem::path& dir, const std::string& repo)
{
    std::error_code listed;
    std::vector<std::filesystem::path> files;
    for (std::filesystem::directory_iterator it(dir, listed), end; !listed && it != end;
         it.increment(listed))
        if (it->path().extension() == ".notice") files.push_back(it->path());
    std::ranges::sort(files);
    const std::string key = repo_key(repo);
    std::vector<std::string> out;
    for (const std::filesystem::path& f : files)
    {
        std::string body;
        {
            std::ifstream in(f, std::ios::binary);
            if (!in) continue;
            body.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        std::string_view rest = body;
        auto line = [&]
        {
            const size_t end = rest.find('\n');
            std::string_view l = rest.substr(0, end);
            rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
            return l;
        };
        if (line() != header || line() != key) continue;
        // Taken by whoever removes it: another process may be here too.
        std::error_code removed;
        if (!std::filesystem::remove(f, removed) || removed) continue;
        out.emplace_back(rest);
    }
    return out;
}

}  // namespace tortilla
