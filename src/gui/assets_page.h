#pragma once
// ASSETS: the game's assets as a folder tree with search and filters, and an inspector for the one
// picked (EBX properties, texture preview, Lua source, where it lives) with exports. Every action is a
// registry command (asset index / tree / search / info / types, ebx get, texture export,
// asset lua-source, asset export-raw) run on the job system.
#include "core/registry.h"

namespace studio::gui
{

    struct App;

    void assets_page(App &app);

    // The shared native index was rebuilt by another page; reload this page's cached tree and summary.
    void assets_index_refreshed();

    // Startup options for scripts and screenshots, given with --page assets:
    //   --arg search=<text>  --arg kind=all|ebx|res|chunk  --arg category=<group>  --arg type=<type name>
    //   --arg open=<folder>[;<folder>...]  --arg select=<asset name>  --arg select-kind=ebx|res|chunk
    //   --arg tab=properties|texture|lua|info  --arg edit=<field path>=<value> (a draft change; give it more than once)
    void assets_startup(const Json &args);

} // namespace studio::gui
