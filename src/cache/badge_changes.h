// The paths whose badges changed, for the file manager extensions that must
// redraw them themselves: Explorer is told by the cache (SHChangeNotify),
// the Linux file managers only by their extension, which asks tortilla cache
// every so often what changed since it last asked:
//
//   changes 1 <run> <next>     asked: the protocol's version (1), the run
//                              last heard from ("-" for none) and the
//                              number of the first change not yet heard
//   <run> <next> <all>[<TAB><path>]...
//                              answered: this run, the number to ask from
//                              next, and the paths changed since (UTF-8);
//                              <all> 1 when they can't all be said (another
//                              run, or more than are kept), so everything
//                              is drawn again
//
// A cache that doesn't know the request answers "?", and the extension
// keeps to what it has. Asking doesn't count as a question for the cache's
// idle time: a file manager left open doesn't keep it running.
#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tortilla
{

class badge_changes
{
public:
    // `keep`: the most recent changes remembered; an asker further behind
    // is told to draw everything again.
    explicit badge_changes(size_t keep = 4096);

    // From the cache's thread: paths in UTF-8.
    void add(const std::vector<std::string>& paths);

    // The answer to "changes <rest>", `rest` being what follows "changes ".
    std::string answer(std::string_view rest) const;

    std::string_view run() const { return run_; }

private:
    mutable std::mutex m_;
    std::string run_;                // this run's, so an asker sees a restart
    uint64_t next_ = 1;              // the next change's number
    std::deque<std::string> paths_;  // changes next_ - size() to next_ - 1
    size_t keep_;
};

// The extension's side: what it asks, and what the answer says.
struct changes_heard
{
    std::string run;
    uint64_t next = 0;
    bool all = false;
    std::vector<std::string_view> paths;  // slices of the answer
};
std::string changes_request(std::string_view run, uint64_t next);
// nullopt for an answer that isn't one ("?" from an older cache).
std::optional<changes_heard> read_changes(std::string_view answer);

}  // namespace tortilla
