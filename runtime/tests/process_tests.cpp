#include "sranipatch/process.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace
{
  using namespace std::chrono_literals;

  void require(bool condition, const char* message)
  {
    if (!condition)
      throw std::runtime_error(message);
  }

  unsigned long process_id()
  {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long>(getpid());
#endif
  }

  unsigned long read_pid(const std::filesystem::path& path)
  {
    std::ifstream input(path);
    unsigned long result {};
    input >> result;
    return result;
  }

  bool running(unsigned long pid)
  {
#ifdef _WIN32
    const auto process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!process)
      return false;
    const auto result = WaitForSingleObject(process, 0);
    CloseHandle(process);
    return result == WAIT_TIMEOUT;
#else
    std::ifstream input("/proc/" + std::to_string(pid) + "/stat");
    std::string status;
    std::getline(input, status);
    const auto name_end = status.rfind(')');
    return name_end != std::string::npos && name_end + 2 < status.size() && status[name_end + 2] != 'Z';
#endif
  }

  int child(int argc, char** argv)
  {
    sranipatch::ProcessLifetime lifetime;
    for (int i = 2; i < argc; ++i)
    {
      if (std::string_view(argv[i]) == "--lifetime" && i + 1 < argc)
        lifetime = sranipatch::ProcessLifetime(argv[++i]);
    }
    const std::string_view mode(argv[1]);
    if (mode == "--blocked-marker")
    {
      {
        std::ofstream marker(argv[2]);
        marker << process_id();
      }
      std::this_thread::sleep_for(10s);
      return 0;
    }
    if (mode == "--abrupt-parent")
    {
      sranipatch::ChildProcess descendant;
      descendant.start(std::filesystem::absolute(argv[0]), { "--blocked-marker", argv[2] });
      const auto deadline = std::chrono::steady_clock::now() + 5s;
      while (!read_pid(argv[2]) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
      std::_Exit(41);
    }
    if (mode == "--arguments")
    {
      require(argc == 9, "Arguments were split or omitted");
      require(std::string_view(argv[2]).empty(), "Empty argument lost");
      require(std::string_view(argv[3]) == "two words", "Spaces not preserved");
      require(std::string_view(argv[4]) == "a\"b", "Quote not preserved");
      require(std::string_view(argv[5]) == "trailing slash \\", "Trailing slash not preserved");
      require(std::string_view(argv[6]) == "& | $ ( )", "Shell punctuation not preserved");
      return 17;
    }
    if (mode == "--wait")
    {
      const auto deadline = std::chrono::steady_clock::now() + 5s;
      while (!lifetime.stop_requested() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
      return lifetime.stop_requested() ? 23 : 24;
    }
    if (mode == "--ignore-stop")
      std::this_thread::sleep_for(10s);
    return 0;
  }

  int wait(sranipatch::ChildProcess& process)
  {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline)
    {
      if (const auto code = process.poll())
        return *code;
      std::this_thread::sleep_for(5ms);
    }
    throw std::runtime_error("Child did not exit");
  }

  void run(const std::filesystem::path& executable)
  {
    require(!sranipatch::ProcessLifetime().stop_requested(), "Standalone process was stopped");
    sranipatch::ChildProcess process;
    require(!process.active() && !process.poll(), "Empty process appears running");
    process.start(executable, { "--arguments", "", "two words", "a\"b", "trailing slash \\", "& | $ ( )" });
    require(wait(process) == 17, "Argument round-trip failed");
    require(process.poll() == 17, "Exit code was not retained");
    process.start(executable, { "--wait" });
    require(process.active(), "Started process is not owned");
    process.request_stop();
    require(wait(process) == 23, "Graceful lifetime stop failed");
    process.start(executable, { "--ignore-stop" });
    const auto begin = std::chrono::steady_clock::now();
    process.stop(50ms);
    require(!process.active(), "Forced termination left running process");
    require(std::chrono::steady_clock::now() - begin < 2s, "Forced termination exceeded deadline");
    process.start(executable, { "--wait" });
    sranipatch::ChildProcess moved(std::move(process));
    require(moved.active(), "Move lost process ownership");
    process = sranipatch::ChildProcess();
    moved.request_stop();
    require(wait(moved) == 23, "Moved lifetime stop failed");
    bool rejected {};
    try
    {
      process.start(executable.parent_path() / "sranipatch-no-such-executable");
    }
    catch (const std::exception&)
    {
      rejected = true;
    }
    require(rejected && !process.active(), "Missing executable was accepted");

    struct Marker
    {
      std::filesystem::path path { std::filesystem::temp_directory_path() / ("sranipatch-process-" + std::to_string(process_id())) };

      ~Marker()
      {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
      }
    } marker;

    process.start(executable, { "--abrupt-parent", marker.path.string() });
    unsigned long descendant {};
    const auto ready_deadline = std::chrono::steady_clock::now() + 5s;
    while (!descendant && std::chrono::steady_clock::now() < ready_deadline)
    {
      descendant = read_pid(marker.path);
      std::this_thread::sleep_for(5ms);
    }
    require(descendant != 0, "Descendant did not become ready");
    const auto death_deadline = std::chrono::steady_clock::now() + 2s;
    // Do not reap the parent yet: its own teardown must terminate the blocked descendant.
    while (running(descendant) && std::chrono::steady_clock::now() < death_deadline)
      std::this_thread::sleep_for(5ms);
    const bool descendant_stopped = !running(descendant);
    require(wait(process) == 41, "Abrupt parent did not exit as expected");
    require(descendant_stopped, "Blocked descendant survived abrupt parent exit");
  }
}

// Exceptions from the body are caught below; clang-tidy misidentifies MSVC exception inheritance.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char** argv)
{
  try
  {
    if (argc > 1)
      return child(argc, argv);
    run(std::filesystem::absolute(argv[0]));
    std::cout << "Process lifecycle tests passed\n";
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
