// The Studio+ window: borderless like the ReSkate launcher (ported from ReSkate (GPL-3.0)
// Launcher/gui.cpp run() and window_procedure, commit 3d259667d706), but resizable, maximisable and
// remembering where it was.
#include "gui/app.h"

#include "core/settings.h"
#include "gui/look.h"
#include "gui/maps_page.h"
#include "gui/assets_page.h"
#include "gui/cosmetics_page.h"
#include "gui/animations_page.h"
#include "gui/renderer.h"
#include "gui/widgets.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include <fstream>
#include <optional>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace studio::gui
{
    namespace fs = std::filesystem;
    namespace
    {

        App *g_app{};
        Renderer *g_renderer{};
        bool g_in_frame = false;
        bool g_sizing = false;
        bool g_fonts_dirty = false;
        constexpr UINT_PTR sizing_timer = 1;

        fs::path window_file()
        {
            return Settings::data_dir() / L"window.json";
        }

        struct Placement
        {
            RECT rect{};
            bool maximized = false;
        };

        std::optional<Placement> load_placement()
        {
            std::ifstream in(window_file(), std::ios::binary);
            if (!in)
                return std::nullopt;
            try
            {
                Json j = Json::parse(in);
                Placement p;
                p.rect.left = j.at("x").get<LONG>();
                p.rect.top = j.at("y").get<LONG>();
                p.rect.right = p.rect.left + j.at("width").get<LONG>();
                p.rect.bottom = p.rect.top + j.at("height").get<LONG>();
                p.maximized = j.value("maximized", false);
                if (p.rect.right - p.rect.left < 200 || p.rect.bottom - p.rect.top < 150)
                    return std::nullopt;
                // Somewhere a monitor still is, or it opens off-screen after a monitor is unplugged.
                if (!MonitorFromRect(&p.rect, MONITOR_DEFAULTTONULL))
                    return std::nullopt;
                return p;
            }
            catch (...)
            {
                return std::nullopt;
            }
        }

        void save_placement(HWND window)
        {
            WINDOWPLACEMENT placement{sizeof(placement)};
            if (!GetWindowPlacement(window, &placement))
                return;
            const RECT &r = placement.rcNormalPosition;
            Json j = {{"x", r.left},
                      {"y", r.top},
                      {"width", r.right - r.left},
                      {"height", r.bottom - r.top},
                      {"maximized", placement.showCmd == SW_SHOWMAXIMIZED || IsZoomed(window)}};
            std::error_code ec;
            fs::create_directories(Settings::data_dir(), ec);
            std::ofstream out(window_file(), std::ios::binary | std::ios::trunc);
            out << j.dump(2) << "\n";
        }

        void render_frame()
        {
            if (!g_app || !g_renderer || g_in_frame)
                return;
            g_in_frame = true;
            if (g_fonts_dirty)
            {
                // A move to a monitor with another DPI: fonts and style are rebuilt at the new scale.
                g_fonts_dirty = false;
                load_fonts();
                apply_style();
                g_renderer->reload_fonts();
            }
            RECT client{};
            GetClientRect(g_app->window, &client);
            g_renderer->resize(static_cast<UINT>(client.right - client.left),
                               static_cast<UINT>(client.bottom - client.top));
            ImGui_ImplDX12_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            draw_frame(*g_app);
            ImGui::Render();
            g_renderer->render();
            g_in_frame = false;
        }

        LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
        {
            if (ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam))
                return 1;
            switch (message)
            {
            case WM_NCCALCSIZE:
                // The whole window is client area: no system frame or caption, as in the launcher. A maximised
                // window hangs its frame off the screen's edges, so it is trimmed to the work area instead.
                if (wparam)
                {
                    if (IsZoomed(window))
                    {
                        auto &params = *reinterpret_cast<NCCALCSIZE_PARAMS *>(lparam);
                        MONITORINFO monitor{sizeof(monitor)};
                        if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor))
                            params.rgrc[0] = monitor.rcWork;
                    }
                    return 0;
                }
                break;
            case WM_NCHITTEST:
            {
                POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                ScreenToClient(window, &point);
                RECT client{};
                GetClientRect(window, &client);
                // The edges resize it, unlike the launcher's fixed window.
                if (!IsZoomed(window))
                {
                    const LONG edge = static_cast<LONG>(S(6));
                    const bool left = point.x < edge, right = point.x >= client.right - edge;
                    const bool top = point.y < edge, bottom = point.y >= client.bottom - edge;
                    if (top && left)
                        return HTTOPLEFT;
                    if (top && right)
                        return HTTOPRIGHT;
                    if (bottom && left)
                        return HTBOTTOMLEFT;
                    if (bottom && right)
                        return HTBOTTOMRIGHT;
                    if (left)
                        return HTLEFT;
                    if (right)
                        return HTRIGHT;
                    if (top)
                        return HTTOP;
                    if (bottom)
                        return HTBOTTOM;
                }
                // The top strip drags the borderless window, except over its buttons or an open popup.
                const bool popup =
                    ImGui::GetCurrentContext() && ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
                if (!popup && point.y >= 0 && static_cast<float>(point.y) < S(title_bar_height) &&
                    static_cast<float>(point.x) <
                        static_cast<float>(client.right) - S(window_button_width * window_button_count))
                    return HTCAPTION;
                return HTCLIENT;
            }
            case WM_GETMINMAXINFO:
            {
                auto &info = *reinterpret_cast<MINMAXINFO *>(lparam);
                info.ptMinTrackSize.x = static_cast<LONG>(S(min_width));
                info.ptMinTrackSize.y = static_cast<LONG>(S(min_height));
                return 0;
            }
            case WM_SIZE:
                // While an edge is being dragged the main loop is stuck in the system's sizing loop, so the
                // frame is drawn from here to keep the content following the edge.
                if (wparam != SIZE_MINIMIZED && g_sizing)
                    render_frame();
                return 0;
            case WM_ENTERSIZEMOVE:
                g_sizing = true;
                SetTimer(window, sizing_timer, 16, nullptr);
                return 0;
            case WM_EXITSIZEMOVE:
                g_sizing = false;
                KillTimer(window, sizing_timer);
                return 0;
            case WM_TIMER:
                if (wparam == sizing_timer)
                {
                    render_frame();
                    return 0;
                }
                break;
            case WM_DPICHANGED:
            {
                g_scale = static_cast<float>(HIWORD(wparam)) / 96.0f;
                g_fonts_dirty = true;
                const auto *suggested = reinterpret_cast<const RECT *>(lparam);
                SetWindowPos(window,
                             nullptr,
                             suggested->left,
                             suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            case WM_SYSCOMMAND:
                if ((wparam & 0xfff0) == SC_KEYMENU)
                    return 0;
                break;
            case WM_DROPFILES:
            {
                const auto drop = reinterpret_cast<HDROP>(wparam);
                const auto count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                POINT point{};
                DragQueryPoint(drop, &point);
                {
                    std::lock_guard lock(g_drop.mutex);
                    g_drop.paths.clear();
                    for (UINT i = 0; i < count; ++i)
                    {
                        std::wstring path(DragQueryFileW(drop, i, nullptr, 0) + 1, L' ');
                        path.resize(DragQueryFileW(drop, i, path.data(), static_cast<UINT>(path.size())));
                        g_drop.paths.emplace_back(path);
                    }
                    g_drop.point = ImVec2(static_cast<float>(point.x), static_cast<float>(point.y));
                    g_drop.pending = !g_drop.paths.empty();
                }
                DragFinish(drop);
                SetForegroundWindow(window);
                return 0;
            }
            case WM_CLOSE:
                if (g_app && g_jobs.running() && !g_app->close_confirmed)
                {
                    g_app->close_requested = true;
                    return 0;
                }
                save_placement(window);
                DestroyWindow(window);
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            }
            return DefWindowProcW(window, message, wparam, lparam);
        }

        // Startup options, for scripts and screenshots:
        //   --page home|maps|cosmetics|animations|project|assets|settings|commands
        //   --command "<group> <name>" [--arg name=value ...] [--run]   open a command in the runner, fill it, run it
        //   --activity                                                  open the Activity drawer
        struct Options
        {
            std::optional<page> page;
            std::string command;
            Json args = Json::object();
            bool run = false;
            bool activity = false;
        };

        Options parse_options()
        {
            Options options;
            int count = 0;
            wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &count);
            if (!argv)
                return options;
            std::vector<std::string> args;
            for (int i = 1; i < count; ++i)
                args.push_back(wide_to_utf8(argv[i]));
            LocalFree(argv);
            static const std::pair<const char *, page> pages[]{{"home", page::home},
                                                               {"maps", page::maps},
                                                               {"cosmetics", page::cosmetics},
                                                               {"animations", page::animations},
                                                               {"project", page::project},
                                                               {"assets", page::assets},
                                                               {"settings", page::settings},
                                                               {"commands", page::commands}};
            for (size_t i = 0; i < args.size(); ++i)
            {
                const auto &a = args[i];
                const bool has_value = i + 1 < args.size();
                if (a == "--page" && has_value)
                {
                    const auto &name = args[++i];
                    for (const auto &[key, page] : pages)
                        if (name == key)
                            options.page = page;
                }
                else if (a == "--command" && has_value)
                {
                    options.command = args[++i];
                }
                else if (a == "--arg" && has_value)
                {
                    const auto &pair = args[++i];
                    const auto eq = pair.find('=');
                    if (eq == std::string::npos)
                        continue;
                    const std::string name = pair.substr(0, eq), value = pair.substr(eq + 1);
                    // A name given twice becomes a list, as --name twice does on the CLI.
                    if (options.args.contains(name))
                    {
                        if (!options.args[name].is_array())
                            options.args[name] = Json::array({options.args[name]});
                        options.args[name].push_back(value);
                    }
                    else
                    {
                        options.args[name] = value;
                    }
                }
                else if (a == "--run")
                {
                    options.run = true;
                }
                else if (a == "--activity")
                {
                    options.activity = true;
                }
            }
            return options;
        }

        void apply_options(App &app, const Options &options)
        {
            if (options.page)
                app.page = *options.page;
            if (options.activity)
                app.activity_open = true;
            if (options.page == page::maps)
                maps_startup(options.args, options.run);
            if (options.page == page::assets)
                assets_startup(options.args);
            if (options.page == page::cosmetics)
                cosmetics_startup(options.args, options.run);
            if (options.page == page::animations)
                animations_startup(options.args, options.run);
            if (options.command.empty())
                return;
            const auto space = options.command.find(' ');
            if (space == std::string::npos)
                return;
            const Command *command =
                Registry::instance().find(options.command.substr(0, space), options.command.substr(space + 1));
            if (!command)
                return;
            app.page = page::commands;
            app.runner.bind(command);
            // A List param given once arrives as a string; the form wants it as a list.
            Json args = options.args;
            for (const auto &p : command->params)
                if (p.type == ParamType::List && args.contains(p.name) && args[p.name].is_string())
                    args[p.name] = Json::array({args[p.name]});
            app.runner.fill(args);
            if (options.run)
                app.runner.run();
        }

    } // namespace

    int run_app()
    {
        const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const Options options = parse_options();

        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        g_scale = std::max(1.0f, static_cast<float>(GetDpiForSystem()) / 96.0f);
        const auto work_width = static_cast<float>(work.right - work.left);
        const auto work_height = static_cast<float>(work.bottom - work.top);
        // The first run opens at the design size, or smaller when the screen is.
        const int width = static_cast<int>(std::min(S(design_width), work_width * 0.94f));
        const int height = static_cast<int>(std::min(S(design_height), work_height * 0.94f));

        const auto instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW window_class{sizeof(window_class)};
        window_class.style = CS_HREDRAW | CS_VREDRAW;
        window_class.lpfnWndProc = window_procedure;
        window_class.hInstance = instance;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
        window_class.hIconSm = static_cast<HICON>(LoadImageW(instance,
                                                             MAKEINTRESOURCEW(1),
                                                             IMAGE_ICON,
                                                             GetSystemMetrics(SM_CXSMICON),
                                                             GetSystemMetrics(SM_CYSMICON),
                                                             LR_DEFAULTCOLOR));
        window_class.lpszClassName = L"ReSkateStudioPlus";
        RegisterClassExW(&window_class);

        // WS_THICKFRAME and WS_CAPTION give it the system's resizing, snapping and minimise/maximise
        // animations; WM_NCCALCSIZE takes the frame they would draw away again.
        const HWND window =
            CreateWindowExW(WS_EX_APPWINDOW,
                            window_class.lpszClassName,
                            L"ReSkate Studio+",
                            WS_POPUP | WS_THICKFRAME | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
                            work.left + (work.right - work.left - width) / 2,
                            work.top + (work.bottom - work.top - height) / 2,
                            width,
                            height,
                            nullptr,
                            nullptr,
                            instance,
                            nullptr);
        if (!window)
        {
            MessageBoxW(nullptr, L"Cannot create the Studio+ window.", L"ReSkate Studio+", MB_ICONERROR);
            return 1;
        }
        const DWORD corners = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(window, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corners, sizeof(corners));
        const BOOL dark = TRUE;
        DwmSetWindowAttribute(window, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
        // WS_THICKFRAME keeps the system drop shadow; the frame itself goes in WM_NCCALCSIZE.
        SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

        // Back where it was last time, still hidden so nothing is drawn before the fonts exist.
        const auto placement = load_placement();
        if (placement)
        {
            WINDOWPLACEMENT restore{sizeof(restore)};
            restore.showCmd = SW_HIDE;
            restore.rcNormalPosition = placement->rect;
            SetWindowPlacement(window, &restore);
        }
        g_scale = static_cast<float>(GetDpiForWindow(window)) / 96.0f;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().LogFilename = nullptr;
        load_fonts();
        apply_style();
        g_fonts_dirty = false;
        ImGui_ImplWin32_Init(window);
        Renderer renderer;
        if (!renderer.init(window))
        {
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            DestroyWindow(window);
            MessageBoxW(nullptr,
                        L"This PC's graphics driver cannot draw the Studio+ window. Update your graphics driver "
                        L"and try again.",
                        L"ReSkate Studio+",
                        MB_ICONERROR);
            if (com)
                CoUninitialize();
            return 1;
        }

        App app;
        app.window = window;
        app.renderer = &renderer;
        init_app(app);
        apply_options(app, options);
        g_app = &app;
        g_renderer = &renderer;

        DragAcceptFiles(window, TRUE);
        ShowWindow(window, placement && placement->maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
        UpdateWindow(window);

        bool running = true;
        while (running)
        {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
                if (message.message == WM_QUIT)
                    running = false;
            }
            if (!running)
                break;
            if (IsIconic(window))
            {
                Sleep(50);
                continue;
            }
            render_frame();
        }

        g_app = nullptr;
        g_renderer = nullptr;
        // Commands still running are cancelled; one that ignores it is not waited for forever.
        g_jobs.cancel_all();
        const bool finished = g_jobs.wait_all(5000);
        renderer.shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        if (com)
            CoUninitialize();
        // A worker still inside a command would outlive the registry it reads; leave without tearing it down.
        if (!finished)
            ExitProcess(0);
        return 0;
    }

} // namespace studio::gui
