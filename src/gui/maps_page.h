#pragma once
// The CUSTOM MAPS page: a Blender scene or a normalized map folder in, a Skate level out, built with
// map compile and installed with mod deploy.
#include "core/registry.h"

namespace studio::gui
{

    struct App;

    void maps_page(App &app);

    // Startup options for scripts and screenshots, given with --page maps:
    //   --arg map=<scene or folder>  --arg staging-root=<folder>  --arg mod-folder=<name>  --arg game-root=<folder>
    //   --arg preset=quick|full  --arg view=options|build|install|lists  --arg install=confirm  and any map compile
    //   option
    //   --run builds straight away; install=confirm asks to install once a build is there.
    void maps_startup(const Json &args, bool run);

} // namespace studio::gui
