#pragma once
// The generic command runner: a form built from a registry command's params, a Run button, progress
// and Cancel while it runs, and the result as a JSON tree. Also the Activity drawer.
#include "core/registry.h"
#include "gui/jobs.h"

#include <windows.h>

#include <imgui.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace studio::gui
{

    struct FieldState
    {
        std::string text;               // String, Path, Integer, Number
        bool flag = false;              // Boolean
        int choice = -1;                // Enum: index into choices, -1 for none
        std::vector<std::string> items; // List
        std::string next_item;          // List: the value being typed
    };

    struct Runner
    {
        const Command *command = nullptr;
        std::vector<FieldState> fields;
        std::shared_ptr<Job> job; // the run shown under the form
        std::string problem;      // why Run was refused (arguments that do not validate)
        int reveal = 0;           // scroll to the run: 2 when it starts, 1 until it finishes, then a few frames below 0

        // Shows `c` with a fresh form (defaults filled in). Keeps the form if it is already showing `c`.
        void bind(const Command *c);
        // Shows a past run, with its arguments back in the form.
        void show(const std::shared_ptr<Job> &past);
        // Puts an arguments object (as Json, CLI-style strings are fine) into the form.
        void fill(const Json &args);
        // What RUN does: checks the arguments, then starts the command. False with `problem` set if they do not check.
        bool run();
        // The arguments object for execute(), holding only what was filled in.
        Json args() const;
    };

    // The form, Run/Cancel, progress and result for runner.command.
    void draw_runner(Runner &runner, HWND owner);
    // Progress and Cancel while `job` runs, then its outcome, JSON tree and copy buttons.
    void draw_job(Job &job, bool show_cli = true);
    // A JSON value as a collapsible tree; right-click a value to copy it.
    void json_tree(const Json &value);

    // The Activity drawer along the bottom of the content area. `open_job` shows a run in the runner.
    // Returns the height it took.
    float draw_activity(ImVec2 position,
                        float width,
                        float max_height,
                        bool &open,
                        float &open_height,
                        const std::function<void(const std::shared_ptr<Job> &)> &open_job);

    // Whether a Path param should browse for a folder rather than a file, from its name.
    bool param_wants_folder(const Param &param);

} // namespace studio::gui
