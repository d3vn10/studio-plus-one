#pragma once
// Commands run from the GUI: each one on its own worker thread, so the window never waits.
// Every run is kept for the Activity panel with the CLI line that repeats it.
#include "core/registry.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace studio::gui
{

    enum class JobState
    {
        running,
        ok,
        failed,
        cancelled
    };

    struct Job
    {
        int id = 0;
        const Command *command = nullptr;
        Json args;       // as entered in the GUI, before defaults are filled in
        std::string cli; // the equivalent studio-plus command line
        std::chrono::system_clock::time_point started_at;
        std::chrono::steady_clock::time_point started, finished;
        std::atomic<bool> cancel{false};
        std::atomic<bool> done{false};

        // Written by the worker, read by the window; hold `mutex` for these.
        mutable std::mutex mutex;
        double fraction = -1; // < 0: unknown
        std::string message;
        std::vector<std::pair<std::string, std::string>> log; // level, message
        Json outcome; // execute()'s {"ok", "result"} or {"ok", "error"} once done

        JobState state() const;
        double seconds() const;
    };

    class Jobs
    {
      public:
        std::shared_ptr<Job> start(const Command &command, Json args);
        const std::vector<std::shared_ptr<Job>> &all() const
        {
            return jobs_;
        }
        int running() const;
        void cancel_all();
        // Waits up to `milliseconds` for every job to finish; false if some are still going.
        bool wait_all(int milliseconds) const;

      private:
        std::vector<std::shared_ptr<Job>> jobs_;
        int next_id_ = 1;
    };
    inline Jobs g_jobs;

    // One argument quoted the way CommandLineToArgvW reads it back.
    std::string quote_arg(std::string_view text);
    // "studio-plus <group> <name> ..." for these arguments: positionals in order where the CLI accepts
    // them, --name value for the rest, and defaults left out.
    std::string cli_line(const Command &command, const Json &args);
    // "0.4 s", "12 s", "3 m 07 s".
    std::string format_seconds(double seconds);

} // namespace studio::gui
