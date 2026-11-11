// Ported from ReSkate (GPL-3.0) Launcher/gui.cpp and Launcher/gui_home.cpp, commit 3d259667d706.
#include "gui/look.h"

#include <array>
#include <cmath>
#include <filesystem>

namespace studio::gui
{
    namespace fs = std::filesystem;

    namespace
    {
        // Fonts are embedded as RCDATA (third_party/fonts); Segoe UI is the fallback.
        ImFont *embedded_font(const wchar_t *name, float size)
        {
            static const ImWchar ranges[]{
                0x0020, 0x024F, 0x0400, 0x052F, 0x2000, 0x206F, 0x2190, 0x2193, 0x2713, 0x2717, 0};
            const auto instance = GetModuleHandleW(nullptr);
            const auto resource = FindResourceW(instance, name, MAKEINTRESOURCEW(10) /* RT_RCDATA */);
            if (!resource)
                return nullptr;
            const auto loaded = LoadResource(instance, resource);
            void *data = loaded ? LockResource(loaded) : nullptr;
            if (!data)
                return nullptr;
            ImFontConfig config;
            config.FontDataOwnedByAtlas = false; // the resource lives as long as the process
            config.OversampleH = size >= 48 ? 1 : 2;
            return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
                data, static_cast<int>(SizeofResource(instance, resource)), S(size), &config, ranges);
        }

        ImFont *system_font(const wchar_t *file, float size)
        {
            std::array<wchar_t, MAX_PATH> windows{};
            GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size()));
            const auto path = fs::path(windows.data()) / L"Fonts" / file;
            std::error_code error;
            if (!fs::is_regular_file(path, error))
                return nullptr;
            return ImGui::GetIO().Fonts->AddFontFromFileTTF(path.string().c_str(), S(size));
        }
    } // namespace

    bool load_fonts()
    {
        auto &io = ImGui::GetIO();
        io.Fonts->Clear();
        io.Fonts->TexGlyphPadding = 3; // large glyphs bleed into neighbours with 1 px
        const auto font = [](const wchar_t *name, const wchar_t *fallback, float size)
        {
            if (auto *loaded = embedded_font(name, size))
                return loaded;
            if (auto *loaded = system_font(fallback, size))
                return loaded;
            return ImGui::GetIO().Fonts->AddFontDefault();
        };
        g_fonts.body = font(L"FONT_BODY", L"segoeui.ttf", 15);
        g_fonts.caption = font(L"FONT_BODY", L"segoeui.ttf", 12);
        g_fonts.bold = font(L"FONT_HEADING", L"segoeuib.ttf", 16);
        g_fonts.heading = font(L"FONT_HEADING", L"segoeuib.ttf", 22);
        g_fonts.tile = font(L"FONT_HEADING", L"seguibl.ttf", 30);
        g_fonts.action = font(L"FONT_HEADING", L"seguibl.ttf", 50);
        g_fonts.title = font(L"FONT_BRUSH", L"seguibl.ttf", 86);
        io.FontDefault = g_fonts.body;
        // Built here rather than on the first frame: a window with no atlas draws nothing but its clear colour.
        return io.Fonts->Build();
    }

    void apply_style()
    {
        auto &style = ImGui::GetStyle();
        style = ImGuiStyle();
        // skate.'s menus are square-cornered, flat and high-contrast.
        style.WindowRounding = 0;
        style.ChildRounding = 0;
        style.FrameRounding = 0;
        style.GrabRounding = 0;
        style.PopupRounding = 0;
        style.WindowBorderSize = 0;
        style.ChildBorderSize = 0;
        style.PopupBorderSize = S(1);
        style.FrameBorderSize = 0;
        style.WindowPadding = ImVec2(S(28), S(24));
        style.FramePadding = ImVec2(S(12), S(8));
        style.ItemSpacing = ImVec2(S(10), S(10));
        style.ItemInnerSpacing = ImVec2(S(8), S(6));
        style.IndentSpacing = S(18);
        style.ScrollbarSize = S(10);
        style.GrabMinSize = S(10);
        style.CellPadding = ImVec2(S(8), S(5));
        auto *colours = style.Colors;
        const auto rgb = [](ImU32 colour) { return ImGui::ColorConvertU32ToFloat4(colour); };
        colours[ImGuiCol_Text] = rgb(color::text);
        colours[ImGuiCol_TextDisabled] = rgb(color::muted);
        colours[ImGuiCol_WindowBg] = rgb(color::panel);
        colours[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
        colours[ImGuiCol_Border] = rgb(color::outline);
        colours[ImGuiCol_FrameBg] = rgb(rgba(45, 45, 47));
        colours[ImGuiCol_FrameBgHovered] = rgb(rgba(61, 62, 66));
        colours[ImGuiCol_FrameBgActive] = rgb(rgba(72, 73, 78));
        colours[ImGuiCol_Button] = rgb(rgba(45, 45, 47));
        colours[ImGuiCol_ButtonHovered] = rgb(rgba(61, 62, 66));
        colours[ImGuiCol_ButtonActive] = rgb(rgba(72, 73, 78));
        colours[ImGuiCol_CheckMark] = rgb(color::blue);
        colours[ImGuiCol_SliderGrab] = rgb(color::blue);
        colours[ImGuiCol_Header] = rgb(rgba(45, 45, 47));
        colours[ImGuiCol_HeaderHovered] = rgb(rgba(1, 131, 255, 0.35f));
        colours[ImGuiCol_HeaderActive] = rgb(rgba(1, 131, 255, 0.5f));
        colours[ImGuiCol_PopupBg] = rgb(rgba(26, 26, 26));
        colours[ImGuiCol_ModalWindowDimBg] = rgb(rgba(4, 6, 9, 0.72f));
        colours[ImGuiCol_Separator] = rgb(color::outline);
        colours[ImGuiCol_TextSelectedBg] = rgb(rgba(1, 131, 255, 0.45f));
        colours[ImGuiCol_NavHighlight] = ImVec4(0, 0, 0, 0);
        // Not set by the launcher, which has no tables or scrolling trees: taken from skate_theme.
        colours[ImGuiCol_TableHeaderBg] = rgb(skate_theme::tile_grey);
        colours[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
        colours[ImGuiCol_TableRowBgAlt] = rgb(rgba(255, 255, 255, 0.035f));
        colours[ImGuiCol_TableBorderLight] = rgb(rgba(52, 53, 56));
        colours[ImGuiCol_TableBorderStrong] = rgb(rgba(61, 62, 66));
        colours[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
        colours[ImGuiCol_ScrollbarGrab] = rgb(skate_theme::tile_light);
        colours[ImGuiCol_ScrollbarGrabHovered] = rgb(rgba(104, 108, 112));
        colours[ImGuiCol_ScrollbarGrabActive] = rgb(color::blue);
        colours[ImGuiCol_PlotHistogram] = rgb(color::blue);
    }

    void draw_background(ImDrawList *draw, ImVec2 size, float time)
    {
        // Vertical gradient plus a warm glow bottom-right and a cool glow top-left,
        // evaluated on a grid of bilinear quads. The quads are opaque: stacked
        // circles band, and translucent layers under the modal dim left holes.
        struct Glow
        {
            ImVec2 centre;
            float radius;
            ImVec4 colour;
        };
        const Glow glows[]{
            {ImVec2(size.x * 0.95f, size.y * 1.05f), S(700), ImVec4(1.0f, 0.5f, 0.08f, 0.42f)},
            {ImVec2(size.x * 0.02f, size.y * -0.08f), S(560), ImVec4(0.23f, 0.38f, 0.9f, 0.22f)},
        };
        const auto top = ImGui::ColorConvertU32ToFloat4(color::background_top);
        const auto bottom = ImGui::ColorConvertU32ToFloat4(color::background_bottom);
        const auto field_at = [&](float x, float y)
        {
            const float v = std::clamp(y / size.y, 0.0f, 1.0f);
            ImVec4 out(
                top.x + (bottom.x - top.x) * v, top.y + (bottom.y - top.y) * v, top.z + (bottom.z - top.z) * v, 1);
            for (const auto &glow : glows)
            {
                const float t = std::min(std::hypot(x - glow.centre.x, y - glow.centre.y) / glow.radius, 1.0f);
                const float a = glow.colour.w * (1 - t) * (1 - t);
                out.x += (glow.colour.x - out.x) * a;
                out.y += (glow.colour.y - out.y) * a;
                out.z += (glow.colour.z - out.z) * a;
            }
            return ImGui::ColorConvertFloat4ToU32(out);
        };
        constexpr int columns = 32, rows = 18;
        const float cell_x = size.x / columns, cell_y = size.y / rows;
        for (int row = 0; row < rows; ++row)
            for (int column = 0; column < columns; ++column)
            {
                const float x0 = cell_x * static_cast<float>(column), y0 = cell_y * static_cast<float>(row);
                const float x1 = x0 + cell_x, y1 = y0 + cell_y;
                draw->AddRectFilledMultiColor(ImVec2(x0, y0),
                                              ImVec2(x1, y1),
                                              field_at(x0, y0),
                                              field_at(x1, y0),
                                              field_at(x1, y1),
                                              field_at(x0, y1));
            }

        // Skatepark floor: a perspective grid.
        const ImVec2 vanish(size.x * 0.5f, size.y * 0.46f);
        const float floor = size.y * 0.62f;
        for (int line = -14; line <= 14; ++line)
        {
            const float x = size.x * 0.5f + static_cast<float>(line) * S(120);
            draw->AddLine(
                ImVec2(vanish.x + (x - vanish.x) * 0.18f, floor), ImVec2(x, size.y), rgba(255, 255, 255, 0.035f), 1);
        }
        const float scroll = std::fmod(time * 0.06f, 1.0f);
        for (int row = 0; row < 9; ++row)
        {
            const float t = (static_cast<float>(row) + scroll) / 9.0f;
            const float y = floor + (size.y - floor) * t * t;
            draw->AddLine(ImVec2(0, y), ImVec2(size.x, y), rgba(255, 255, 255, 0.03f * t + 0.01f), 1);
        }

        // Drifting outline shapes, like chalk marks on a ramp.
        for (int index = 0; index < 28; ++index)
        {
            const float seed = static_cast<float>(index) * 12.9898f;
            const float fx = std::fmod(std::abs(std::sin(seed) * 43758.5453f), 1.0f);
            const float fy = std::fmod(std::abs(std::sin(seed * 1.7f) * 24634.6345f), 1.0f);
            const float drift = time * (0.004f + 0.006f * fx);
            const ImVec2 centre(size.x * std::fmod(fx + drift, 1.0f),
                                size.y * fy + std::sin(time * 0.3f + seed) * S(6));
            const float radius = S(3.0f + 16.0f * fy * fx);
            const float alpha = 0.05f + 0.07f * fy;
            if (index % 3 == 0)
            {
                const float angle = time * 0.2f + seed;
                ImVec2 corners[4];
                for (int corner = 0; corner < 4; ++corner)
                {
                    const float a = angle + static_cast<float>(corner) * 1.5707963f;
                    corners[corner] = ImVec2(centre.x + std::cos(a) * radius, centre.y + std::sin(a) * radius);
                }
                draw->AddQuad(corners[0], corners[1], corners[2], corners[3], rgba(255, 255, 255, alpha), 1.2f);
            }
            else
            {
                draw->AddCircle(centre, radius, rgba(255, 255, 255, alpha), 0, 1.2f);
            }
        }
        // Vignette edges.
        draw->AddRectFilledMultiColor(
            ImVec2(0, size.y * 0.75f), size, 0, 0, rgba(0, 0, 0, 0.45f), rgba(0, 0, 0, 0.45f));
    }

    void page_title(ImDrawList *draw, ImVec2 position, const char *text, float wanted)
    {
        const int start = draw->VtxBuffer.Size;
        const float size = wanted > 0 ? wanted : g_fonts.title->FontSize;
        const auto extent = g_fonts.title->CalcTextSizeA(size, FLT_MAX, 0, text);
        draw->AddText(g_fonts.title, size, ImVec2(position.x + S(3), position.y + S(4)), rgba(0, 0, 0, 0.5f), text);
        draw->AddText(g_fonts.title, size, position, color::text, text);
        skate_theme::rotate_since(
            draw, start, -4.0f, ImVec2(position.x + extent.x * 0.5f, position.y + extent.y * 0.5f));
    }

    void window_buttons(ImDrawList *draw, HWND window, ImVec2 size)
    {
        const ImVec2 button(S(window_button_width), S(window_button_height));
        bool hovered{};
        ImVec2 position(size.x - button.x * window_button_count, 0);
        if (tile_hit("##minimise", position, button, true, hovered))
            ShowWindow(window, SW_MINIMIZE);
        if (hovered)
            draw->AddRectFilled(position, ImVec2(position.x + button.x, button.y), rgba(255, 255, 255, 0.08f));
        ImVec2 middle(position.x + button.x * 0.5f, button.y * 0.5f);
        draw->AddLine(ImVec2(middle.x - S(5), middle.y), ImVec2(middle.x + S(5), middle.y), color::text, S(1));

        // Studio+ is resizable, so it has the maximise button the launcher leaves out.
        position.x += button.x;
        const bool zoomed = IsZoomed(window) != FALSE;
        if (tile_hit("##maximise", position, button, true, hovered))
            ShowWindow(window, zoomed ? SW_RESTORE : SW_MAXIMIZE);
        if (hovered)
            draw->AddRectFilled(position, ImVec2(position.x + button.x, button.y), rgba(255, 255, 255, 0.08f));
        middle = ImVec2(position.x + button.x * 0.5f, button.y * 0.5f);
        if (zoomed)
        {
            draw->AddRect(ImVec2(middle.x - S(5), middle.y - S(3)),
                          ImVec2(middle.x + S(3), middle.y + S(5)),
                          color::text,
                          0,
                          0,
                          S(1));
            draw->AddLine(
                ImVec2(middle.x - S(3), middle.y - S(5)), ImVec2(middle.x + S(5), middle.y - S(5)), color::text, S(1));
            draw->AddLine(
                ImVec2(middle.x + S(5), middle.y - S(5)), ImVec2(middle.x + S(5), middle.y + S(3)), color::text, S(1));
        }
        else
        {
            draw->AddRect(ImVec2(middle.x - S(5), middle.y - S(5)),
                          ImVec2(middle.x + S(5), middle.y + S(5)),
                          color::text,
                          0,
                          0,
                          S(1));
        }

        position.x += button.x;
        if (tile_hit("##close", position, button, true, hovered))
            PostMessageW(window, WM_CLOSE, 0, 0);
        if (hovered)
            draw->AddRectFilled(position, ImVec2(position.x + button.x, button.y), rgba(232, 17, 35));
        const ImVec2 cross(position.x + button.x * 0.5f, button.y * 0.5f);
        draw->AddLine(
            ImVec2(cross.x - S(5), cross.y - S(5)), ImVec2(cross.x + S(5), cross.y + S(5)), color::text, S(1));
        draw->AddLine(
            ImVec2(cross.x - S(5), cross.y + S(5)), ImVec2(cross.x + S(5), cross.y - S(5)), color::text, S(1));
    }

    void rough_rect(ImDrawList *draw, ImVec2 a, ImVec2 b, ImU32 colour, unsigned seed)
    {
        skate_theme::rough_rect(draw, a, b, colour, seed, g_scale);
    }

    unsigned seed_of(const char *text)
    {
        unsigned seed = 7;
        for (const char *letter = text; *letter; ++letter)
            seed = seed * 31u + static_cast<unsigned char>(*letter);
        return seed;
    }

    void progress_bar(ImDrawList *draw, ImVec2 a, ImVec2 b, float fill, float time)
    {
        skate_theme::striped_bar(draw, a, b, fill, time, g_scale);
    }

    void draw_status_icon(ImDrawList *draw, ImVec2 centre, skate_theme::Icon icon, float time)
    {
        skate_theme::status_icon(draw, centre, icon, time, g_scale);
    }

    void panel_title(const char *text)
    {
        ImGui::PushFont(g_fonts.tile);
        ImGui::TextUnformatted(text);
        ImGui::PopFont();
    }

    bool tile_hit(const char *id, ImVec2 position, ImVec2 size, bool enabled, bool &hovered)
    {
        ImGui::SetCursorScreenPos(position);
        ImGui::BeginDisabled(!enabled);
        const bool pressed = ImGui::InvisibleButton(id, size);
        hovered = enabled && ImGui::IsItemHovered();
        ImGui::EndDisabled();
        if (hovered)
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        return pressed;
    }

    bool nav_tile(
        float width, const char *label, bool selected, const std::string &count, bool accent, const std::string &tip)
    {
        auto *draw = ImGui::GetWindowDrawList();
        const ImVec2 position = ImGui::GetCursorScreenPos();
        const ImVec2 size(width, S(46));
        bool hovered{};
        const bool pressed = tile_hit(label, position, size, true, hovered);
        const ImVec2 end(position.x + size.x, position.y + size.y);
        // Each tile gets its own hand-cut edge, so no two wobble the same way.
        rough_rect(draw,
                   position,
                   end,
                   selected  ? color::blue
                   : hovered ? color::tile_grey
                             : color::tile,
                   seed_of(label));
        // The launcher's rail has two tiles in the heading font; eight fit better in the bold one.
        ImFont *font = g_fonts.bold;
        draw->AddText(font,
                      font->FontSize,
                      ImVec2(position.x + S(16), position.y + (size.y - font->FontSize) * 0.5f),
                      selected ? color::ink : color::text,
                      label);
        if (!count.empty())
        {
            const float height = g_fonts.caption->FontSize + S(8);
            badge(draw,
                  ImVec2(end.x - S(12) - badge_width(count), position.y + (size.y - height) * 0.5f),
                  count,
                  selected ? rgba(0, 0, 0, 0.32f)
                  : accent ? color::blue
                           : rgba(255, 255, 255, 0.14f),
                  !selected && accent ? color::ink : color::text);
        }
        if (hovered && !tip.empty())
            ImGui::SetTooltip("%s", tip.c_str());
        ImGui::SetCursorScreenPos(ImVec2(position.x, end.y + S(6)));
        return pressed;
    }

    float badge_width(const std::string &text)
    {
        return g_fonts.caption->CalcTextSizeA(g_fonts.caption->FontSize, FLT_MAX, 0, text.c_str()).x + S(18);
    }

    void badge(ImDrawList *draw, ImVec2 position, const std::string &text, ImU32 fill, ImU32 ink)
    {
        const float height = g_fonts.caption->FontSize + S(8);
        const ImVec2 end(position.x + badge_width(text), position.y + height);
        draw->AddRectFilled(position, end, fill, height * 0.5f);
        draw->AddText(g_fonts.caption,
                      g_fonts.caption->FontSize,
                      ImVec2(position.x + S(9), position.y + S(4)),
                      ink,
                      text.c_str());
    }

    void field(const char *name, const std::string &value)
    {
        if (value.empty())
            return;
        ImGui::PushFont(g_fonts.caption);
        ImGui::TextDisabled("%s", name);
        ImGui::PopFont();
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(value.c_str());
        ImGui::PopTextWrapPos();
    }

    bool list_row(const char *id, float width, float height, bool ticked)
    {
        auto *draw = ImGui::GetWindowDrawList();
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const ImVec2 end(start.x + width, start.y + height);
        draw->AddRectFilled(start, end, ticked ? rgba(28, 38, 52) : rgba(31, 31, 34));
        // The hover tint is the Selectable's; the row's own colour is under it.
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImGui::ColorConvertU32ToFloat4(rgba(255, 255, 255, 0.055f)));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImGui::ColorConvertU32ToFloat4(rgba(255, 255, 255, 0.09f)));
        const bool pressed = ImGui::Selectable(id, false, ImGuiSelectableFlags_AllowOverlap, ImVec2(width, height));
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered())
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ticked)
            draw->AddRectFilled(start, ImVec2(start.x + S(3), end.y), color::blue);
        draw->AddLine(ImVec2(start.x, end.y - S(1)), ImVec2(end.x, end.y - S(1)), rgba(255, 255, 255, 0.07f), S(1));
        return pressed;
    }

    bool toggle(const char *id, bool *on)
    {
        const ImVec2 size(S(42), S(22));
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const bool pressed = ImGui::InvisibleButton(id, size);
        if (pressed)
            *on = !*on;
        const bool hovered = ImGui::IsItemHovered();
        if (hovered)
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        auto *draw = ImGui::GetWindowDrawList();
        const float radius = size.y * 0.5f;
        draw->AddRectFilled(start,
                            ImVec2(start.x + size.x, start.y + size.y),
                            *on ? color::blue : rgba(255, 255, 255, hovered ? 0.2f : 0.13f),
                            radius);
        draw->AddCircleFilled(ImVec2(start.x + (*on ? size.x - radius : radius), start.y + radius),
                              radius - S(3),
                              *on ? color::ink : rgba(196, 202, 212));
        return pressed;
    }

    void begin_tile(const char *id, unsigned seed)
    {
        // The rough edge wobbles a pixel or two past the child, so it is drawn clipped to the parent instead.
        const ImVec2 parent_min = ImGui::GetWindowDrawList()->GetClipRectMin();
        const ImVec2 parent_max = ImGui::GetWindowDrawList()->GetClipRectMax();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(20), S(16)));
        ImGui::BeginChild(id,
                          ImVec2(0, 0),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        // The size is last frame's, which is the one an auto-sized child settles on.
        const ImVec2 a = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        auto *draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(
            ImVec2(std::max(parent_min.x, a.x - S(3)), std::max(parent_min.y, a.y - S(3))),
            ImVec2(std::min(parent_max.x, a.x + size.x + S(3)), std::min(parent_max.y, a.y + size.y + S(3))));
        rough_rect(draw, a, ImVec2(a.x + size.x, a.y + size.y), color::tile, seed);
        draw->PopClipRect();
    }

    void end_tile()
    {
        ImGui::EndChild();
    }

    void inline_caption(const char *text)
    {
        ImGui::SameLine(0, S(10));
        const float ascent_gap = g_fonts.bold->Ascent - g_fonts.caption->Ascent;
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ascent_gap);
        ImGui::PushFont(g_fonts.caption);
        ImGui::TextDisabled("%s", text);
        ImGui::PopFont();
    }

    bool primary_button(const char *label, ImVec2 size)
    {
        push_primary_button();
        const bool pressed = ImGui::Button(label, size);
        pop_primary_button();
        return pressed;
    }

} // namespace studio::gui
