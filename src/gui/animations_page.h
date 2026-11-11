#pragma once
// The ANIMATIONS page: find a clip by name (asset find), read it (anim info), export it to FBX (anim export),
// check an FBX (anim fbx-info) and put a take back in place of the clip (anim import).
#include "core/registry.h"

namespace studio::gui
{

    struct App;

    void animations_page(App &app);

    // Startup options for scripts and screenshots, given with --page animations:
    //   --arg find=<text>  --arg clip=<clip>  --arg fbx=<file>  --arg output=<.fbproject|.fbmod>
    //   --arg show=find|info|export|import|ride scrolls to that step
    //   --run searches for `find`, reads `clip` and, with fbx, lists its takes.
    void animations_startup(const Json &args, bool run);

    // Opens the page on `clip` and reads it (from the Assets page).
    void animations_open_clip(App &app, const std::string &clip);

} // namespace studio::gui
