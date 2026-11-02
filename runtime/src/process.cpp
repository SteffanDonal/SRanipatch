#include "sranipatch/process.hpp"

#include "sranipatch/diagnostics.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <signal.h>
#endif

#include <array>
#include <bit>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

#ifndef _WIN32
extern char** environ;
#endif

namespace sranipatch
{
  namespace
  {
    std::uintptr_t parse_token(std::string_view token)
    {
      std::uintptr_t value {};
      const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
      if (error != std::errc {} || end != token.data() + token.size() || value == 0)
        throw std::runtime_error("Invalid process lifetime handle");
      return value;
    }

#ifdef _WIN32
    std::wstring wide(std::string_view text)
    {
      if (text.empty())
        return {};
      const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
      if (!count)
        throw std::runtime_error("Invalid UTF-8 process argument");
      std::wstring result(static_cast<std::size_t>(count), L'\0');
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count);
      return result;
    }

    std::wstring quote(std::wstring_view argument)
    {
      std::wstring result { L"\"" };
      std::size_t slashes {};
      for (const auto character : argument)
      {
        if (character == L'\\')
        {
          ++slashes;
          continue;
        }
        result.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result += character;
        slashes = 0;
      }
      result.append(slashes * 2, L'\\');
      result += L'"';
      return result;
    }

    struct Handle
    {
      HANDLE value {};

      ~Handle() { reset(); }

      void reset() noexcept
      {
        if (value)
          CloseHandle(std::exchange(value, nullptr));
      }
    };
#else
    struct Descriptor
    {
      int value { -1 };

      ~Descriptor() { reset(); }

      void reset() noexcept
      {
        if (value >= 0)
          close(std::exchange(value, -1));
      }
    };
#endif
  }

  struct ChildProcess::State
  {
#ifdef _WIN32
    Handle process;
    Handle lifetime;
    Handle job;
#else
    pid_t process { -1 };
    Descriptor lifetime;
#endif
    bool running {};
    std::optional<int> exit_code;
  };

  struct ProcessLifetime::State
  {
#ifdef _WIN32
    Handle pipe;
#else
    Descriptor pipe;
#endif
  };

  ChildProcess::ChildProcess() = default;

  ChildProcess::~ChildProcess()
  {
    stop();
  }

  ChildProcess::ChildProcess(ChildProcess&&) noexcept = default;

  ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept
  {
    if (this != &other)
    {
      stop();
      state_ = std::move(other.state_);
    }
    return *this;
  }

  void ChildProcess::start(const std::filesystem::path& executable, const std::vector<std::string>& arguments,
    const std::filesystem::path& working_directory)
  {
    if (active())
      throw std::logic_error("Child process is already running");
    if (!executable.is_absolute())
      throw std::invalid_argument("Child executable must be an absolute path");
    for (const auto& argument : arguments)
    {
      if (argument.find('\0') != std::string::npos)
        throw std::invalid_argument("Process arguments cannot contain null characters");
    }
    auto next = std::make_unique<State>();
#ifdef _WIN32
    next->job.value = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!next->job.value || !SetInformationJobObject(next->job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
      throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Create child process job");
    Handle read;
    SECURITY_ATTRIBUTES security { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    if (!CreatePipe(&read.value, &next->lifetime.value, &security, 0)
      || !SetHandleInformation(next->lifetime.value, HANDLE_FLAG_INHERIT, 0))
      throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Create process lifetime pipe");
    std::wstring command = quote(executable.native());
    for (const auto& argument : arguments)
      command += L" " + quote(wide(argument));
    command += L" --lifetime " + std::to_wstring(reinterpret_cast<std::uintptr_t>(read.value));
    SIZE_T bytes {};
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<std::byte> storage(bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes))
      throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Initialize process attributes");

    struct AttributeCleanup
    {
      LPPROC_THREAD_ATTRIBUTE_LIST value;

      ~AttributeCleanup() { DeleteProcThreadAttributeList(value); }
    } cleanup { attributes };

    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, static_cast<void*>(&read.value), sizeof(read.value), nullptr, nullptr))
      throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Set inherited process handles");
    STARTUPINFOEXW startup {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process {};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
          EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
          working_directory.empty() ? nullptr : working_directory.c_str(), &startup.StartupInfo, &process))
      throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Launch child process");
    next->process.value = process.hProcess;
    log_event("child.created", "pid=", process.dwProcessId, " executable=", executable);
    Handle thread;
    thread.value = process.hThread;
    if (!AssignProcessToJobObject(next->job.value, next->process.value)
      || ResumeThread(thread.value) == static_cast<DWORD>(-1))
    {
      const auto error = GetLastError();
      TerminateProcess(next->process.value, 1);
      WaitForSingleObject(next->process.value, INFINITE);
      throw std::system_error(static_cast<int>(error), std::system_category(), "Assign child process job");
    }
#else
    int descriptors[2] {};
    if (pipe2(descriptors, O_CLOEXEC) != 0)
      throw std::system_error(errno, std::generic_category(), "Create process lifetime pipe");
    Descriptor read;
    read.value = descriptors[0];
    next->lifetime.value = descriptors[1];
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error)
      throw std::system_error(error, std::generic_category(), "Initialize process actions");

    struct ActionsCleanup
    {
      posix_spawn_file_actions_t* value;

      ~ActionsCleanup() { posix_spawn_file_actions_destroy(value); }
    } cleanup { &actions };

    const auto check = [](int result) {
      if (result)
        throw std::system_error(result, std::generic_category(), "Configure child process");
    };
    check(posix_spawn_file_actions_addclose(&actions, next->lifetime.value));
    check(posix_spawn_file_actions_adddup2(&actions, read.value, 3));
    if (read.value != 3)
      check(posix_spawn_file_actions_addclose(&actions, read.value));
    if (!working_directory.empty())
      check(posix_spawn_file_actions_addchdir_np(&actions, working_directory.c_str()));
    std::vector<std::string> values { executable.string() };
    values.insert(values.end(), arguments.begin(), arguments.end());
    values.emplace_back("--lifetime");
    values.emplace_back("3:" + std::to_string(getpid()));
    std::vector<char*> argv;
    argv.reserve(values.size() + 1);
    for (auto& value : values)
      argv.push_back(value.data());
    argv.push_back(nullptr);
    error = posix_spawn(&next->process, executable.c_str(), &actions, nullptr, argv.data(), environ);
    if (error)
      throw std::system_error(error, std::generic_category(), "Launch child process");
    log_event("child.created", "pid=", next->process, " executable=", executable);
#endif
    next->running = true;
    state_ = std::move(next);
  }

  std::optional<int> ChildProcess::poll() noexcept
  {
    if (!state_ || !state_->running)
      return state_ ? state_->exit_code : std::nullopt;
#ifdef _WIN32
    if (WaitForSingleObject(state_->process.value, 0) != WAIT_OBJECT_0)
      return std::nullopt;
    DWORD result {};
    if (!GetExitCodeProcess(state_->process.value, &result))
      result = 1;
    state_->exit_code = static_cast<int>(result);
    log_event("child.exited", "pid=", GetProcessId(state_->process.value), " code=", *state_->exit_code);
    state_->process.reset();
    state_->job.reset();
#else
    int result {};
    const auto waited = waitpid(state_->process, &result, WNOHANG);
    if (!waited || (waited < 0 && errno == EINTR))
      return std::nullopt;
    state_->exit_code = waited < 0 ? 1 : WIFEXITED(result) ? WEXITSTATUS(result)
                                                           : 128 + WTERMSIG(result);
    log_event("child.exited", "pid=", state_->process, " code=", *state_->exit_code);
    state_->process = -1;
#endif
    state_->running = false;
    state_->lifetime.reset();
    return state_->exit_code;
  }

  bool ChildProcess::active() const noexcept
  {
    return state_ && state_->running;
  }

  void ChildProcess::request_stop() noexcept
  {
    if (state_)
      state_->lifetime.reset();
  }

  void ChildProcess::stop(std::chrono::milliseconds timeout) noexcept
  {
    if (active())
      log_event("child.stop_requested", "timeout_ms=", timeout.count());
    request_stop();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (active() && !poll() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!active())
      return;
    log_event("child.force_terminate", "timeout_ms=", timeout.count());
#ifdef _WIN32
    TerminateJobObject(state_->job.value, 1);
    WaitForSingleObject(state_->process.value, INFINITE);
    state_->job.reset();
#else
    kill(state_->process, SIGKILL);
    int result {};
    while (waitpid(state_->process, &result, 0) < 0 && errno == EINTR)
    {
    }
    state_->process = -1;
#endif
    state_->running = false;
    state_->exit_code = 1;
  }

  ProcessLifetime::ProcessLifetime() = default;

  ProcessLifetime::ProcessLifetime(std::string_view token)
  {
#ifdef _WIN32
    const auto value = parse_token(token);
#else
    const auto separator = token.find(':');
    if (separator == std::string_view::npos)
      throw std::runtime_error("Process lifetime token has no parent identity");
    const auto value = parse_token(token.substr(0, separator));
    const auto parent = parse_token(token.substr(separator + 1));
    if (parent > static_cast<std::uintptr_t>(INT_MAX))
      throw std::runtime_error("Invalid process lifetime parent");
#endif
    auto next = std::make_unique<State>();
#ifdef _WIN32
    const auto handle = std::bit_cast<HANDLE>(value);
    if (GetFileType(handle) != FILE_TYPE_PIPE || !SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0))
      throw std::runtime_error("Invalid process lifetime pipe");
    next->pipe.value = handle;
#else
    if (value > static_cast<std::uintptr_t>(INT_MAX))
      throw std::runtime_error("Invalid process lifetime descriptor");
    const auto descriptor = static_cast<int>(value);
    struct stat info {};
    if (fstat(descriptor, &info) != 0 || !S_ISFIFO(info.st_mode) || fcntl(descriptor, F_SETFD, FD_CLOEXEC) != 0)
      throw std::runtime_error("Invalid process lifetime pipe");
    next->pipe.value = descriptor;
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0)
      throw std::system_error(errno, std::generic_category(), "Configure parent death signal");
    if (getppid() != static_cast<pid_t>(parent))
      raise(SIGKILL);
#endif
    state_ = std::move(next);
  }

  ProcessLifetime::~ProcessLifetime() = default;

  ProcessLifetime::ProcessLifetime(ProcessLifetime&&) noexcept = default;

  ProcessLifetime& ProcessLifetime::operator=(ProcessLifetime&&) noexcept = default;

  bool ProcessLifetime::stop_requested() const noexcept
  {
    if (!state_)
      return false;
#ifdef _WIN32
    DWORD available {};
    return !PeekNamedPipe(state_->pipe.value, nullptr, 0, nullptr, &available, nullptr);
#else
    pollfd descriptor { state_->pipe.value, POLLIN | POLLHUP, 0 };
    return ::poll(&descriptor, 1, 0) > 0 && (descriptor.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0;
#endif
  }

  std::filesystem::path executable_directory()
  {
#ifdef _WIN32
    std::array<wchar_t, 32768> path {};
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size())
      throw std::runtime_error("Cannot resolve executable path");
    return std::filesystem::path(path.data()).parent_path();
#else
    return std::filesystem::read_symlink("/proc/self/exe").parent_path();
#endif
  }

  void cleanup_old_packages() noexcept
  {
    try
    {
      const auto app = executable_directory().parent_path();
      const auto root = app.parent_path();
      if (app.filename() != "app" || !std::filesystem::is_regular_file(app / "package.json")
        || std::filesystem::exists(root / ".update-lock"))
        return;
      for (const auto& entry : std::filesystem::directory_iterator(root))
      {
        const auto name = entry.path().filename().string();
        if (name.starts_with("app.") && name.ends_with(".old") && !entry.is_symlink() && entry.is_directory())
        {
          std::error_code ignored;
          std::filesystem::remove_all(entry.path(), ignored);
        }
      }
    }
    catch (...)
    {
      // An inaccessible or loaded backup must never prevent tracking startup.
      return;
    }
  }
}
