#include "ref_journal.h"

#include "i18n.h"

#include <git2.h>

#include <cstring>

namespace tortilla
{

void ref_journal::record(std::string label, history_edit e)
{
    if (e.moves.empty()) return;
    undone_.clear();
    done_.push_back({std::move(label), std::move(e)});
    while (done_.size() > limit_) done_.pop_front();
}

std::optional<history_edit> ref_journal::undo_edit() const
{
    if (done_.empty()) return std::nullopt;
    return done_.back().edit.undo();
}

std::optional<history_edit> ref_journal::redo_edit() const
{
    if (undone_.empty()) return std::nullopt;
    return undone_.back().edit;
}

void ref_journal::undone()
{
    if (done_.empty()) return;
    undone_.push_back(std::move(done_.back()));
    done_.pop_back();
}

void ref_journal::redone()
{
    if (undone_.empty()) return;
    done_.push_back(std::move(undone_.back()));
    undone_.pop_back();
    while (done_.size() > limit_) done_.pop_front();
}

void ref_journal::forget_undo()
{
    if (!done_.empty()) done_.pop_back();
}

void ref_journal::forget_redo()
{
    if (!undone_.empty()) undone_.pop_back();
}

std::optional<oid> ref_target(const repo& repository, const std::string& full_name)
{
    git_oid id;
    if (git_reference_name_to_id(&id, repository.raw(), full_name.c_str()) != 0)
        return std::nullopt;
    oid out;
    std::memcpy(out.bytes.data(), id.id, out.bytes.size());
    return out;
}

std::optional<std::string> why_stale(const repo& repository, const history_edit& e)
{
    for (const ref_move& m : e.moves)
    {
        const std::optional<oid> now = ref_target(repository, m.ref);
        if (now == m.from) continue;
        std::string_view name = m.ref;
        for (std::string_view p : {"refs/heads/", "refs/tags/", "refs/remotes/"})
            if (name.starts_with(p))
            {
                name.remove_prefix(p.size());
                break;
            }
        if (!m.from) return rap::i18n::format(_("{0} exists already."), std::string(name));
        if (!now) return rap::i18n::format(_("{0} has been deleted since."), std::string(name));
        return rap::i18n::format(_("{0} has moved since."), std::string(name));
    }
    return std::nullopt;
}

}  // namespace tortilla
