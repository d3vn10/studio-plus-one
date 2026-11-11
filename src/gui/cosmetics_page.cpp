// The COSMETICS page. Four views, like the old Studio's workspace modes: BROWSE the installed cosmetics,
// NEW COSMETIC from a mesh and a donor item, REPLACE A GAME MESH (the old "replace original", e.g. the
// board deck), and NATIVE COSTUME packages. Every action is a registry command on the job system, so
// Activity keeps the studio-plus line for each; filtering and sorting the catalog is display only.
#include "gui/cosmetics_page.h"

#include "core/settings.h"
#include "gui/app.h"
#include "gui/look.h"
#include "gui/page_kit.h"
#include "gui/project_page.h"
#include "gui/widgets.h"

#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>

namespace studio::gui
{
    namespace fs = std::filesystem;
    namespace
    {

        constexpr const char *deck_mesh =
            "characters/skateboard/unlicensed/deck/generic/popsicle/2022/deck_gen_popsicle_mesh";
        constexpr const char *truck_mesh =
            "characters/skateboard/unlicensed/truck/generic/default/2022/truck_gen_default_mesh";
        constexpr const char *wheel_mesh =
            "characters/skateboard/unlicensed/wheel/generic/classic/2022/wheel_gen_classic_mesh";
        constexpr const char *preview_mesh = "characters/skateboard/static/static_skateboard_mesh";

        enum class View
        {
            browse,
            make,
            part,
            replace,
            costume
        };

        struct Item
        {
            std::string item, slot, status, texture, reason;
            Json details;
        };

        struct CosmeticsState
        {
            View view = View::browse;
            bool started = false, startup_run = false;
            Json startup = Json::object(); // set only by --page cosmetics; read once by start()
            std::string show;              // startup option: a part of the view to scroll to
            int show_frames = 0;

            // browse
            std::shared_ptr<Job> list, audit;
            bool audit_taken = false;
            std::vector<Item> items;
            std::vector<std::string> slots;
            std::string filter_text, filter_slot;
            int filter_status = 0; // 0 all, 1 ok, 2 fail
            int sort = 0;          // 0 name A-Z, 1 name Z-A, 2 slot A-Z
            std::string selected;

            // new cosmetic
            std::string donor, make_model, make_output;
            bool make_output_edited = false;
            std::shared_ptr<Job> make;

            // a model as its own truck item (cosmetic new-board-part)
            std::string part_model, part_name, part_donor, part_output;
            bool part_output_edited = false;
            std::shared_ptr<Job> part;

            // replace a game mesh
            std::string mesh = deck_mesh, model, output, find, routes, title, author, more_hidden;
            double scale = 1.0;
            bool output_edited = false, hide_deck = false, hide_trucks = false, hide_wheels = false, hide_self = false,
                 rigid = true;
            std::shared_ptr<Job> find_job, info, replace;
            std::string info_for;
            std::shared_ptr<Job> own_mesh; // mesh find for a picked truck item's own mesh
            std::string own_mesh_for;      // that item's last path part

            // native costume
            std::string package, costume_output, costume_fbmod;
            std::shared_ptr<Job> costume_build, costume_info, costume_donors;
        };
        CosmeticsState g_cos;

        bool board_slot(const std::string &slot)
        {
            const std::string s = kit::lower(slot);
            return s.starts_with("board_") || s == "truck" || s == "type_boardsticker";
        }

        void take_audit()
        {
            auto &s = g_cos;
            if (s.audit_taken || !kit::succeeded(s.audit))
                return;
            s.audit_taken = true;
            s.items.clear();
            std::set<std::string> slots;
            const Json r = kit::result(s.audit);
            for (const auto &i : r.value("items", Json::array()))
            {
                Item it;
                it.item = i.value("item", "");
                it.slot = i.value("slot", "");
                it.status = i.value("status", "");
                it.texture = i.value("texture", "");
                it.reason = i.value("reason", "");
                it.details = i;
                slots.insert(it.slot);
                s.items.push_back(std::move(it));
            }
            s.slots.assign(slots.begin(), slots.end());
        }

        void read_mesh()
        {
            auto &s = g_cos;
            s.info_for = s.mesh;
            s.info = kit::run("mesh", "info", {{"mesh", s.mesh}});
        }

        std::string default_output(const std::string &model, const char *ext)
        {
            const std::string stem = model.empty() ? "replaced_mesh" : path_utf8(fs::path(utf8_to_wide(model)).stem());
            return kit::data_path(L"Cosmetics", stem + ext);
        }

        // "a, b; c" -> ["a", "b", "c"]: comma, semicolon or newline separated, trimmed, empties dropped.
        Json split_list(const std::string &text)
        {
            Json out = Json::array();
            std::string part;
            for (size_t i = 0; i <= text.size(); ++i)
            {
                if (i == text.size() || text[i] == ',' || text[i] == ';' || text[i] == '\n')
                {
                    while (!part.empty() && part.front() == ' ')
                        part.erase(part.begin());
                    while (!part.empty() && part.back() == ' ')
                        part.pop_back();
                    if (!part.empty())
                        out.push_back(part);
                    part.clear();
                }
                else
                {
                    part += text[i];
                }
            }
            return out;
        }

        Json replace_args()
        {
            auto &s = g_cos;
            Json hide = Json::array();
            const std::string self = kit::lower(s.mesh);
            if (s.hide_deck && self != deck_mesh)
                hide.push_back(deck_mesh);
            if (s.hide_trucks && self != truck_mesh)
                hide.push_back(truck_mesh);
            if (s.hide_wheels && self != wheel_mesh)
                hide.push_back(wheel_mesh);
            for (const auto &more : split_list(s.more_hidden))
                if (kit::lower(more.get<std::string>()) != self)
                    hide.push_back(more);
            Json args = {{"mesh", s.mesh},
                         {"output", s.output},
                         {"route", split_list(s.routes)},
                         {"hide", hide},
                         {"title", s.title},
                         {"author", s.author}};
            if (s.hide_self)
                args["hide-mesh"] = true;
            else
                args["model"] = s.model;
            if (s.rigid)
                args["rigid"] = true;
            if (s.scale != 1.0)
                args["scale"] = s.scale;
            return args;
        }

        // A truck item may have a mesh of its own (licensed trucks: truck_royal_theroyal ->
        // .../truck_royal_theroyal_mesh). When mesh find has one named after the item, it replaces the generic truck
        // mesh picked meanwhile.
        void take_own_mesh()
        {
            auto &s = g_cos;
            if (!s.own_mesh || kit::running(s.own_mesh))
                return;
            const Json found = kit::result(s.own_mesh);
            const std::string want = s.own_mesh_for + "_mesh";
            s.own_mesh.reset();
            if (!found.is_object() || kit::lower(s.mesh) != truck_mesh)
                return; // the user picked another mesh meanwhile
            for (const auto &m : found.value("meshes", Json::array()))
            {
                const std::string name = m.value("mesh", "");
                if (kit::lower(kit::leaf(name)) == want)
                {
                    s.mesh = name;
                    read_mesh();
                    return;
                }
            }
        }

        void start()
        {
            auto &s = g_cos;
            if (s.started)
                return;
            s.started = true;
            s.list = kit::run("cosmetic", "list", Json::object()); // quick (about 3 s): counts and preview targets
            const Json &a = s.startup;
            const std::string view = a.value("view", "");
            if (view == "new")
                s.view = View::make;
            else if (view == "part")
                s.view = View::part;
            else if (view == "replace")
                s.view = View::replace;
            else if (view == "costume")
                s.view = View::costume;
            s.filter_text = a.value("text", "");
            s.show = a.value("show", "");
            if (a.contains("select"))
                s.selected = a.value("select", "");
            s.filter_slot = a.value("slot", "");
            if (a.contains("mesh"))
                s.mesh = a.value("mesh", "");
            if (a.contains("model"))
            {
                s.model = a.value("model", "");
                s.make_model = s.model;
            }
            if (a.contains("output"))
            {
                s.output = a.value("output", "");
                s.output_edited = true;
            }
            if (a.contains("donor"))
                s.donor = a.value("donor", "");
            if (a.value("hide", "") == "deck-parts")
                s.hide_trucks = s.hide_wheels = true;
            if (a.contains("rigid"))
                s.rigid = a.value("rigid", "") != "false";
            if (s.output.empty())
                s.output = default_output(s.model, ".fbmod");
            if (!s.startup_run)
                return;
            if (s.view == View::browse)
                s.audit = kit::run("cosmetic", "audit", Json::object());
            if (s.view == View::replace)
            {
                read_mesh();
                if (!s.model.empty())
                    s.replace = kit::run("mesh", "replace", replace_args());
            }
        }

        // ------------------------------------------------------------------ the view switch
        void tabs()
        {
            auto &s = g_cos;
            const std::pair<View, const char *> list[]{{View::browse, "BROWSE"},
                                                       {View::make, "NEW COSMETIC"},
                                                       {View::part, "OWN BOARD ITEM"},
                                                       {View::replace, "REPLACE A GAME MESH"},
                                                       {View::costume, "NATIVE COSTUME"}};
            for (const auto &[view, label] : list)
            {
                if (view != View::browse)
                    ImGui::SameLine(0, S(6));
                if (s.view == view ? primary_button(label) : ImGui::Button(label))
                    s.view = view;
            }
        }

        // ------------------------------------------------------------------ browse
        void catalog_summary()
        {
            auto &s = g_cos;
            const Json r = kit::result(s.list);
            if (!r.is_object())
            {
                kit::status(s.list, "Read the catalog");
                return;
            }
            const Json &counts = r.value("counts", Json::object());
            const Json &board = r.value("board_preview", Json::object());
            if (ImGui::BeginTable("##counts", 3, ImGuiTableFlags_SizingStretchSame))
            {
                ImGui::TableNextColumn();
                field("COSMETICS", kit::thousands(counts.value("cosmetics", 0LL)));
                ImGui::TableNextColumn();
                field("BOARD COSMETICS", kit::thousands(counts.value("board_cosmetics", 0LL)));
                ImGui::TableNextColumn();
                field("PREVIEW BOARD MESH", kit::leaf(board.value("mesh", "")));
                ImGui::EndTable();
            }
        }

        void browse_view()
        {
            auto &s = g_cos;
            take_audit();
            begin_tile("##cos_catalog", 71);
            kit::tile_title("INSTALLED COSMETICS");
            catalog_summary();
            if (s.items.empty())
            {
                kit::muted("LOAD ALL COSMETICS walks every cosmetic in the game with its slot, preview status and "
                           "texture. It takes "
                           "about a minute; the list stays here while Studio+ is open.");
                ImGui::BeginDisabled(kit::running(s.audit));
                if (primary_button("LOAD ALL COSMETICS"))
                {
                    s.audit = kit::run("cosmetic", "audit", Json::object());
                    s.audit_taken = false;
                }
                ImGui::EndDisabled();
                kit::status(s.audit, "Loaded");
                end_tile();
                return;
            }
            // Filters (display only; the list came from one cosmetic audit).
            ImGui::SetNextItemWidth(S(320));
            input_text("##text", s.filter_text, 0, "Search name, slot or path");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(S(220));
            if (ImGui::BeginCombo("##slot", s.filter_slot.empty() ? "All slots" : s.filter_slot.c_str()))
            {
                if (ImGui::Selectable("All slots", s.filter_slot.empty()))
                    s.filter_slot.clear();
                for (const auto &slot : s.slots)
                    if (ImGui::Selectable(slot.c_str(), slot == s.filter_slot))
                        s.filter_slot = slot;
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            const char *statuses[] = {"Every status", "Previewable", "Not previewable"};
            ImGui::SetNextItemWidth(S(170));
            ImGui::Combo("##status", &s.filter_status, statuses, 3);
            ImGui::SameLine();
            const char *sorts[] = {"Name A-Z", "Name Z-A", "Slot A-Z"};
            ImGui::SetNextItemWidth(S(130));
            ImGui::Combo("##sort", &s.sort, sorts, 3);
            ImGui::SameLine();
            if (ImGui::Button("RELOAD"))
            {
                s.audit = kit::run("cosmetic", "audit", Json::object());
                s.audit_taken = false;
                s.items.clear();
            }

            const std::string text = kit::lower(s.filter_text);
            std::vector<const Item *> shown;
            for (const auto &it : s.items)
            {
                if (!s.filter_slot.empty() && it.slot != s.filter_slot)
                    continue;
                if (s.filter_status == 1 && it.status != "ok")
                    continue;
                if (s.filter_status == 2 && it.status == "ok")
                    continue;
                if (!text.empty() && kit::lower(it.item).find(text) == std::string::npos &&
                    kit::lower(it.slot).find(text) == std::string::npos)
                    continue;
                shown.push_back(&it);
            }
            std::sort(shown.begin(),
                      shown.end(),
                      [&](const Item *a, const Item *b)
                      {
                          if (s.sort == 2 && a->slot != b->slot)
                              return a->slot < b->slot;
                          return s.sort == 1 ? kit::leaf(a->item) > kit::leaf(b->item)
                                             : kit::leaf(a->item) < kit::leaf(b->item);
                      });
            ImGui::TextDisabled("%zu / %zu cosmetics", shown.size(), s.items.size());

            const float list_height = S(330);
            const float row = S(44);
            ImGui::BeginChild("##items", ImVec2(ImGui::GetContentRegionAvail().x * 0.58f, list_height), 0);
            const float width = ImGui::GetContentRegionAvail().x;
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(shown.size()), row);
            while (clipper.Step())
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                {
                    const Item &it = *shown[static_cast<size_t>(i)];
                    ImGui::PushID(i);
                    const ImVec2 at = ImGui::GetCursorScreenPos();
                    if (list_row("##item", width, row, it.item == s.selected))
                        s.selected = it.item;
                    auto *draw = ImGui::GetWindowDrawList();
                    const ImVec4 clip(at.x, at.y, at.x + width - S(90), at.y + row);
                    draw->AddText(g_fonts.bold,
                                  g_fonts.bold->FontSize,
                                  ImVec2(at.x + S(12), at.y + S(5)),
                                  color::text,
                                  kit::leaf(it.item).c_str(),
                                  nullptr,
                                  0,
                                  &clip);
                    draw->AddText(g_fonts.caption,
                                  g_fonts.caption->FontSize,
                                  ImVec2(at.x + S(12), at.y + S(25)),
                                  color::muted,
                                  it.slot.c_str());
                    if (it.status != "ok")
                    {
                        const std::string mark = "NO PREVIEW";
                        badge(draw,
                              ImVec2(at.x + width - badge_width(mark) - S(8), at.y + S(12)),
                              mark,
                              color::tile_grey,
                              color::text);
                    }
                    ImGui::PopID();
                }
            ImGui::EndChild();
            ImGui::SameLine(0, S(16));
            ImGui::BeginChild("##item_details", ImVec2(0, list_height), 0);
            const Item *sel = nullptr;
            for (const auto &it : s.items)
                if (it.item == s.selected)
                    sel = &it;
            if (!sel)
            {
                kit::muted("Pick a cosmetic to see its details. A clothing or costume item can be the donor of a NEW "
                           "COSMETIC; "
                           "deck, wheel and grip items share one mesh, so change those with REPLACE A GAME MESH.");
            }
            else
            {
                kit::tile_title(kit::leaf(sel->item).c_str());
                field("SLOT", sel->slot);
                field("ITEM", sel->item);
                if (!sel->texture.empty())
                    field("MAIN TEXTURE", sel->texture);
                if (sel->status == "ok")
                {
                    field("SECTIONS / MATERIALS",
                          std::to_string(sel->details.value("sections", 0)) + " / " +
                              std::to_string(sel->details.value("materials", 0)));
                    for (const auto &w : sel->details.value("warning_messages", Json::array()))
                        kit::coloured(w.get<std::string>(), color::warning);
                }
                else
                {
                    field("NOT PREVIEWABLE", sel->reason);
                }
                ImGui::Spacing();
                if (board_slot(sel->slot))
                {
                    if (ImGui::Button("REPLACE ITS MESH"))
                    {
                        const std::string slot = kit::lower(sel->slot);
                        s.mesh = slot == "truck"                           ? truck_mesh
                                 : slot.find("wheel") != std::string::npos ? wheel_mesh
                                                                           : deck_mesh;
                        s.view = View::replace;
                        read_mesh();
                        if (slot == "truck")
                        {
                            s.own_mesh_for = kit::lower(kit::leaf(sel->item));
                            s.own_mesh = kit::run("mesh", "find", {{"text", s.own_mesh_for}});
                        }
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Decks, grips and wheels share one mesh per part, and most trucks share one "
                                          "too: replacing it "
                                          "changes it on every board. Licensed trucks have their own mesh, which is "
                                          "picked when found.");
                    if (kit::lower(sel->slot) == "truck")
                    {
                        ImGui::SameLine();
                        if (primary_button("CLONE AS MY OWN ITEM"))
                        {
                            s.part_donor = sel->item;
                            s.view = View::part;
                        }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("A new truck item with your model, cloned from this one. Only players "
                                              "who pick it see it.");
                    }
                }
                else if (primary_button("USE AS DONOR"))
                {
                    s.donor = sel->item;
                    s.view = View::make;
                }
                ImGui::SameLine();
                if (ImGui::Button("COPY PATH"))
                    copy_text(sel->item);
            }
            ImGui::EndChild();
            end_tile();
        }

        // ------------------------------------------------------------------ new cosmetic
        void make_view(App &app)
        {
            auto &s = g_cos;
            begin_tile("##cos_new", 72);
            kit::tile_title("NEW COSMETIC FROM A MESH");
            kit::muted(
                "Clones a donor cosmetic into a new item and puts your .glb or .fbx mesh on it, skinned to the donor's "
                "skeleton. Name the model file after the new item: 'My Jacket.glb' makes items/<slot>/own_my_jacket. "
                "Each material in the model must match one of the donor's material sections.");
            kit::caption("DONOR ITEM (pick one in BROWSE, or type its path)");
            ImGui::SetNextItemWidth(-1);
            input_text("##donor", s.donor, 0, "items/cust_fullbodycostume/own_costume_gen_isaacclarke_00001");
            kit::caption("MODEL (.glb or .fbx)");
			s.make_model = "";
            if (path_field("##make_model",
                           s.make_model,
                           app.window,
                           false,
                           {{L"Models (*.glb;*.fbx)", L"*.glb;*.fbx"}},
                           S(110)) &&
                !s.make_output_edited)
                s.make_output = default_output(s.make_model, ".fbproject");
            kit::caption("SAVE AS (.fbproject or .fbmod)");
            if (path_field("##make_output",
                           s.make_output,
                           app.window,
                           false,
                           {{L"Project (*.fbproject)", L"*.fbproject"}, {L"Frosty mod (*.fbmod)", L"*.fbmod"}},
                           S(110)))
                s.make_output_edited = true;
            const bool ready = !s.donor.empty() && !s.make_output.empty();
            ImGui::BeginDisabled(!ready || kit::running(s.make));
            if (primary_button("CREATE COSMETIC"))
                s.make = kit::run("cosmetic",
                                  "import-mesh",
                                  {{"donor", s.donor}, {"model", s.make_model}, {"output", s.make_output}});
            ImGui::EndDisabled();
            if (!ready)
            {
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("Needs a donor and a file to save with optional new mesh");
            }
            kit::status(s.make, "Created");
            const Json r = kit::result(s.make);
            if (r.is_object())
            {
                field("NEW ITEM", r.value("item", ""));
                field("SAVED",
                      r.value("output", "") + "  (" + kit::thousands(r.value("output_bytes", 0LL)) + " bytes)");
                if (r.contains("readback"))
                    field("CHECK IN BLENDER", r["readback"].value("path", ""));
            }
            end_tile();
        }

        // ------------------------------------------------------------------ a model as its own truck item
        void part_view(App &app)
        {
            auto &s = g_cos;
            begin_tile("##cos_part", 75);
            kit::tile_title("YOUR MODEL AS ITS OWN BOARD ITEM");
            kit::muted("Makes a NEW truck item that draws your model, such as a scooter. Only players who pick it see "
                       "it: every "
                       "other board stays as it is, unlike REPLACE A GAME MESH, which changes a part on every board. "
                       "Trucks are "
                       "the board part the game lets an item give its own geometry, so the item goes in the trucks "
                       "list, and the "
                       "deck and wheels picked with it still draw. The model must be in board space: Y up, metres, "
                       "board length "
                       "along Z, origin on the ground under the board centre.");
            kit::caption("YOUR MODEL (.glb, or .fbx with Blender)");
            if (path_field("##part_model",
                           s.part_model,
                           app.window,
                           false,
                           {{L"Models (*.glb;*.fbx)", L"*.glb;*.fbx"}},
                           S(110)))
            {
                if (s.part_name.empty())
                    s.part_name = path_utf8(fs::path(utf8_to_wide(s.part_model)).stem());
                if (!s.part_output_edited)
                    s.part_output = default_output(s.part_name.empty() ? s.part_model : s.part_name + ".glb", ".fbmod");
            }
            if (ImGui::BeginTable("##part_meta", 2, ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 1);
                ImGui::TableSetupColumn("donor", ImGuiTableColumnFlags_WidthStretch, 2);
                ImGui::TableNextColumn();
                kit::caption("ITEM NAME");
                ImGui::SetNextItemWidth(-1);
                input_text("##part_name", s.part_name, 0, "Razor Scooter");
                ImGui::TableNextColumn();
                kit::caption("TRUCK TO CLONE (optional: pick one in BROWSE)");
                ImGui::SetNextItemWidth(-1);
                input_text(
                    "##part_donor", s.part_donor, 0, "Empty: a generic truck (finding one takes about a minute)");
                ImGui::EndTable();
            }
            kit::caption("SAVE AS (.fbmod to build and install, or .fbproject)");
            if (path_field("##part_output",
                           s.part_output,
                           app.window,
                           false,
                           {{L"Frosty mod (*.fbmod)", L"*.fbmod"}, {L"Project (*.fbproject)", L"*.fbproject"}},
                           S(110)))
                s.part_output_edited = true;
            const bool ready = !s.part_model.empty() && !s.part_output.empty();
            ImGui::BeginDisabled(!ready || kit::running(s.part));
            if (primary_button("MAKE THE ITEM"))
                s.part = kit::run("cosmetic",
                                  "new-board-part",
                                  {{"model", s.part_model},
                                   {"output", s.part_output},
                                   {"name", s.part_name},
                                   {"donor", s.part_donor}});
            ImGui::EndDisabled();
            if (!ready)
            {
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("Needs a model and a file to save");
            }
            kit::status(s.part, "Made");
            const Json r = kit::result(s.part);
            if (r.is_object())
            {
                field("NEW ITEM", r.value("item", ""));
                field("SAVED",
                      r.value("output", "") + "  (" + kit::thousands(r.value("output_bytes", 0LL)) + " bytes)");
                const Json part = r.value("board_part", Json::object());
                field("CLONED FROM", part.value("donor", "") + "  (mesh " + kit::leaf(part.value("mesh", "")) + ")");
                field("MATERIALS NAMED", part.value("section_used", ""));
                if (r.contains("readback"))
                    field("CHECK IN BLENDER", r["readback"].value("path", ""));
                for (const auto &note : r.value("notes", Json::array()))
                    kit::muted(note.get<std::string>());
                if (ImGui::Button("OPEN FOLDER"))
                    ShellExecuteW(nullptr,
                                  L"open",
                                  fs::path(utf8_to_wide(r.value("output", ""))).parent_path().c_str(),
                                  nullptr,
                                  nullptr,
                                  SW_SHOWNORMAL);
                ImGui::SameLine();
                if (ImGui::Button("BUILD AND INSTALL IN PROJECT & MODS"))
                    build_in_project(app, r.value("output", ""));
            }
            end_tile();
        }

        // ------------------------------------------------------------------ replace a game mesh
        void mesh_picker()
        {
            auto &s = g_cos;
            kit::caption("GAME MESH");
            const std::pair<const char *, const char *> quick[]{
                {"DECK", deck_mesh}, {"TRUCKS", truck_mesh}, {"WHEELS", wheel_mesh}, {"PREVIEW BOARD", preview_mesh}};
            bool first = true;
            for (const auto &[label, name] : quick)
            {
                if (!first)
                    ImGui::SameLine(0, S(6));
                first = false;
                if (kit::lower(s.mesh) == name ? primary_button(label) : ImGui::Button(label))
                {
                    s.mesh = name;
                    read_mesh();
                }
            }
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - S(130));
            input_text("##mesh", s.mesh, 0, "characters/...._mesh");
            ImGui::SameLine();
            ImGui::BeginDisabled(s.mesh.empty() || kit::running(s.info));
            if (ImGui::Button("READ MESH", ImVec2(S(120), 0)))
                read_mesh();
            ImGui::EndDisabled();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - S(130));
            const bool enter =
                input_text("##find", s.find, ImGuiInputTextFlags_EnterReturnsTrue, "Find a mesh by name, e.g. truck");
            ImGui::SameLine();
            ImGui::BeginDisabled(s.find.empty() || kit::running(s.find_job));
            if (ImGui::Button("FIND", ImVec2(S(120), 0)) || (enter && !s.find.empty()))
                s.find_job = kit::run("mesh", "find", {{"text", s.find}});
            ImGui::EndDisabled();
            if (kit::running(s.find_job) || (s.find_job && !kit::succeeded(s.find_job)))
                kit::status(s.find_job, "Found");
            const Json found = kit::result(s.find_job);
            if (found.is_object())
            {
                const Json &meshes = found.value("meshes", Json::array());
                ImGui::TextDisabled("%zu meshes (click to pick; bundles: how many places load it)", meshes.size());
                const float row = S(28);
                ImGui::BeginChild(
                    "##found",
                    ImVec2(0, std::min(S(170), row * static_cast<float>(std::max<size_t>(meshes.size(), 1)) + S(4))),
                    0);
                const float width = ImGui::GetContentRegionAvail().x;
                int i = 0;
                for (const auto &m : meshes)
                {
                    ImGui::PushID(i++);
                    const std::string name = m.value("mesh", "");
                    const ImVec2 at = ImGui::GetCursorScreenPos();
                    if (list_row("##m", width, row, kit::lower(name) == kit::lower(s.mesh)))
                    {
                        s.mesh = name;
                        read_mesh();
                    }
                    ImGui::GetWindowDrawList()->AddText(
                        ImVec2(at.x + S(12), at.y + (row - ImGui::GetTextLineHeight()) * 0.5f),
                        color::text,
                        (name + "   (" + std::to_string(m.value("bundles", 0)) + ")").c_str());
                    ImGui::PopID();
                }
                ImGui::EndChild();
            }
        }

        void mesh_details()
        {
            auto &s = g_cos;
            if (s.info && s.info_for != s.mesh)
                return;
            if (kit::running(s.info) || (s.info && !kit::succeeded(s.info)))
                kit::status(s.info, "Read");
            const Json r = kit::result(s.info);
            if (!r.is_object())
                return;
            const Json lods = r.value("lods", Json::array());
            std::string sections;
            if (!lods.empty())
                for (const auto &sec : lods[0].value("sections", Json::array()))
                {
                    if (!sections.empty())
                        sections += ",  ";
                    sections +=
                        sec.value("section", "") + " (" + kit::thousands(sec.value("triangles", 0LL)) + " tris)";
                }
            field("SECTIONS (material names in your model that match these go there)", sections);
            field("LODS / LOADED BY",
                  std::to_string(lods.size()) + " / " + std::to_string(r.value("bundles", Json::array()).size()) +
                      (r.value("bundles", Json::array()).size() == 1 ? " bundle: " + r["bundles"][0].get<std::string>()
                                                                     : " bundles"));
            const std::string m = kit::lower(s.mesh);
            if (m == preview_mesh)
                kit::coloured(
                    "This is the cosmetic preview board (menus, one tutorial scene), not the board you ride. The old "
                    "ReSkate Studio replaced this one for the Razor Scooter.",
                    color::warning);
            else if (m == deck_mesh || m == truck_mesh || m == wheel_mesh)
                kit::coloured(
                    "Every board uses this mesh: replacing it changes it for every item in that slot, for everyone "
                    "with the mod.",
                    color::warning);
        }

        void replace_view(App &app)
        {
            auto &s = g_cos;
            begin_tile("##cos_replace", 73);
            kit::tile_title("REPLACE A GAME MESH");
            kit::muted(
                "Puts your model in place of one of the game's meshes, like the old Studio's \"replace original\". The "
                "game is not changed: you get a .fbproject or .fbmod for PROJECT & MODS. The model must be in the "
                "game's "
                "space: Y up, metres, a board part with its length along Z and the origin on the ground under the "
                "board centre.");
            ImGui::Spacing();
            take_own_mesh();
            if (kit::running(s.own_mesh))
                kit::muted("Looking for " + s.own_mesh_for + "'s own mesh...");
            mesh_picker();
            mesh_details();
            ImGui::Spacing();
            kit::scroll_here(s.show, s.show_frames, "model");
            kit::caption("YOUR MODEL (.glb, or .fbx with Blender)");
            ImGui::BeginDisabled(s.hide_self);
            if (path_field(
                    "##model", s.model, app.window, false, {{L"Models (*.glb;*.fbx)", L"*.glb;*.fbx"}}, S(110)) &&
                !s.output_edited)
                s.output = default_output(s.model, ".fbmod");
            ImGui::EndDisabled();
            kit::caption("SAVE AS (.fbmod to build and install, or .fbproject)");
            if (path_field("##output",
                           s.output,
                           app.window,
                           false,
                           {{L"Frosty mod (*.fbmod)", L"*.fbmod"}, {L"Project (*.fbproject)", L"*.fbproject"}},
                           S(110)))
                s.output_edited = true;
            kit::caption("MATERIAL ROUTES (optional: Material=Section, comma separated; unmatched materials go to the "
                         "largest section)");
            ImGui::SetNextItemWidth(-1);
            input_text("##routes", s.routes, 0, "Chrome=Deck_Mat, Grip=DeckTop_mat");
            if (ImGui::BeginTable("##mod_meta", 3, ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("title", ImGuiTableColumnFlags_WidthStretch, 3);
                ImGui::TableSetupColumn("author", ImGuiTableColumnFlags_WidthStretch, 2);
                ImGui::TableSetupColumn("scale", ImGuiTableColumnFlags_WidthStretch, 1);
                ImGui::TableNextColumn();
                kit::caption("MOD TITLE (optional)");
                ImGui::SetNextItemWidth(-1);
                input_text("##title", s.title, 0, "The model's file name");
                ImGui::TableNextColumn();
                kit::caption("AUTHOR (optional)");
                ImGui::SetNextItemWidth(-1);
                input_text("##author", s.author, 0, "Your name");
                ImGui::TableNextColumn();
                kit::caption("SCALE");
                ImGui::SetNextItemWidth(-1);
                ImGui::InputDouble("##scale", &s.scale, 0, 0, "%.3f");
                if (s.scale <= 0)
                    s.scale = 1.0;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Scale the model by this factor first, e.g. 0.01 for a model in centimetres");
                ImGui::EndTable();
            }
            ImGui::Spacing();
            auto option = [](const char *id, bool *value, const char *label, const char *tip)
            {
                toggle(id, value);
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(label);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", tip);
            };
            option("##hide_deck",
                   &s.hide_deck,
                   "Hide the deck",
                   "Also hide deck_gen_popsicle_mesh in the same mod (e.g. a whole ride on the trucks)");
            ImGui::SameLine(0, S(24));
            option("##hide_trucks",
                   &s.hide_trucks,
                   "Hide the trucks",
                   "Also hide truck_gen_default_mesh in the same mod (for a scooter or a board without trucks)");
            ImGui::SameLine(0, S(24));
            option(
                "##hide_wheels", &s.hide_wheels, "Hide the wheels", "Also hide wheel_gen_classic_mesh in the same mod");
            ImGui::SameLine(0, S(24));
            option("##rigid",
                   &s.rigid,
                   "Keep it rigid",
                   "Skin everything to each section's main bone, so no part turns with the wheels or trucks");
            ImGui::SameLine(0, S(24));
            option("##hide_self",
                   &s.hide_self,
                   "Hide this mesh instead",
                   "No model needed: the mesh is replaced by one 1 mm triangle");
            kit::caption("MORE MESHES TO HIDE (optional, comma separated game mesh names)");
            ImGui::SetNextItemWidth(-1);
            input_text("##more_hidden", s.more_hidden, 0, "e.g. characters/skateboard/.../truck_royal_theroyal_mesh");
            ImGui::Spacing();
            const bool ready = !s.mesh.empty() && !s.output.empty() && (s.hide_self || !s.model.empty());
            ImGui::BeginDisabled(!ready || kit::running(s.replace));
            if (primary_button(s.hide_self ? "HIDE MESH" : "REPLACE MESH"))
                s.replace = kit::run("mesh", "replace", replace_args());
            ImGui::EndDisabled();
            if (!ready)
            {
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("Needs a game mesh, a model and a file to save");
            }
            if (s.replace && s.replace->done.load())
                kit::scroll_here(s.show, s.show_frames, "result");
            kit::status(s.replace, "Saved");
            const Json r = kit::result(s.replace);
            if (r.is_object())
            {
                field("SAVED",
                      r.value("output", "") + "  (" + kit::thousands(r.value("output_bytes", 0LL)) + " bytes)");
                for (const auto &m : r.value("replaced", Json::array()))
                {
                    std::string line = kit::leaf(m.value("mesh", "")) + (m.value("hidden", false) ? ": hidden" : ":");
                    if (!m.value("hidden", false))
                    {
                        const Json lods = m.value("lods", Json::array());
                        if (!lods.empty())
                            for (const auto &sec : lods[0].value("after", Json::array()))
                                if (sec.value("triangles", 0) > 0)
                                    line += "  " + sec.value("section", "") + " " +
                                            kit::thousands(sec.value("triangles", 0LL)) + " tris";
                        line += "  (" + std::to_string(lods.size()) + " LODs)";
                    }
                    ImGui::Bullet();
                    ImGui::TextUnformatted(line.c_str());
                    for (const auto &route : m.value("routing", Json::array()))
                        if (!m.value("hidden", false))
                            kit::muted("      " + route.value("material", "(no material)") + " -> " +
                                       route.value("section", "") + " (" + route.value("how", "") + ")");
                    for (const auto &w : m.value("warnings", Json::array()))
                        kit::coloured(w.get<std::string>(), color::warning);
                }
                if (ImGui::Button("OPEN FOLDER"))
                    ShellExecuteW(nullptr,
                                  L"open",
                                  fs::path(utf8_to_wide(r.value("output", ""))).parent_path().c_str(),
                                  nullptr,
                                  nullptr,
                                  SW_SHOWNORMAL);
                ImGui::SameLine();
                if (ImGui::Button("BUILD AND INSTALL IN PROJECT & MODS"))
                    build_in_project(app, r.value("output", ""));
            }
            end_tile();
        }

        // ------------------------------------------------------------------ native costume
        void costume_view(App &app)
        {
            auto &s = g_cos;
            begin_tile("##cos_costume", 74);
            kit::tile_title("NATIVE COSTUME PACKAGE");
            kit::muted(
                "Builds a native full-body costume from a package folder: recipe.json, mesh.res and chunks\\ with "
                "exactly "
                "8 <GUID>.chunk files, plus optional textures. The costume is cloned from the Isaac Clarke donor.");
            kit::caption("PACKAGE FOLDER");
            path_field("##package", s.package, app.window, true, {}, S(110));
            kit::caption("SAVE AS (.fbmod or .fbproject)");
            path_field("##costume_output",
                       s.costume_output,
                       app.window,
                       false,
                       {{L"Frosty mod (*.fbmod)", L"*.fbmod"}, {L"Project (*.fbproject)", L"*.fbproject"}},
                       S(110));
            ImGui::BeginDisabled(s.package.empty() || s.costume_output.empty() || kit::running(s.costume_build));
            if (primary_button("VALIDATE AND BUILD"))
                s.costume_build = kit::run("costume", "build", {{"package", s.package}, {"output", s.costume_output}});
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(kit::running(s.costume_donors));
            if (ImGui::Button("CHECK THE DONOR"))
                s.costume_donors = kit::run("costume", "donors", Json::object());
            ImGui::EndDisabled();
            kit::status(s.costume_build, "Built");
            if (const Json r = kit::result(s.costume_build); r.is_object())
            {
                field("SAVED",
                      r.value("output", "") + "  (" + kit::thousands(r.value("output_bytes", 0LL)) + " bytes)");
                if (r.value("format", "") == "fbmod")
                    s.costume_fbmod = r.value("output", "");
            }
            kit::status(s.costume_donors, "Donor checked");
            if (const Json r = kit::result(s.costume_donors); r.is_object())
                for (auto it = r.begin(); it != r.end(); ++it)
                {
                    std::string name = it.key();
                    for (auto &ch : name)
                        ch = ch == '_' ? ' ' : static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                    field(name.c_str(), it->is_string() ? it->get<std::string>() : it->dump());
                }
            ImGui::Spacing();
            kit::caption("CHECK A COSTUME .FBMOD");
            path_field(
                "##costume_fbmod", s.costume_fbmod, app.window, false, {{L"Frosty mod (*.fbmod)", L"*.fbmod"}}, S(110));
            ImGui::BeginDisabled(s.costume_fbmod.empty() || kit::running(s.costume_info));
            if (ImGui::Button("CHECK"))
                s.costume_info = kit::run("costume", "info", {{"fbmod", s.costume_fbmod}});
            ImGui::EndDisabled();
            kit::status(s.costume_info, "Checked");
            if (const Json r = kit::result(s.costume_info); r.is_object())
                for (auto it = r.begin(); it != r.end(); ++it)
                {
                    std::string name = it.key();
                    for (auto &ch : name)
                        ch = ch == '_' ? ' ' : static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                    field(name.c_str(), it->is_string() ? it->get<std::string>() : it->dump());
                }
            end_tile();
        }
    } // namespace

    void cosmetics_startup(const Json &args, bool run)
    {
        g_cos.startup = args.is_object() ? args : Json::object();
        g_cos.startup_run = run;
    }

    void cosmetics_replace_mesh(App &app, const std::string &mesh)
    {
        start();
        g_cos.view = View::replace;
        g_cos.mesh = mesh;
        read_mesh();
        app.page = page::cosmetics;
    }

    void cosmetics_page(App &app)
    {
        start();
        kit::heading("COSMETICS");
        kit::muted(
            "Browse the installed cosmetics, make new ones from your meshes, or replace one of the game's meshes. "
            "Every button runs a studio-plus command; ACTIVITY keeps the command line for each run.");
        ImGui::Spacing();
        tabs();
        ImGui::Spacing();
        switch (g_cos.view)
        {
        case View::browse:
            browse_view();
            break;
        case View::make:
            make_view(app);
            break;
        case View::part:
            part_view(app);
            break;
        case View::replace:
            replace_view(app);
            break;
        case View::costume:
            costume_view(app);
            break;
        }
    }

} // namespace studio::gui
