#pragma once
// Small pieces the COSMETICS and ANIMATIONS pages share: headings in the launcher's fonts, starting a
// registry command on the job system, and one status block per action (progress while it runs, then
// the result or the error with its hint, plus the CLI line that repeats it).
#include "core/registry.h"
#include "gui/jobs.h"

#include <imgui.h>

#include <memory>
#include <string>

namespace studio::gui::kit
{

    void heading(const char *text);                // page heading (tile font)
    void tile_title(const char *text);             // tile heading
    void step_title(int number, const char *text); // "1  FIND A CLIP"
    void caption(const char *text);
    void muted(const std::string &text);                  // wrapped, grey
    void coloured(const std::string &text, ImU32 colour); // wrapped

    bool running(const std::shared_ptr<Job> &job);
    bool succeeded(const std::shared_ptr<Job> &job);
    // The job's {"ok", "result"|"error"} once done, else an empty object.
    Json outcome(const std::shared_ptr<Job> &job);
    // result of a successful job, else null.
    Json result(const std::shared_ptr<Job> &job);

    // Starts `group name` with `args` (empty strings and nulls left out). Null when the command is missing.
    std::shared_ptr<Job> run(const char *group, const char *name, Json args);

    // Progress and CANCEL while the job runs; then "<done_text> in 3 s" or the error and a hint. Nothing for
    // a null job. Ends with COPY COMMAND LINE / COPY RESULT JSON.
    void status(const std::shared_ptr<Job> &job, const char *done_text);

    // Scrolls the page so the next item drawn is at the top, for a few frames, when `target` is `name`
    // (startup option --arg show=<name>, for scripts and screenshots).
    void scroll_here(std::string &target, int &frames, const char *name);

    // <data dir>\<folder>\<file>, creating the folder.
    std::string data_path(const wchar_t *folder, const std::string &file);
    // The last part of an asset path, e.g. c_proto_onb_push for animation/dingo/c_proto_onb_push.
    std::string leaf(const std::string &asset);
    std::string lower(std::string text);
    // 12,345
    std::string thousands(long long value);

    // object[key] as a number, or `fallback` when it is missing, null or not a number. Engine-backed
    // commands report null for values the engine left out, and Json::value would throw on those.
    template <typename T> T number(const Json &object, const char *key, T fallback)
    {
        if (!object.is_object())
            return fallback;
        const auto it = object.find(key);
        return it != object.end() && it->is_number() ? it->template get<T>() : fallback;
    }

} // namespace studio::gui::kit
