#include "gravatar.h"

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
    return rap::sys::cache_dir("tortilla") / "gravatar";
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

gravatar::gravatar(rap::ui& ui)
    : ui_(ui)
    , thread_([this](std::stop_token stop) { run(stop); })
{
}

gravatar::~gravatar()
{
    thread_.request_stop();
    cancelled_ = true;
    wake_.notify_all();
}

void gravatar::request(const std::string& email, int px, result done)
{
    ++newest_;
    if (email.empty())
    {
        done_ = {};
        done(nullptr);
        return;
    }
    const std::string key = email + "\n" + std::to_string(px);
    if (auto it = cache_.find(key); it != cache_.end())
    {
        done_ = {};
        done(it->second);
        return;
    }
    done_ = std::move(done);
    std::lock_guard lock(mutex_);
    pending_ = job{key, email_hash(email), px, newest_};
    cancelled_ = true;  // the fetch under way, if any, is for an older request
    wake_.notify_all();
}

void gravatar::run(std::stop_token stop)
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
        // d=404: no made-up picture for an address without one.
        const auto r = rap::sys::http_get(
            "https://gravatar.com/avatar/" + j.hash + "?s=" + std::to_string(j.px) + "&d=404",
            {.timeout = 10s, .max_bytes = size_t(1) << 20, .cancelled = &cancelled_});
        if (cancelled_ || stop.stop_requested()) continue;
        std::error_code ec;
        fs::create_directories(cache_folder(), ec);
        if (r.status == 200 && !r.body.empty())
        {
            std::ofstream(picture, std::ios::binary) << r.body;
            finished(j, r.body);
        }
        else
        {
            // Only Gravatar's own "none" is remembered; a failure (offline)
            // is tried again next time.
            if (r.status == 404) std::ofstream touch(none);
            finished(j, std::nullopt);
        }
    }
}

// On the thread: decode, then hand over on the UI thread.
void gravatar::finished(const job& j, std::optional<std::string> bytes)
{
    std::shared_ptr<rap::sys::decoded_image> decoded;
    if (bytes)
        if (auto d = rap::sys::decode_image(
                {reinterpret_cast<const uint8_t*>(bytes->data()), bytes->size()}))
            decoded = std::make_shared<rap::sys::decoded_image>(std::move(*d));
    ui_.post(
        [this, alive = std::weak_ptr<bool>(alive_), j, decoded]
        {
            if (!alive.lock()) return;
            std::shared_ptr<rap::image> image;
            if (decoded)
                image = ui_.create_image(decoded->width, decoded->height, decoded->format,
                                         decoded->pixels.data(), size_t(decoded->width) * 4);
            cache_[j.key] = image;
            if (j.id == newest_ && done_) std::exchange(done_, {})(image);
        });
}

}  // namespace tortilla
