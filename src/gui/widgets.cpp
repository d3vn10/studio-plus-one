#include "gui/widgets.h"

#include "core/settings.h"
#include "gui/look.h"

#include <shobjidl.h>
#include <wrl/client.h>

#include <filesystem>

namespace studio::gui
{
    using Microsoft::WRL::ComPtr;

    std::wstring pick_path(HWND owner, bool folder, const std::wstring &start, const std::vector<FileFilter> &filters)
    {
        ComPtr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
            return {};
        FILEOPENDIALOGOPTIONS options{};
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
                           (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
        if (!folder && !filters.empty())
        {
            std::vector<COMDLG_FILTERSPEC> specs;
            for (const auto &f : filters)
                specs.push_back({f.name, f.pattern});
            specs.push_back({L"All files", L"*.*"});
            dialog->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
        }
        // Start where the current value points, or beside it when it is a file.
        if (!start.empty())
        {
            std::error_code ec;
            std::filesystem::path at(start);
            if (!std::filesystem::is_directory(at, ec))
                at = at.parent_path();
            ComPtr<IShellItem> item;
            if (!at.empty() && std::filesystem::is_directory(at, ec) &&
                SUCCEEDED(SHCreateItemFromParsingName(at.c_str(), nullptr, IID_PPV_ARGS(&item))))
                dialog->SetFolder(item.Get());
        }
        if (FAILED(dialog->Show(owner)))
            return {};
        ComPtr<IShellItem> result;
        PWSTR path = nullptr;
        if (FAILED(dialog->GetResult(&result)) || FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &path)))
            return {};
        std::wstring out = path;
        CoTaskMemFree(path);
        return out;
    }

    namespace
    {
        int resize_callback(ImGuiInputTextCallbackData *data)
        {
            if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
            {
                auto *text = static_cast<std::string *>(data->UserData);
                text->resize(static_cast<size_t>(data->BufTextLen));
                data->Buf = text->data();
            }
            return 0;
        }
    } // namespace

    bool input_text(const char *id, std::string &value, ImGuiInputTextFlags flags, const char *hint)
    {
        flags |= ImGuiInputTextFlags_CallbackResize;
        if (hint)
            return ImGui::InputTextWithHint(
                id, hint, value.data(), value.capacity() + 1, flags, resize_callback, &value);
        return ImGui::InputText(id, value.data(), value.capacity() + 1, flags, resize_callback, &value);
    }

    bool take_drop(std::string &value)
    {
        std::lock_guard lock(g_drop.mutex);
        if (!g_drop.pending || g_drop.paths.empty())
            return false;
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        if (g_drop.point.x < a.x || g_drop.point.x > b.x || g_drop.point.y < a.y || g_drop.point.y > b.y)
            return false;
        value = wide_to_utf8(g_drop.paths.front());
        g_drop.pending = false;
        g_dropped_into = &value;
        return true;
    }

    bool path_field(const char *id,
                    std::string &value,
                    HWND owner,
                    bool folder,
                    const std::vector<FileFilter> &filters,
                    float browse_width)
    {
        ImGui::PushID(id);
        const float browse = browse_width > 0 ? browse_width : S(96);
        const ImVec2 start = ImGui::GetCursorScreenPos();
        ImGui::SetNextItemWidth(
            std::max(S(80), ImGui::GetContentRegionAvail().x - browse - ImGui::GetStyle().ItemSpacing.x));
        bool changed = input_text(
            "##value", value, 0, folder ? "Folder path, or drop a folder here" : "File path, or drop a file here");
        if (!g_drop_fallback)
            g_drop_fallback = &value;
        ImGui::SameLine();
        const bool clicked = ImGui::Button("BROWSE", ImVec2(browse, 0));
        const ImVec2 end = ImGui::GetItemRectMax();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(folder ? "Choose a folder (right-click for a file)"
                                     : "Choose a file (right-click for a folder)");
        bool pick_other = false;
        if (ImGui::BeginPopupContextItem("##browse_menu"))
        {
            if (ImGui::MenuItem(folder ? "Choose a file..." : "Choose a folder..."))
                pick_other = true;
            ImGui::EndPopup();
        }
        if (clicked || pick_other)
        {
            const bool want_folder = clicked ? folder : !folder;
            const auto picked =
                pick_path(owner, want_folder, utf8_to_wide(value), want_folder ? std::vector<FileFilter>{} : filters);
            if (!picked.empty())
            {
                value = wide_to_utf8(picked);
                changed = true;
            }
        }
        // A drop anywhere on the field and its button lands here.
        {
            std::lock_guard lock(g_drop.mutex);
            if (g_drop.pending && !g_drop.paths.empty() && g_drop.point.x >= start.x && g_drop.point.x <= end.x &&
                g_drop.point.y >= start.y && g_drop.point.y <= end.y)
            {
                value = wide_to_utf8(g_drop.paths.front());
                g_drop.pending = false;
                g_dropped_into = &value;
                changed = true;
            }
        }
        ImGui::PopID();
        return changed;
    }

    void copy_text(const std::string &text)
    {
        ImGui::SetClipboardText(text.c_str());
    }

    bool copy_button(const char *id, const std::string &text)
    {
        const bool pressed = ImGui::SmallButton(id);
        if (pressed)
            copy_text(text);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Copy to the clipboard");
        return pressed;
    }

} // namespace studio::gui
