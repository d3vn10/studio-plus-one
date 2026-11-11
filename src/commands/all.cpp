#include "commands/commands.h"

namespace studio
{
    void register_all_commands()
    {
        static bool done = false;
        if (done)
            return;
        done = true;
        auto &r = Registry::instance();
        register_studio_commands(r);
        register_map_commands(r);
        register_mod_commands(r);
        register_asset_commands(r);
        register_cosmetic_commands(r);
        register_media_commands(r);
        register_browse_commands(r);
    }
} // namespace studio
