#include "badge_changes.h"

#include <charconv>
#include <chrono>
#include <random>

namespace tortilla
{

namespace
{

// At most this many paths in one answer: an asker this far behind draws
// everything again instead.
constexpr size_t k_max_answer = 4096;

// The next word of `s`, up to a space, taken off it.
std::string_view word(std::string_view& s)
{
    const size_t space = s.find(' ');
    const std::string_view w = s.substr(0, space);
    s.remove_prefix(space == std::string_view::npos ? s.size() : space + 1);
    return w;
}

std::optional<uint64_t> number(std::string_view s)
{
    uint64_t v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc() || r.ptr != s.data() + s.size()) return std::nullopt;
    return v;
}

}  // namespace

badge_changes::badge_changes(size_t keep)
    : keep_(keep)
{
    // The time and a random number: two runs never share one.
    std::random_device rd;
    run_ = std::to_string(std::chrono::system_clock::now().time_since_epoch() /
                          std::chrono::milliseconds(1)) +
           "-" + std::to_string(rd() & 0xffff);
}

void badge_changes::add(const std::vector<std::string>& paths)
{
    std::lock_guard lock(m_);
    for (const std::string& p : paths)
    {
        paths_.push_back(p);
        ++next_;
    }
    while (paths_.size() > keep_) paths_.pop_front();
}

std::string badge_changes::answer(std::string_view rest) const
{
    std::lock_guard lock(m_);
    std::string out = run_ + " " + std::to_string(next_);
    const std::string_view version = word(rest), run = word(rest);
    const auto from = number(word(rest));
    if (version != "1" || !from) return out + " 1";
    // An asker new to the cache hears everything of this run; one of
    // another run, everything again.
    uint64_t since = *from;
    if (run == "-")
        since = 1;
    else if (run != run_)
        return out + " 1";
    const uint64_t first = next_ - paths_.size();
    if (since > next_) return out + " 1";  // not a number this run gave
    if (since < first || next_ - since > k_max_answer) return out + " 1";
    out += " 0";
    for (uint64_t i = since; i < next_; ++i)
    {
        const std::string& p = paths_[size_t(i - first)];
        // A path the line can't carry: everything again, to be sure.
        if (p.find_first_of("\t\n") != std::string::npos)
            return run_ + " " + std::to_string(next_) + " 1";
        out += '\t';
        out += p;
    }
    return out;
}

std::string changes_request(std::string_view run, uint64_t next)
{
    return "changes 1 " + std::string(run.empty() ? "-" : run) + " " + std::to_string(next);
}

std::optional<changes_heard> read_changes(std::string_view answer)
{
    const size_t tab = answer.find('\t');
    std::string_view head = answer.substr(0, tab);
    changes_heard out;
    out.run = word(head);
    const auto next = number(word(head));
    const std::string_view all = word(head);
    if (out.run.empty() || out.run == "?" || !next || (all != "0" && all != "1"))
        return std::nullopt;
    out.next = *next;
    out.all = all == "1";
    for (std::string_view rest = tab == std::string_view::npos ? "" : answer.substr(tab + 1);
         !rest.empty();)
    {
        const size_t t = rest.find('\t');
        out.paths.push_back(rest.substr(0, t));
        rest.remove_prefix(t == std::string_view::npos ? rest.size() : t + 1);
    }
    return out;
}

}  // namespace tortilla
