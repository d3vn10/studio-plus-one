#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace studio
{
    struct ProcessOptions
    {
        std::filesystem::path executable;
        std::vector<std::string> args; // UTF-8, quoted for Windows automatically
        std::filesystem::path working_dir;
        std::vector<std::pair<std::wstring, std::wstring>> extra_env;
        std::function<void(std::string_view line)> on_stdout_line;
        std::function<void(std::string_view line)> on_stderr_line;
        std::atomic<bool> *cancel = nullptr; // when set, the process tree is terminated
    };

    struct ProcessResult
    {
        int exit_code = -1;
        std::string stdout_text;
        std::string stderr_text;
        double seconds = 0;
        bool cancelled = false;
    };

    // Runs a child process with no window, streaming both pipes line by line. Throws Error("process_start_failed").
    ProcessResult run_process(const ProcessOptions &options);

    // Windows command-line quoting for one argument (CommandLineToArgvW rules).
    std::wstring quote_argument(std::wstring_view arg);
} // namespace studio
