#pragma once
// PROJECT & MODS: the mods installed in the ReSkate folder, a .fbproject and .fbmod files, building
// .fbmods into a staging folder and installing it, and a few health checks on the game.
#include "gui/app.h"

namespace studio::gui
{

    void project_page(App &app);

    // Puts `file` (.fbmod or .fbproject) in the BUILD & INSTALL list and opens that section, for pages that
    // just made a mod.
    void build_in_project(App &app, const std::string &file);

} // namespace studio::gui
