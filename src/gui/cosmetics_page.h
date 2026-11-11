#pragma once
// The COSMETICS page: browse the installed cosmetics (cosmetic list, cosmetic audit), make a new cosmetic
// from a mesh (cosmetic import-mesh), replace a game mesh such as the board deck (mesh find / info /
// replace) and build native costumes (costume build / info / donors).
#include "core/registry.h"

namespace studio::gui
{

    struct App;

    void cosmetics_page(App &app);

    // Startup options for scripts and screenshots, given with --page cosmetics:
    //   --arg view=browse|new|part|replace|costume  --arg text=<filter>  --arg slot=<slot>
    //   --arg mesh=<game mesh>  --arg model=<.glb|.fbx>  --arg output=<.fbproject|.fbmod>  --arg hide=deck-parts
    //   --arg rigid=true  --arg donor=<item>  --arg select=<item>  --arg show=model|result (scrolls there)
    //   --run loads the catalog (audit) on browse, reads the mesh on replace and, with a model, replaces it.
    void cosmetics_startup(const Json &args, bool run);

    // Opens REPLACE A GAME MESH on `mesh` and reads it (from the Assets page).
    void cosmetics_replace_mesh(App &app, const std::string &mesh);

} // namespace studio::gui
