#pragma once
// The Studio+ window: its pages, what they remember between frames, and the window itself.
#include "core/settings.h"
#include "gui/jobs.h"
#include "gui/runner.h"

#include <windows.h>

#include <array>
#include <memory>
#include <string>

namespace studio::gui
{

    class Renderer;

    enum class page
    {
        home,
        maps,
        cosmetics,
        animations,
        project,
        assets,
        settings,
        commands
    };

    struct SettingRow
    {
        std::string key; // the `studio set` key
        std::string label;
        std::string help;
        bool folder = true;
        std::string edit;         // what is in the field
        std::shared_ptr<Job> job; // the last save
        bool handled = true;      // the save's outcome has been taken in
    };

    struct App
    {
        HWND window{};
        Renderer *renderer{};
        page page = page::home;

        Runner runner; // All commands and anything opened from Activity
        std::string command_filter;

        std::shared_ptr<Job> doctor; // the last `studio doctor`
        bool doctor_handled = true;

        Settings settings;
        std::array<SettingRow, 3> rows;
        std::string focus_key; // a setting a FIX button pointed at, highlighted for a moment
        double focus_until = 0;

        bool activity_open = false;
        float activity_height = 0;
        bool close_requested = false; // the window was closed while commands were running
        bool close_confirmed = false;
    };

    void init_app(App &app);
    void draw_frame(App &app);
    void run_doctor(App &app);

    // Runs the window until it is closed. Returns the process exit code.
    int run_app();

} // namespace studio::gui
