#pragma once
// Form widgets shared by the pages and the command runner: text input on std::string, path fields
// with Browse and drag-and-drop, and the folder/file pickers behind them.
#include <windows.h>

#include <imgui.h>

#include <mutex>
#include <string>
#include <vector>

namespace studio::gui
{

    // Files dropped on the window (WM_DROPFILES), waiting for a path field under the drop point to take them.
    struct Drop
    {
        std::mutex mutex;
        std::vector<std::wstring> paths;
        ImVec2 point; // client coordinates, which are ImGui's
        bool pending = false;
    };
    inline Drop g_drop;
    // The path field a drop goes to when it lands on no field: the first one drawn this frame.
    inline std::string *g_drop_fallback = nullptr;
    // Set by a path field that took a drop this frame, so the page can react (Settings saves it).
    inline const std::string *g_dropped_into = nullptr;

    struct FileFilter
    {
        const wchar_t *name;
        const wchar_t *pattern;
    };

    // IFileOpenDialog for a folder or a file. Empty when cancelled.
    std::wstring pick_path(HWND owner,
                           bool folder,
                           const std::wstring &start,
                           const std::vector<FileFilter> &filters = {});

    bool input_text(const char *id, std::string &value, ImGuiInputTextFlags flags = 0, const char *hint = nullptr);
    // A text field plus BROWSE. Right-click BROWSE to pick a folder instead of a file, or the other way.
    // Takes a file dropped on it. Returns true when the value changed.
    bool path_field(const char *id,
                    std::string &value,
                    HWND owner,
                    bool folder,
                    const std::vector<FileFilter> &filters = {},
                    float browse_width = 0);
    // Takes a pending drop if it landed on the last item drawn.
    bool take_drop(std::string &value);

    // Copies text and shows a short "Copied" tooltip on the item that did it.
    void copy_text(const std::string &text);
    // A small grey button that copies `text` when pressed.
    bool copy_button(const char *id, const std::string &text);

} // namespace studio::gui
