#include "mcp/server.h"
#include "core/registry.h"
#include "core/settings.h"
#include "version.h"
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <cstdio>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace studio
{
    namespace
    {
        constexpr const char *protocol_version = "2025-06-18";

        std::mutex g_out;

        void send(const Json &message)
        {
            std::string text = message.dump() + "\n";
            std::lock_guard lock(g_out);
            fwrite(text.data(), 1, text.size(), stdout);
            fflush(stdout);
        }

        void reply(const Json &id, Json result)
        {
            send({{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
        }

        void reply_error(const Json &id, int code, const std::string &message)
        {
            send({{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}});
        }

        std::string instructions()
        {
            return "ReSkate Studio+ builds mods for skate. running under ReSkate: custom maps from .blend/.fbx, "
                   "cosmetics, "
                   "animations, and inspection of the game's assets. Tools are named <group>_<command>. Start with "
                   "studio_doctor to check the ReSkate folder, Blender and engine are set; fix anything missing with "
                   "studio_set. "
                   "Paths are Windows paths on the machine running Studio+. Tools that change the ReSkate folder say "
                   "so in their "
                   "description. Long tools report progress.";
        }

        Json tool_list()
        {
            Json tools = Json::array();
            for (const auto &c : Registry::instance().all())
            {
                std::string description = c.summary + ".\n\n" + c.description;
                if (c.writes_game)
                    description += "\n\nChanges files inside the ReSkate folder.";
                else if (c.writes_files)
                    description += "\n\nWrites or replaces files at the output path you give.";
                if (!c.examples.empty())
                {
                    description += "\n\nCLI equivalent:";
                    for (const auto &e : c.examples)
                        description += "\n  studio-plus " + e;
                }
                Json tool = {{"name", c.tool_name()},
                             {"title", c.id()},
                             {"description", description},
                             {"inputSchema", c.input_schema()}};
                bool read_only = c.read_only() && !(c.group == "studio" && (c.name == "set" || c.name == "raw"));
                tool["annotations"] = {{"readOnlyHint", read_only},
                                       {"destructiveHint", c.writes_game || c.writes_files},
                                       {"openWorldHint", false}};
                tools.push_back(tool);
            }
            return tools;
        }

        struct Running
        {
            std::atomic<bool> cancel{false};
            std::thread worker;
        };
        std::mutex g_running_lock;
        std::map<std::string, std::shared_ptr<Running>> g_running;

        void call_tool(const Json &id, const Json &params)
        {
            std::string name = params.value("name", "");
            const Command *command = Registry::instance().find_tool(name);
            if (!command)
            {
                reply_error(id, -32602, "Unknown tool: " + name);
                return;
            }
            Json arguments =
                params.contains("arguments") && params["arguments"].is_object() ? params["arguments"] : Json::object();
            Json progress_token = params.contains("_meta") && params["_meta"].contains("progressToken")
                                      ? params["_meta"]["progressToken"]
                                      : Json();

            auto running = std::make_shared<Running>();
            std::string key = id.dump();
            {
                std::lock_guard lock(g_running_lock);
                g_running[key] = running;
            }
            running->worker = std::thread(
                [id, arguments, progress_token, command, running, key]
                {
                    Settings settings = Settings::load();
                    Context context(settings);
                    context.cancel = &running->cancel;
                    Json logs = Json::array();
                    std::mutex logs_lock;
                    double last = 0;
                    bool known = false;
                    context.on_progress = [&](double fraction, std::string_view message)
                    {
                        if (progress_token.is_null())
                            return;
                        Json p = {{"progressToken", progress_token}, {"message", message}};
                        // MCP progress must always increase. Known fractions are percentages out of 100; an
                        // unknown step nudges the value up slightly so it never jumps or reads as finished.
                        double value = fraction >= 0 ? fraction * 100.0 : last + 0.01;
                        if (value <= last)
                            value = last + 0.01;
                        if (fraction >= 0)
                            known = true;
                        if (known && value > 99.99)
                            value = 99.99;
                        last = value;
                        p["progress"] = value;
                        if (known)
                            p["total"] = 100.0;
                        send({{"jsonrpc", "2.0"}, {"method", "notifications/progress"}, {"params", p}});
                    };
                    context.on_log = [&](std::string_view level, std::string_view message)
                    {
                        std::lock_guard lock(logs_lock);
                        if (logs.size() < 200)
                            logs.push_back({{"level", level}, {"message", message}});
                    };
                    Json outcome = execute(context, *command, arguments);
                    if (!logs.empty())
                        outcome["log"] = logs;
                    bool ok = outcome["ok"].get<bool>();
                    if (!running->cancel)
                    {
                        Json result = {{"content", Json::array({{{"type", "text"}, {"text", outcome.dump(2)}}})},
                                       {"structuredContent", outcome},
                                       {"isError", !ok}};
                        reply(id, result);
                    }
                    std::lock_guard lock(g_running_lock);
                    if (auto it = g_running.find(key); it != g_running.end())
                    {
                        it->second->worker.detach();
                        g_running.erase(it);
                    }
                });
        }

        void handle(const Json &message)
        {
            if (!message.is_object())
                return;
            std::string method = message.value("method", "");
            bool is_request = message.contains("id");
            Json id = is_request ? message["id"] : Json();
            Json params = message.contains("params") ? message["params"] : Json::object();

            if (method == "initialize")
            {
                std::string requested = params.value("protocolVersion", protocol_version);
                reply(id,
                      {{"protocolVersion", requested.empty() ? protocol_version : requested},
                       {"capabilities", {{"tools", {{"listChanged", false}}}}},
                       {"serverInfo",
                        {{"name", "reskate-studio-plus"},
                         {"title", "ReSkate Studio+"},
                         {"version", STUDIO_PLUS_VERSION}}},
                       {"instructions", instructions()}});
            }
            else if (method == "ping")
            {
                reply(id, Json::object());
            }
            else if (method == "tools/list")
            {
                reply(id, {{"tools", tool_list()}});
            }
            else if (method == "tools/call")
            {
                call_tool(id, params);
            }
            else if (method == "notifications/cancelled")
            {
                std::string key = params.contains("requestId") ? params["requestId"].dump() : "";
                std::lock_guard lock(g_running_lock);
                if (auto it = g_running.find(key); it != g_running.end())
                    it->second->cancel = true;
            }
            else if (method.rfind("notifications/", 0) == 0)
            {
                // initialized and other notifications need no answer
            }
            else if (is_request)
            {
                reply_error(id, -32601, "Method not found: " + method);
            }
        }
    } // namespace

    int run_mcp_server()
    {
        _setmode(_fileno(stdin), _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
        std::string line;
        while (std::getline(std::cin, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty())
                continue;
            Json message;
            try
            {
                message = Json::parse(line);
            }
            catch (const std::exception &)
            {
                send({{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32700}, {"message", "Parse error"}}}});
                continue;
            }
            if (message.is_array())
                for (const auto &m : message)
                    handle(m);
            else
                handle(message);
        }
        // stdin closed: let running tools finish so their replies are not cut off.
        for (;;)
        {
            std::shared_ptr<Running> running;
            {
                std::lock_guard lock(g_running_lock);
                if (g_running.empty())
                    break;
                running = g_running.begin()->second;
            }
            Sleep(100);
        }
        return 0;
    }
} // namespace studio
