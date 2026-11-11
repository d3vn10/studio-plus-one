#pragma once
// The ReSkate launcher's look, ported from ReSkate (GPL-3.0) Launcher/gui_internal.h, gui.cpp and
// gui_home.cpp at commit 3d259667d706: colours, fonts, scale, style and the hand-drawn widgets.
#include "gui/skate_theme.h"

#include <windows.h>

#include <imgui.h>

#include <algorithm>
#include <string>

namespace studio::gui
{

    // The default window size at scale 1. The launcher's design size is 1440x840; Studio+ is resizable,
    // so this is only where it starts.
    constexpr float design_width = 1360.0f;
    constexpr float design_height = 840.0f;
    constexpr float min_width = 1000.0f;
    constexpr float min_height = 640.0f;
    // The custom title bar: the top strip drags the window, apart from the buttons on its right.
    constexpr float title_bar_height = 44.0f;
    constexpr float window_button_width = 46.0f;
    constexpr float window_button_height = 34.0f;
    constexpr int window_button_count = 3;

    inline ImU32 rgba(int r, int g, int b, float a = 1.0f)
    {
        return IM_COL32(r, g, b, static_cast<int>(std::clamp(a, 0.0f, 1.0f) * 255.0f));
    }
    namespace color
    {
        inline const ImU32 background_top = rgba(9, 11, 15);
        inline const ImU32 background_bottom = rgba(17, 20, 27);
        inline const ImU32 page = rgba(14, 15, 18);
        inline const ImU32 text = rgba(236, 239, 244);
        inline const ImU32 muted = rgba(128, 137, 151);
        inline const ImU32 danger = rgba(255, 92, 92);
        inline const ImU32 panel = rgba(26, 26, 26, 0.97f);
        // Sampled from skate.'s own menus (HUB screen).
        inline const ImU32 tile = skate_theme::tile;
        inline const ImU32 tile_grey = skate_theme::tile_light;
        inline const ImU32 blue = skate_theme::blue;
        inline const ImU32 good = skate_theme::good;
        inline const ImU32 warning = skate_theme::warning;
        inline const ImU32 avatar = skate_theme::avatar;
        inline const ImU32 ink = skate_theme::black;
        inline const ImU32 outline = rgba(255, 255, 255, 0.12f);
    } // namespace color

    using skate_theme::pop_primary_button;
    using skate_theme::push_primary_button;

    struct Fonts
    {
        ImFont *body{}; // Montserrat SemiBold
        ImFont *caption{};
        ImFont *bold{}; // Montserrat ExtraBold
        ImFont *heading{};
        ImFont *tile{};   // tile headers, like the HUB's "BOUNTIES"
        ImFont *action{}; // the PLAY tile
        ImFont *title{};  // brushed page title, like the HUB's "HUB"
    };
    inline Fonts g_fonts;

    inline float g_scale = 1.0f;
    inline float S(float value)
    {
        return value * g_scale;
    }
    inline ImVec2 S(float x, float y)
    {
        return ImVec2(x * g_scale, y * g_scale);
    }

    // Builds the font atlas at g_scale (embedded RCDATA fonts, Segoe UI if one is missing).
    bool load_fonts();
    // Square, flat, high-contrast: the launcher's apply_style(), at g_scale.
    void apply_style();

    // Gradient, perspective floor and drifting chalk marks behind every page.
    void draw_background(ImDrawList *draw, ImVec2 size, float time);
    // Brushed title, tilted like the HUB's; `size` zero means the title font's own.
    void page_title(ImDrawList *draw, ImVec2 position, const char *text, float size = 0);
    // Minimise, maximise/restore and close in the top-right corner.
    void window_buttons(ImDrawList *draw, HWND window, ImVec2 size);

    void rough_rect(ImDrawList *draw, ImVec2 a, ImVec2 b, ImU32 colour, unsigned seed);
    unsigned seed_of(const char *text);
    void progress_bar(ImDrawList *draw, ImVec2 a, ImVec2 b, float fill, float time);
    void draw_status_icon(ImDrawList *draw, ImVec2 centre, skate_theme::Icon icon, float time);

    void panel_title(const char *text);
    // An invisible button at an absolute screen position, for a tile drawn by hand.
    bool tile_hit(const char *id, ImVec2 position, ImVec2 size, bool enabled, bool &hovered);
    // A tile in the nav rail, blue while its page is open. Leaves the cursor where the next tile goes.
    bool nav_tile(float width,
                  const char *label,
                  bool selected,
                  const std::string &count = {},
                  bool accent = false,
                  const std::string &tip = {});
    float badge_width(const std::string &text);
    void badge(
        ImDrawList *draw, ImVec2 position, const std::string &text, ImU32 fill = color::blue, ImU32 ink = color::ink);
    // A caption over its value.
    void field(const char *name, const std::string &value);
    // One row of a list: background, hover tint, a rule under it and a blue edge when ticked.
    bool list_row(const char *id, float width, float height, bool ticked);
    // An on/off switch, blue while on. Returns true when it was flipped.
    bool toggle(const char *id, bool *on);
    // A tile drawn with rough edges behind a block of ImGui content: call begin, draw, then end.
    void begin_tile(const char *id, unsigned seed);
    void end_tile();
    // Small grey caption text on the same line as the bold text before it, sharing its baseline.
    void inline_caption(const char *text);
    // A button the width of its label in the primary (blue, black text) colours.
    bool primary_button(const char *label, ImVec2 size = ImVec2(0, 0));

} // namespace studio::gui
