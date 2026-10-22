// A background thread with its own Repo that runs only the newest job: for
// work triggered by the UI (a row click's file list) where a newer request
// makes older ones pointless. Results reach the UI thread only if no newer
// job was submitted and the worker still exists, so clicks never wait on
// git and stale results never land.
#pragma once

#include "git/repo.h"

#include <rapport/rapport.h>

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace tortilla
{

class RepoWorker
{
public:
    struct State;

    // What a job can check and do while it runs: whether a newer job has
    // made it pointless (stop early), and hand results to the UI thread
    // before it finishes (they're dropped if it has gone stale by then).
    class Progress
    {
    public:
        bool stale() const;
        void post(std::function<void()>) const;

    private:
        friend class RepoWorker;
        Progress(rap::Ui& ui, std::shared_ptr<State> state, unsigned gen)
            : ui_(ui)
            , state_(std::move(state))
            , gen_(gen)
        {
        }
        rap::Ui& ui_;
        std::shared_ptr<State> state_;
        unsigned gen_;
    };

    // Runs on the worker thread; returns what to do on the UI thread.
    using Job = std::function<std::function<void()>(Repo&, const Progress&)>;

    RepoWorker(rap::Ui&, std::string path);
    ~RepoWorker();

    void submit(Job);

    struct State
    {
        std::mutex mutex;
        std::condition_variable wake;
        Job pending;
        unsigned latest = 0;  // generation of the newest submitted job
        bool stop = false;
    };

private:
    rap::Ui& ui_;
    std::shared_ptr<State> state_ = std::make_shared<State>();
    std::thread thread_;
};

}  // namespace tortilla
