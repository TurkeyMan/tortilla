#include "avatars.h"

#include "util/sha256.h"

#include <rapport/sys.h>

#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace tortilla
{
namespace
{

namespace fs = std::filesystem;
using namespace std::chrono_literals;

constexpr auto k_keep_picture = std::chrono::hours(24 * 7), k_keep_none = std::chrono::hours(24);

fs::path cache_folder()
{
    return rap::sys::cache_dir("tortilla") / "avatars";
}

// Gravatar's hash: of the address trimmed and lowercased.
std::string email_hash(const std::string& email)
{
    size_t first = 0, last = email.size();
    while (first < last && std::isspace(static_cast<unsigned char>(email[first]))) ++first;
    while (last > first && std::isspace(static_cast<unsigned char>(email[last - 1]))) --last;
    std::string s = email.substr(first, last - first);
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return sha256_hex(s);
}

bool fresh(const fs::path& p, std::chrono::hours keep)
{
    std::error_code ec;
    const auto when = fs::last_write_time(p, ec);
    return !ec && fs::file_time_type::clock::now() - when < keep;
}

}  // namespace

avatars::avatars(rap::ui& ui)
    : ui_(ui)
    , thread_([this](std::stop_token stop) { run(stop); })
{
}

avatars::~avatars()
{
    thread_.request_stop();
    cancelled_ = true;
    wake_.notify_all();
}

void avatars::request(author who, int px, result done)
{
    ++newest_;
    if (who.email.empty())
    {
        done_ = {};
        done(nullptr);
        return;
    }
    const std::string key = who.email + "\n" + std::to_string(px);
    if (auto it = cache_.find(key); it != cache_.end())
    {
        done_ = {};
        done(it->second);
        return;
    }
    done_ = std::move(done);
    std::lock_guard lock(mutex_);
    std::string hash = email_hash(who.email);
    pending_ = job{key, std::move(hash), px, newest_, std::move(who)};
    cancelled_ = true;  // the fetch under way, if any, is for an older request
    wake_.notify_all();
}

void avatars::run(std::stop_token stop)
{
    while (true)
    {
        job j;
        {
            std::unique_lock lock(mutex_);
            if (!wake_.wait(lock, stop, [this] { return pending_.has_value(); })) return;
            j = std::move(*pending_);
            pending_.reset();
            cancelled_ = false;
        }
        const fs::path base = cache_folder() / (j.hash + "-" + std::to_string(j.px));
        fs::path picture = base, none = base;
        picture += ".img";
        none += ".none";
        if (fresh(none, k_keep_none))
        {
            finished(j, std::nullopt);
            continue;
        }
        if (fresh(picture, k_keep_picture))
        {
            std::ifstream in(picture, std::ios::binary);
            finished(j, std::string(std::istreambuf_iterator<char>(in), {}));
            continue;
        }
        bool failed = false;
        auto bytes = fetch(j, failed);
        if (cancelled_ || stop.stop_requested()) continue;
        std::error_code ec;
        fs::create_directories(cache_folder(), ec);
        if (bytes)
            std::ofstream(picture, std::ios::binary) << *bytes;
        else if (!failed)
            std::ofstream touch(none);  // a failure (offline) is tried again next time
        finished(j, std::move(bytes));
    }
}

// On the thread, each way in turn; a way that answers "none" passes to the
// next, as does one that fails, but then "none" isn't remembered.
std::optional<std::string> avatars::fetch(const job& j, bool& failed)
{
    const rap::sys::http_options options{.headers = {{"User-Agent", "Tortilla"}},
                                         .timeout = 10s,
                                         .max_bytes = size_t(1) << 20,
                                         .cancelled = &cancelled_};
    auto get = [&](const std::string& url) -> std::optional<std::string>
    {
        auto r = rap::sys::http_get(url, options);
        if (r.status == 200 && !r.body.empty()) return std::move(r.body);
        if (r.status != 404) failed = true;
        return std::nullopt;
    };
    // A GitHub noreply address names its account: no address sent anywhere.
    if (const std::string url = noreply_avatar_url(j.who.email, j.px); !url.empty())
        if (auto bytes = get(url)) return bytes;
    if (cancelled_) return std::nullopt;
    // d=404: no made-up picture for an address without one.
    if (auto bytes =
            get("https://gravatar.com/avatar/" + j.hash + "?s=" + std::to_string(j.px) + "&d=404"))
        return bytes;
    // The repository's own forges, each host once, the leading remote's.
    if (j.who.commit.empty() || j.who.remotes.empty()) return std::nullopt;
    for (const forge_remote& r : leading_remotes(j.who.workdir, j.who.remotes))
    {
        if (cancelled_) return std::nullopt;
        const std::string key = r.account.host + ' ' + r.account.login;
        auto known = tokens_.find(key);
        if (known == tokens_.end())
        {
            auto t = load_token(r.account);
            known = tokens_.emplace(key, t ? std::optional(t->access) : std::nullopt).first;
        }
        if (!known->second) continue;  // no token: as good as no account
        auto bytes = author_avatar(r.account.kind, r.repo, j.who.commit, j.who.email, j.px,
                                   *known->second, &cancelled_);
        if (!bytes)
        {
            failed = true;
            continue;
        }
        if (!bytes->empty()) return std::move(*bytes);
    }
    return std::nullopt;
}

// On the thread: decode, then hand over on the UI thread.
void avatars::finished(const job& j, std::optional<std::string> bytes)
{
    std::shared_ptr<rap::sys::decoded_image> decoded;
    if (bytes)
        if (auto d = rap::sys::decode_image(
                {reinterpret_cast<const uint8_t*>(bytes->data()), bytes->size()}))
            decoded = std::make_shared<rap::sys::decoded_image>(std::move(*d));
    ui_.post(
        [this, alive = std::weak_ptr<bool>(alive_), key = j.key, id = j.id, decoded]
        {
            if (!alive.lock()) return;
            std::shared_ptr<rap::image> image;
            if (decoded)
                image = ui_.create_image(decoded->width, decoded->height, decoded->format,
                                         decoded->pixels.data(), size_t(decoded->width) * 4);
            cache_[key] = image;
            if (id == newest_ && done_) std::exchange(done_, {})(image);
        });
}

}  // namespace tortilla
