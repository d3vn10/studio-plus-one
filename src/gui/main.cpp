// ReSkate Studio+.exe: the window. It runs the same registry commands as studio-plus and the MCP server.
#include "core/registry.h"
#include "gui/app.h"

#include <windows.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    studio::register_all_commands();
    return studio::gui::run_app();
}
