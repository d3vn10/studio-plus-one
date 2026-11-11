#pragma once
// Ported from ReSkate (GPL-3.0) Extension/UI/Overlay/skate_style.h, commit 3d259667d706.
#include "gui/skate_theme.h"
#include <imgui.h>
#include <iterator>

namespace studio::gui::style
{
    // The game's own palette (UI/skate_theme.h), shared with the launcher.
    inline constexpr ImU32 ink = IM_COL32(26, 26, 26, 255);
    inline constexpr ImU32 paper = skate_theme::white;
    inline constexpr ImU32 blue = skate_theme::blue;
    inline constexpr ImU32 muted = skate_theme::grey_text;

    // Deterministic screen-print details shared by the menu and console.
    inline void halftone(ImDrawList *draw, ImVec2 at, int columns, int rows, ImU32 color)
    {
        for (int y = 0; y < rows; ++y)
            for (int x = 0; x < columns; ++x)
                draw->AddCircleFilled(ImVec2(at.x + x * 7.0f + (y % 2) * 3.0f, at.y + y * 7.0f),
                                      0.7f + 0.16f * static_cast<float>((x + y) % 4),
                                      color,
                                      6);
    }
    // A white keycap followed by what it does, like the game's "Esc Back"
    // prompt. Returns the width drawn.
    inline float keycap(ImDrawList *draw, ImFont *font, float scale, ImVec2 at, const char *key, const char *label)
    {
        const auto cap = font->CalcTextSizeA(12 * scale, FLT_MAX, 0, key);
        draw->AddRectFilled(at, ImVec2(at.x + cap.x + 12 * scale, at.y + 20 * scale), paper, 3 * scale);
        draw->AddText(
            font, 12 * scale, ImVec2(at.x + 6 * scale, at.y + (20 * scale - cap.y) * .5f), skate_theme::black, key);
        const auto text = font->CalcTextSizeA(14 * scale, FLT_MAX, 0, label);
        draw->AddText(font, 14 * scale, ImVec2(at.x + cap.x + 20 * scale, at.y + 2 * scale), paper, label);
        return cap.x + 20 * scale + text.x;
    }
} // namespace studio::gui::style
