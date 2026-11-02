#include "sranipatch/process.hpp"
#include "sranipatch/protocol.hpp"
#include "sranipatch/runtime_control.hpp"

#include <openvr.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace
{
  constexpr auto section = "driver_sranipatch";

  bool steamvr_running()
  {
#ifdef _WIN32
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
      throw std::runtime_error("Cannot enumerate SteamVR processes");
    PROCESSENTRY32W entry {};
    entry.dwSize = sizeof(entry);
    bool found {};
    if (Process32FirstW(snapshot, &entry))
    {
      do
      {
        found = _wcsicmp(entry.szExeFile, L"vrserver.exe") == 0;
      }
      while (!found && Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
#else
    for (const auto& entry : std::filesystem::directory_iterator("/proc"))
    {
      std::ifstream file(entry.path() / "comm");
      std::string name;
      if (std::getline(file, name) && name == "vrserver")
        return true;
    }
    return false;
#endif
  }

  bool fresh(std::uint64_t heartbeat)
  {
    const auto now = sranipatch::monotonic_ns();
    return heartbeat && heartbeat <= now && now - heartbeat < 3'000'000'000ULL;
  }

  struct Shutdown
  {
    ~Shutdown() { vr::VR_Shutdown(); }
  };
}

int main(int argc, char** argv)
{
  try
  {
    if (argc < 2)
      throw std::runtime_error("Usage: sranipatch-package-control status|pause|resume [0|1]");
    const std::string command = argv[1];
    if (command != "status" && command != "pause" && command != "resume")
      throw std::runtime_error("Unknown package control command");
    if (!steamvr_running())
    {
      if (command == "status")
      {
        std::cout << "{\"steamvr\":false,\"active\":false,\"run\":false,\"managed\":false,\"revision\":0}\n";
        return 0;
      }
      throw std::runtime_error("SteamVR is no longer running");
    }
    vr::EVRInitError error {};
    vr::VR_Init(&error, vr::VRApplication_Background);
    if (error != vr::VRInitError_None)
      throw std::runtime_error(vr::VR_GetVRInitErrorAsEnglishDescription(error));
    const Shutdown shutdown;
    auto* settings = vr::VRSettings();
    std::array<char, 128> name {};
    settings->GetString(section, "ipc", name.data(), static_cast<std::uint32_t>(name.size()));
    sranipatch::ControlChannel channel(name[0] ? name.data() : "sranipatch");
    const auto state = channel.read();
    std::array<char, 32768> runtime_directory {};
    settings->GetString(section, "runtimeDirectory", runtime_directory.data(), static_cast<std::uint32_t>(runtime_directory.size()));
    const std::string encoded_directory(runtime_directory.data());
    const auto runtime_path = std::filesystem::path(std::u8string(encoded_directory.begin(), encoded_directory.end()));
    const bool active = fresh(state.driver.heartbeat_ns)
      && runtime_path.lexically_normal() == sranipatch::executable_directory().lexically_normal();
    if (command == "status")
    {
      std::cout << std::boolalpha << "{\"steamvr\":true,\"active\":" << active
                << ",\"run\":" << settings->GetBool(section, "runServer")
                << ",\"managed\":" << (fresh(state.server.heartbeat_ns) && state.server.managed)
                << ",\"revision\":" << (active ? settings->GetInt32(section, "loadedRevision") : 0) << "}\n";
      return 0;
    }
    const bool run = command == "resume" && argc == 3 && std::string(argv[2]) == "1";
    if (!active)
      throw std::runtime_error("This package's driver is no longer active");
    vr::EVRSettingsError setting_error {};
    settings->SetBool(section, "runServer", run, &setting_error);
    if (setting_error != vr::VRSettingsError_None)
      throw std::runtime_error(settings->GetSettingsErrorNameFromEnum(setting_error));
    if (command == "pause" && active && state.server.managed)
    {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
      while (fresh(channel.read().server.heartbeat_ns))
      {
        if (std::chrono::steady_clock::now() >= deadline)
          throw std::runtime_error("Timed out stopping the managed server");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    }
    return 0;
  }
  catch (const std::exception& error)
  {
    std::fprintf(stderr, "SRanipatch: %s\n", error.what());
    return 1;
  }
  catch (...)
  {
    std::fputs("SRanipatch: Unexpected runtime control failure\n", stderr);
    return 1;
  }
}
