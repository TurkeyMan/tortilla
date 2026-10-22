#include "repo_worker.h"

namespace tortilla
{

RepoWorker::RepoWorker(rap::Ui& ui, std::string path)
    : ui_(ui)
{
    thread_ = std::thread(
        [&ui, state = state_, path = std::move(path)]
        {
            auto repo = Repo::open(path);
            for (;;)
            {
                Job job;
                unsigned gen;
                {
                    std::unique_lock lock(state->mutex);
                    state->wake.wait(lock, [&] { return state->stop || state->pending; });
                    if (state->stop) return;
                    job = std::move(state->pending);
                    state->pending = nullptr;
                    gen = state->latest;
                }
                if (!repo) continue;
                const Progress progress(ui, state, gen);
                if (std::function<void()> apply = job(*repo, progress)) progress.post(std::move(apply));
            }
        });
}

bool RepoWorker::Progress::stale() const
{
    std::lock_guard lock(state_->mutex);
    return state_->stop || gen_ != state_->latest;
}

void RepoWorker::Progress::post(std::function<void()> apply) const
{
    ui_.post(
        [state = state_, gen = gen_, apply = std::move(apply)]
        {
            bool current;
            {
                std::lock_guard lock(state->mutex);
                current = !state->stop && gen == state->latest;
            }
            if (current) apply();
        });
}

RepoWorker::~RepoWorker()
{
    {
        std::lock_guard lock(state_->mutex);
        state_->stop = true;
    }
    state_->wake.notify_all();
    thread_.join();
}

void RepoWorker::submit(Job job)
{
    {
        std::lock_guard lock(state_->mutex);
        state_->pending = std::move(job);  // replaces any job not started yet
        ++state_->latest;
    }
    state_->wake.notify_one();
}

}  // namespace tortilla
