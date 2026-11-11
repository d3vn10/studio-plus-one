#pragma once
// One definition per action. The GUI, the CLI and the MCP server all read commands from here,
// so an action can never exist in one of them and be missing from the others.
#include <nlohmann/json.hpp>
#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace studio
{
    using Json = nlohmann::ordered_json;

    enum class ParamType
    {
        String,
        Path,
        Integer,
        Number,
        Boolean,
        Enum,
        List
    };
    std::string_view to_string(ParamType type);

    struct Param
    {
        std::string name; // kebab-case: the CLI flag (--name) and the JSON / MCP argument key
        ParamType type = ParamType::String;
        std::string help;
        bool required = false;
        bool positional = false;          // CLI only: may be given in order without --name
        std::vector<std::string> choices; // ParamType::Enum
        std::optional<Json> default_value;
        std::optional<double> minimum; // Integer / Number: smallest allowed value
    };

    // A failure the caller can act on. `code` is stable and machine-readable (snake_case).
    struct Error : std::runtime_error
    {
        std::string code;
        Json details;
        Error(std::string code_, const std::string &message, Json details_ = Json::object())
            : std::runtime_error(message), code(std::move(code_)), details(std::move(details_))
        {
        }
    };

    struct Settings;

    class Context
    {
      public:
        explicit Context(Settings &s) : settings(s)
        {
        }
        Settings &settings;
        std::function<void(double fraction, std::string_view message)> on_progress; // fraction < 0: unknown
        std::function<void(std::string_view level, std::string_view message)> on_log;
        std::atomic<bool> *cancel = nullptr;

        void progress(double fraction, std::string_view message) const
        {
            if (on_progress)
                on_progress(fraction, message);
        }
        void log(std::string_view level, std::string_view message) const
        {
            if (on_log)
                on_log(level, message);
        }
        bool cancelled() const
        {
            return cancel && cancel->load();
        }

        // args["game-root"] if given, else the saved setting, else Error("game_root_missing").
        // Throws Error("not_a_game_root") when the folder has no Skate.exe.
        std::filesystem::path game_root(const Json &args) const;
    };

    using Handler = std::function<Json(Context &, const Json &args)>;

    struct Command
    {
        std::string group;       // e.g. "map"
        std::string name;        // e.g. "compile"
        std::string summary;     // one line, shown in lists
        std::string description; // full help text
        std::vector<Param> params;
        std::vector<std::string> examples; // CLI lines without the leading "studio-plus"
        bool writes_game = false;          // changes files inside the game install
        bool long_running = false;         // can take more than a few seconds
        Handler run;
        // Creates or replaces files outside the game (an output file, a staging folder). Set
        // automatically when a param is named output, staging-root or staging-dir.
        bool writes_files = false;
        bool read_only() const
        {
            return !writes_game && !writes_files;
        }

        std::string id() const
        {
            return group + " " + name;
        }
        std::string tool_name() const; // MCP tool name: group_name, '-' becomes '_'
        Json describe() const;         // machine-readable spec, also the MCP inputSchema source
        Json input_schema() const;     // JSON Schema for the arguments object
    };

    class Registry
    {
      public:
        static Registry &instance();
        void add(Command command);
        const std::vector<Command> &all() const
        {
            return commands_;
        }
        const Command *find(std::string_view group, std::string_view name) const;
        const Command *find_tool(std::string_view tool_name) const;
        std::vector<std::string> groups() const;
        Json describe() const;

      private:
        std::vector<Command> commands_;
    };

    // Fills defaults, checks required params, types and enum choices. Throws Error("invalid_arguments").
    Json normalise_args(const Command &command, const Json &args);

    // Shared parameter for every command that reads the game.
    Param game_root_param();

    // Runs a command with validated args. Never throws: returns {"ok":true,"result":...} or
    // {"ok":false,"error":{"code","message","details"}}.
    Json execute(Context &context, const Command &command, const Json &args);

    // Each command group registers itself from its own file.
    void register_all_commands();
} // namespace studio
