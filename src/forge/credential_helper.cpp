#include "credential_helper.h"

#include "util/programs.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace tortilla::credential_helper
{
namespace
{
std::mutex runner_mutex;
runner fake;

#ifdef _WIN32
struct handle
{
    HANDLE value = nullptr;
    ~handle()
    {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
    void close()
    {
        if (value) CloseHandle(value);
        value = nullptr;
    }
};

std::wstring wide(const std::string& s)
{
    const int count =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(std::max(0, count)), L'\0');
    if (count)
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), out.data(),
                            count);
    return out;
}

std::wstring quote(const std::wstring& s)
{
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : s)
    {
        if (c == L'\\')
        {
            ++slashes;
            continue;
        }
        out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L'"';
}

std::expected<reply, std::string> native(const request& r)
{
    const auto program = wide(find_program(r.program));
    if (program.empty()) return std::unexpected("Git is not installed.");
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    handle input_read, input_write, output_read, output_write, errors;
    if (!CreatePipe(&input_read.value, &input_write.value, &security, 0) ||
        !CreatePipe(&output_read.value, &output_write.value, &security, 0))
        return std::unexpected("Couldn't create Git credential pipes.");
    SetHandleInformation(input_write.value, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(output_read.value, HANDLE_FLAG_INHERIT, 0);
    errors.value = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (errors.value == INVALID_HANDLE_VALUE)
        return std::unexpected("Couldn't discard Git credential stderr.");

    // https://learn.microsoft.com/en-us/windows/win32/procthread/changing-environment-variables
    // CreateProcess requires case-insensitive Unicode ordering, without locale.
    struct environment_less
    {
        bool operator()(const std::wstring& a, const std::wstring& b) const
        {
            return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
        }
    };
    std::map<std::wstring, std::wstring, environment_less> env;
    if (auto block = GetEnvironmentStringsW())
    {
        for (const wchar_t* p = block; *p; p += wcslen(p) + 1)
        {
            const std::wstring item(p);
            const auto at = item.find(L'=', item.starts_with(L'=') ? 1 : 0);
            if (at != std::wstring::npos) env[item.substr(0, at)] = item.substr(at + 1);
        }
        FreeEnvironmentStringsW(block);
    }
    for (const auto& [name, value] : r.environment)
    {
        const auto key = wide(name);
        std::erase_if(env,
                      [&](const auto& entry) {
                          return CompareStringOrdinal(entry.first.c_str(), -1, key.c_str(), -1,
                                                      TRUE) == CSTR_EQUAL;
                      });
        // An empty value unsets the child variable; core.askPass is also
        // overridden on the Git command line, so no fallback can launch it.
        if (!value.empty()) env[key] = wide(value);
    }
    std::wstring environment;
    for (const auto& [name, value] : env) environment += name + L'=' + value + L'\0';
    environment += L'\0';
    std::wstring command = quote(program);
    for (const auto& arg : r.arguments) command += L' ' + quote(wide(arg));
    const auto directory = wide(r.directory);

    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> attributes(bytes);
    auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &bytes))
        return std::unexpected("Couldn't initialize Git credential child handles.");
    struct attribute_guard
    {
        LPPROC_THREAD_ATTRIBUTE_LIST p;
        ~attribute_guard() { DeleteProcThreadAttributeList(p); }
    } guard{list};
    HANDLE inherited[]{input_read.value, output_write.value, errors.value};
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                   sizeof(inherited), nullptr, nullptr))
        return std::unexpected("Couldn't restrict Git credential child handles.");
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.StartupInfo.hStdInput = input_read.value;
    startup.StartupInfo.hStdOutput = output_write.value;
    startup.StartupInfo.hStdError = errors.value;
    startup.lpAttributeList = list;
    handle job;
    job.value = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation,
                                               &limits, sizeof(limits)))
        return std::unexpected("Couldn't isolate Git credential child.");
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT |
                            EXTENDED_STARTUPINFO_PRESENT,
                        environment.data(), directory.empty() ? nullptr : directory.c_str(),
                        &startup.StartupInfo, &process))
        return std::unexpected("Couldn't start Git credential lookup.");
    handle child{process.hProcess}, thread{process.hThread};
    if (!AssignProcessToJobObject(job.value, child.value))
    {
        TerminateProcess(child.value, 1);
        WaitForSingleObject(child.value, INFINITE);
        return std::unexpected("Couldn't isolate Git credential child.");
    }
    input_read.close();
    output_write.close();
    // Input is checked below to fit an anonymous pipe before starting any
    // child, so this cannot block on a helper that does not read stdin.
    DWORD written = 0;
    const bool fed =
        WriteFile(input_write.value, r.input.data(), DWORD(r.input.size()), &written, nullptr) &&
        written == r.input.size();
    input_write.close();
    ResumeThread(thread.value);
    if (!fed)
    {
        TerminateJobObject(job.value, 1);
        return std::unexpected("Couldn't write Git credential request.");
    }
    reply out;
    std::array<char, 4096> buffer;
    const auto until = std::chrono::steady_clock::now() + r.timeout;
    for (;;)
    {
        if (std::chrono::steady_clock::now() >= until)
        {
            out.timed_out = true;
            break;
        }
        DWORD available = 0;
        const bool pipe =
            PeekNamedPipe(output_read.value, nullptr, 0, nullptr, &available, nullptr);
        if (available)
        {
            DWORD count = 0;
            if (!ReadFile(output_read.value, buffer.data(),
                          std::min<DWORD>(available, DWORD(buffer.size())), &count, nullptr))
                break;
            if (out.output.size() + count > r.output_limit)
            {
                out.exceeded_limit = true;
                break;
            }
            out.output.append(buffer.data(), count);
            continue;
        }
        if (!pipe || WaitForSingleObject(child.value, 0) == WAIT_OBJECT_0) break;
        if (std::chrono::steady_clock::now() >= until)
        {
            out.timed_out = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (out.timed_out || out.exceeded_limit) TerminateJobObject(job.value, 1);
    const auto remaining =
        std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(
                                      until - std::chrono::steady_clock::now())
                                      .count());
    if (WaitForSingleObject(child.value, DWORD(remaining)) == WAIT_TIMEOUT)
    {
        out.timed_out = true;
        TerminateJobObject(job.value, 1);
        WaitForSingleObject(child.value, INFINITE);
    }
    DWORD code = 1;
    GetExitCodeProcess(child.value, &code);
    out.exit_code = int(code);
    return out;
}
#else
std::expected<reply, std::string> native(const request& r)
{
    int input[2]{-1, -1}, output[2]{-1, -1};
    if (pipe(input) || pipe(output))
    {
        for (int fd : {input[0], input[1], output[0], output[1]})
            if (fd >= 0) close(fd);
        return std::unexpected("Couldn't create Git credential pipes.");
    }
    struct pipes_guard
    {
        int* a;
        int* b;
        ~pipes_guard()
        {
            for (int fd : {a[0], a[1], b[0], b[1]})
                if (fd >= 0) close(fd);
        }
    } pipes{input, output};
    std::map<std::string, std::string> env;
    for (char** p = environ; *p; ++p)
    {
        const std::string item(*p);
        const auto at = item.find('=');
        if (at != std::string::npos) env[item.substr(0, at)] = item.substr(at + 1);
    }
    for (const auto& [name, value] : r.environment)
        if (value.empty())
            env.erase(name);
        else
            env[name] = value;
    std::vector<std::string> storage;
    for (const auto& [name, value] : env) storage.push_back(name + '=' + value);
    std::vector<char*> environment;
    for (auto& item : storage) environment.push_back(item.data());
    environment.push_back(nullptr);
    std::vector<std::string> arguments{r.program};
    arguments.insert(arguments.end(), r.arguments.begin(), r.arguments.end());
    std::vector<char*> argv;
    for (auto& a : arguments) argv.push_back(a.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attributes);
    struct spawn_guard
    {
        posix_spawn_file_actions_t* a;
        posix_spawnattr_t* b;
        ~spawn_guard()
        {
            posix_spawn_file_actions_destroy(a);
            posix_spawnattr_destroy(b);
        }
    } spawn_cleanup{&actions, &attributes};
    posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    for (int fd : {input[0], input[1], output[0], output[1]})
        posix_spawn_file_actions_addclose(&actions, fd);
    // Git -C supplies the directory without mutating this process's cwd.
    if (posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP) ||
        posix_spawnattr_setpgroup(&attributes, 0))
        return std::unexpected("Couldn't isolate Git credential child process group.");
    pid_t pid;
    if (posix_spawnp(&pid, r.program.c_str(), &actions, &attributes, argv.data(),
                     environment.data()))
        return std::unexpected("Couldn't start Git credential lookup.");
    close(input[0]);
    input[0] = -1;
    close(output[1]);
    output[1] = -1;
    fcntl(input[1], F_SETFL, O_NONBLOCK);
    // Block SIGPIPE only on this thread while feeding the small request.
    sigset_t block, previous;
    sigemptyset(&block);
    sigaddset(&block, SIGPIPE);
    sigset_t pending_before;
    sigpending(&pending_before);
    pthread_sigmask(SIG_BLOCK, &block, &previous);
    const auto written = write(input[1], r.input.data(), r.input.size());
    if (written < 0 && errno == EPIPE && !sigismember(&pending_before, SIGPIPE))
    {
        sigset_t pending_after;
        sigpending(&pending_after);
        if (sigismember(&pending_after, SIGPIPE))
        {
            int signal;
            sigwait(&block, &signal);
        }
    }
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    close(input[1]);
    input[1] = -1;
    reply out;
    if (written != ssize_t(r.input.size()))
    {
        kill(-pid, SIGKILL);
        waitpid(pid, nullptr, 0);
        return std::unexpected("Couldn't write Git credential request.");
    }
    fcntl(output[0], F_SETFL, O_NONBLOCK);
    std::array<char, 4096> buffer;
    const auto until = std::chrono::steady_clock::now() + r.timeout;
    for (;;)
    {
        if (std::chrono::steady_clock::now() >= until)
        {
            out.timed_out = true;
            break;
        }
        const auto count = read(output[0], buffer.data(), buffer.size());
        if (count > 0)
        {
            if (out.output.size() + size_t(count) > r.output_limit)
            {
                out.exceeded_limit = true;
                break;
            }
            out.output.append(buffer.data(), size_t(count));
            continue;
        }
        if (!count) break;
        if (errno != EAGAIN && errno != EINTR) break;
        if (std::chrono::steady_clock::now() >= until)
        {
            out.timed_out = true;
            break;
        }
        pollfd ready{output[0], POLLIN, 0};
        poll(&ready, 1, 10);
    }
    if (out.timed_out || out.exceeded_limit) kill(-pid, SIGKILL);
    int status = 0;
    // A child may close stdout before exiting. Keep the same deadline.
    for (;;)
    {
        const auto waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) break;
        if (waited < 0 && errno == EINTR) continue;
        if (waited < 0)
        {
            out.exit_code = -1;
            return out;
        }
        if (std::chrono::steady_clock::now() >= until)
        {
            out.timed_out = true;
            kill(-pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    out.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return out;
}
#endif
}  // namespace

std::expected<reply, std::string> run(const request& r)
{
    if (r.input.size() > 2048 || r.timeout.count() <= 0 || r.timeout > std::chrono::seconds(30) ||
        r.output_limit > 65536)
        return std::unexpected("Invalid Git credential lookup bounds.");
    runner use;
    {
        std::lock_guard lock(runner_mutex);
        use = fake;
    }
    return use ? use(r) : native(r);
}
void set_runner(runner value)
{
    std::lock_guard lock(runner_mutex);
    fake = std::move(value);
}
}  // namespace tortilla::credential_helper
