#include "core/process.h"
#include "core/registry.h"
#include "core/settings.h"
#include <windows.h>
#include <chrono>
#include <thread>

namespace studio
{

    std::wstring quote_argument(std::wstring_view arg)
    {
        if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
            return std::wstring(arg);
        std::wstring out = L"\"";
        for (size_t i = 0;; ++i)
        {
            size_t backslashes = 0;
            while (i < arg.size() && arg[i] == L'\\')
            {
                ++i;
                ++backslashes;
            }
            if (i == arg.size())
            {
                out.append(backslashes * 2, L'\\');
                break;
            }
            if (arg[i] == L'"')
            {
                out.append(backslashes * 2 + 1, L'\\');
                out.push_back(L'"');
            }
            else
            {
                out.append(backslashes, L'\\');
                out.push_back(arg[i]);
            }
        }
        out.push_back(L'"');
        return out;
    }

    namespace
    {
        struct Handle
        {
            HANDLE h = nullptr;
            ~Handle()
            {
                if (h && h != INVALID_HANDLE_VALUE)
                    CloseHandle(h);
            }
        };

        // Reads a pipe to EOF, splitting on \n and \r so progress written with carriage returns still arrives.
        void pump(HANDLE pipe, std::string &all, const std::function<void(std::string_view)> &on_line)
        {
            std::string pending;
            char buffer[4096];
            DWORD got = 0;
            auto flush = [&]
            {
                if (!pending.empty() && on_line)
                    on_line(pending);
                pending.clear();
            };
            while (ReadFile(pipe, buffer, sizeof(buffer), &got, nullptr) && got > 0)
            {
                all.append(buffer, got);
                for (DWORD i = 0; i < got; ++i)
                {
                    char c = buffer[i];
                    if (c == '\n' || c == '\r')
                        flush();
                    else
                        pending.push_back(c);
                }
            }
            flush();
        }

        std::wstring environment_block(const std::vector<std::pair<std::wstring, std::wstring>> &extra)
        {
            std::wstring block;
            if (extra.empty())
                return block;
            LPWCH current = GetEnvironmentStringsW();
            for (LPWCH p = current; *p; p += wcslen(p) + 1)
            {
                std::wstring_view entry(p);
                bool overridden = false;
                for (const auto &[k, v] : extra)
                    if (entry.size() > k.size() && _wcsnicmp(entry.data(), k.c_str(), k.size()) == 0 &&
                        entry[k.size()] == L'=')
                        overridden = true;
                if (!overridden)
                {
                    block += entry;
                    block.push_back(L'\0');
                }
            }
            FreeEnvironmentStringsW(current);
            for (const auto &[k, v] : extra)
            {
                block += k + L"=" + v;
                block.push_back(L'\0');
            }
            block.push_back(L'\0');
            return block;
        }
    } // namespace

    ProcessResult run_process(const ProcessOptions &options)
    {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        Handle out_read, out_write, err_read, err_write;
        if (!CreatePipe(&out_read.h, &out_write.h, &sa, 0) || !CreatePipe(&err_read.h, &err_write.h, &sa, 0))
            throw Error("process_start_failed", "Could not create pipes");
        SetHandleInformation(out_read.h, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(err_read.h, HANDLE_FLAG_INHERIT, 0);

        std::wstring command_line = quote_argument(options.executable.native());
        for (const auto &a : options.args)
            command_line += L" " + quote_argument(utf8_to_wide(a));

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = out_write.h;
        si.hStdError = err_write.h;

        // A job object makes sure Blender and anything else the child starts dies with it.
        Handle job;
        job.h = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (job.h)
            SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

        std::wstring env = environment_block(options.extra_env);
        PROCESS_INFORMATION pi{};
        std::wstring cwd = options.working_dir.native();
        auto start = std::chrono::steady_clock::now();
        if (!CreateProcessW(options.executable.c_str(),
                            command_line.data(),
                            nullptr,
                            nullptr,
                            TRUE,
                            CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                            env.empty() ? nullptr : env.data(),
                            cwd.empty() ? nullptr : cwd.c_str(),
                            &si,
                            &pi))
        {
            DWORD code = GetLastError();
            throw Error("process_start_failed",
                        "Could not start " + path_utf8(options.executable) + " (Windows error " + std::to_string(code) +
                            ")",
                        {{"executable", path_utf8(options.executable)}, {"windows_error", code}});
        }
        Handle process{pi.hProcess}, thread{pi.hThread};
        if (job.h)
            AssignProcessToJobObject(job.h, process.h);
        ResumeThread(thread.h);
        CloseHandle(out_write.h);
        out_write.h = nullptr;
        CloseHandle(err_write.h);
        err_write.h = nullptr;

        ProcessResult result;
        std::thread out_thread([&] { pump(out_read.h, result.stdout_text, options.on_stdout_line); });
        std::thread err_thread([&] { pump(err_read.h, result.stderr_text, options.on_stderr_line); });

        while (WaitForSingleObject(process.h, 100) == WAIT_TIMEOUT)
        {
            if (options.cancel && options.cancel->load())
            {
                result.cancelled = true;
                if (job.h)
                    TerminateJobObject(job.h, 1);
                else
                    TerminateProcess(process.h, 1);
            }
        }
        out_thread.join();
        err_thread.join();
        DWORD code = 0;
        GetExitCodeProcess(process.h, &code);
        result.exit_code = static_cast<int>(code);
        result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        return result;
    }
} // namespace studio
