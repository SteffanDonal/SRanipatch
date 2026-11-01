#include "sranipatch/process.hpp"
#include "sranipatch/protocol.hpp"
#include "sranipatch/runtime_control.hpp"

#include <openvr_driver.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace
{
  using namespace std::chrono_literals;
  constexpr auto section = "driver_sranipatch";

  void require(bool condition, const char* message)
  {
    if (!condition)
      throw std::runtime_error(message);
  }

  template <typename Predicate>
  void wait(Predicate predicate, const char* message)
  {
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    while (!predicate())
    {
      require(std::chrono::steady_clock::now() < deadline, message);
      std::this_thread::sleep_for(20ms);
    }
  }

  class Settings : public vr::IVRSettings
  {
  public:
    const char* GetSettingsErrorNameFromEnum(vr::EVRSettingsError) override { return "None"; }

    void SetBool(const char*, const char* key, bool value, vr::EVRSettingsError* = nullptr) override
    {
      std::lock_guard lock(mutex_);
      booleans_[key] = value;
    }

    void SetInt32(const char*, const char* key, std::int32_t value, vr::EVRSettingsError* = nullptr) override
    {
      std::lock_guard lock(mutex_);
      integers_[key] = value;
    }

    void SetFloat(const char*, const char*, float, vr::EVRSettingsError* = nullptr) override {}

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): OpenVR fixes this override's argument order.
    void SetString(const char*, const char* key, const char* value, vr::EVRSettingsError* = nullptr) override
    {
      std::lock_guard lock(mutex_);
      strings_[key] = value;
    }

    bool GetBool(const char*, const char* key, vr::EVRSettingsError* = nullptr) override
    {
      std::lock_guard lock(mutex_);
      return booleans_[key];
    }

    std::int32_t GetInt32(const char*, const char* key, vr::EVRSettingsError* = nullptr) override
    {
      std::lock_guard lock(mutex_);
      return integers_[key];
    }

    float GetFloat(const char*, const char*, vr::EVRSettingsError* = nullptr) override { return 0; }

    void GetString(const char*, const char* key, char* value, std::uint32_t size, vr::EVRSettingsError* = nullptr) override
    {
      const auto text = string(key);
      if (size)
      {
        const auto length = std::min<std::size_t>(text.size(), size - 1);
        std::memcpy(value, text.data(), length);
        value[length] = '\0';
      }
    }

    void RemoveSection(const char*, vr::EVRSettingsError* = nullptr) override {}

    void RemoveKeyInSection(const char*, const char*, vr::EVRSettingsError* = nullptr) override {}

    std::string string(const char* key)
    {
      std::lock_guard lock(mutex_);
      return strings_[key];
    }

  private:
    std::mutex mutex_;
    std::map<std::string, bool> booleans_;
    std::map<std::string, std::int32_t> integers_;
    std::map<std::string, std::string> strings_;
  };

  class DriverLog : public vr::IVRDriverLog
  {
  public:
    void Log(const char*) override {}
  };

  class Context : public vr::IVRDriverContext
  {
  public:
    void* GetGenericInterface(const char* name, vr::EVRInitError* error) override
    {
      if (error)
        *error = vr::VRInitError_None;
      if (std::strcmp(name, vr::IVRSettings_Version) == 0)
        return &settings;
      if (std::strcmp(name, vr::IVRDriverLog_Version) == 0)
        return &log;
      // InitServer checks these interfaces; this driver never calls their methods.
      if (std::strcmp(name, vr::IVRServerDriverHost_Version) == 0
        || std::strcmp(name, vr::IVRProperties_Version) == 0
        || std::strcmp(name, vr::IVRDriverManager_Version) == 0
        || std::strcmp(name, vr::IVRResources_Version) == 0)
        return &unused_;
      if (error)
        *error = vr::VRInitError_Init_InterfaceNotFound;
      return nullptr;
    }

    vr::DriverHandle_t GetDriverHandle() override { return 1; }

    Settings settings;
    DriverLog log;

  private:
    int unused_ {};
  };

  class Module
  {
  public:
    explicit Module(const std::filesystem::path& file)
    {
#ifdef _WIN32
      handle_ = LoadLibraryW(file.c_str());
      require(handle_ != nullptr, "Cannot load staged driver");
      const auto address = GetProcAddress(handle_, "HmdDriverFactory");
#else
      handle_ = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
      require(handle_ != nullptr, "Cannot load staged driver");
      const auto address = dlsym(handle_, "HmdDriverFactory");
#endif
      require(address != nullptr, "Driver factory missing");
      using Factory = void* (*)(const char*, int*);
      const auto factory = reinterpret_cast<Factory>(address);
      int error {};
      provider = static_cast<vr::IServerTrackedDeviceProvider*>(factory(vr::IServerTrackedDeviceProvider_Version, &error));
      require(provider != nullptr, "Driver provider missing");
      require(factory("unrecognized", &error) == nullptr && error == vr::VRInitError_Init_InterfaceNotFound,
        "Factory must reject unknown interfaces");
    }

    ~Module()
    {
      stop();
#ifdef _WIN32
      FreeLibrary(handle_);
#else
      dlclose(handle_);
#endif
    }

    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    void start(Context& context)
    {
      require(provider->Init(&context) == vr::VRInitError_None, "Driver init failed");
      started_ = true;
    }

    void stop()
    {
      if (started_)
      {
        provider->Cleanup();
        started_ = false;
      }
    }

    vr::IServerTrackedDeviceProvider* provider {};

  private:
#ifdef _WIN32
    HMODULE handle_ {};
#else
    void* handle_ {};
#endif
    bool started_ {};
  };

  int server(int argc, char** argv)
  {
    std::string ipc, lifetime;
    for (int i = 1; i + 1 < argc; i += 2)
    {
      if (std::string_view(argv[i]) == "--ipc")
        ipc = argv[i + 1];
      else if (std::string_view(argv[i]) == "--lifetime")
        lifetime = argv[i + 1];
    }
    std::ofstream("starts", std::ios::app) << "start\n";
    std::string mode;
    std::ifstream("mode") >> mode;
    if (mode == "exit")
      return 7;
    sranipatch::ProcessLifetime parent(lifetime);
    sranipatch::ControlChannel channel(ipc);
    sranipatch::ServerControl control;
    control.session_id = sranipatch::monotonic_ns();
    control.managed = true;
    while (!parent.stop_requested())
    {
      control.heartbeat_ns = sranipatch::monotonic_ns();
      const auto driver = channel.read().driver;
      control.overlay_revision = driver.overlay_revision;
      control.overlay = driver.show_overlay ? sranipatch::OverlayState::running : sranipatch::OverlayState::stopped;
      std::string overlay_mode;
      std::ifstream("overlay") >> overlay_mode;
      control.message.fill('\0');
      if (driver.show_overlay && overlay_mode == "failed")
      {
        control.overlay = sranipatch::OverlayState::failed;
        sranipatch::set_text(control.message, "Dashboard failed twice");
      }
      channel.publish_server(control);
      std::this_thread::sleep_for(20ms);
    }
    control.heartbeat_ns = 0;
    channel.publish_server(control);
    std::ofstream("stops", std::ios::app) << "stop\n";
    return 0;
  }

  std::size_t lines(const std::filesystem::path& file)
  {
    std::ifstream input(file);
    std::size_t count {};
    for (std::string line; std::getline(input, line);)
      ++count;
    return count;
  }

  void test(const std::filesystem::path& source)
  {
    const auto stamp = std::to_string(sranipatch::monotonic_ns());
    const auto root = std::filesystem::temp_directory_path() / ("sranipatch-driver-" + stamp);
    const auto binaries = root / "bin";
    const auto module_directory = root / "driver" / "bin" / "test";
    std::filesystem::create_directories(binaries);
    std::filesystem::create_directories(module_directory);
    const auto staged = module_directory / source.filename();
    std::filesystem::copy_file(source, staged);
#ifdef _WIN32
    const auto server_file = binaries / "sranipatch-server.exe";
    const auto self = sranipatch::executable_directory() / "sranipatch-driver-tests.exe";
#else
    const auto server_file = binaries / "sranipatch-server";
    const auto self = std::filesystem::read_symlink("/proc/self/exe");
#endif
    std::filesystem::copy_file(self, server_file);
    const std::string ipc = "driver-test-" + stamp;
    sranipatch::ControlChannel channel(ipc);
    Context context;
    context.settings.SetString(section, "ipc", ipc.c_str());
    context.settings.SetBool(section, "runServer", true);
    {
      Module module(staged);
      std::ofstream(binaries / "mode") << "exit";
      module.start(context);
      wait([&] { return context.settings.string("status").find("stopped after repeated failures") != std::string::npos; },
        "Repeated server failures were not reported in SteamVR settings");
      require(lines(binaries / "starts") == 2, "Driver must stop after exactly two failures");
      require(context.settings.GetInt32(section, "statusState") == 6, "Exhausted recovery must publish the server failure display state");
      std::this_thread::sleep_for(1200ms);
      require(lines(binaries / "starts") == 2, "Exhausted recovery must remain stopped");

      std::ofstream(binaries / "mode") << "live";
      context.settings.SetBool(section, "retryServer", true);
      wait([&] { return context.settings.string("status") == "Tracking server running"; },
        "Explicit retry did not start the replacement server");
      require(!context.settings.GetBool(section, "retryServer"), "Retry action was not reset");
      require(context.settings.GetInt32(section, "statusState") == 2, "Running server must publish the running display state");
      context.settings.SetInt32(section, "statusState", 0);
      wait([&] { return context.settings.GetInt32(section, "statusState") == 2; },
        "Resetting settings must not leave the read-only status stale");
      context.settings.SetBool(section, "showOverlay", true);
      wait([&] { return channel.read().server.overlay == sranipatch::OverlayState::running; },
        "SteamVR overlay preference was not forwarded");
      std::ofstream(binaries / "overlay") << "failed";
      wait([&] { return !context.settings.GetBool(section, "showOverlay")
                   && context.settings.string("status").find("Overlay stopped:") != std::string::npos; },
        "Client failure must disable the overlay and report its error");
      std::this_thread::sleep_for(300ms);
      require(context.settings.string("status").find("Overlay stopped:") != std::string::npos,
        "Client error must remain visible after the overlay is disabled");
      std::ofstream(binaries / "overlay") << "live";
      context.settings.SetBool(section, "showOverlay", true);
      wait([&] { return channel.read().server.overlay == sranipatch::OverlayState::running
                   && context.settings.string("status") == "Tracking server running"; },
        "Client retry must clear the error and tolerate stale failure feedback");
      const auto previous_session = channel.read().server.session_id;
      context.settings.SetBool(section, "restartServer", true);
      wait([&] { const auto state = channel.read(); return state.server.session_id != previous_session
          && state.server.overlay == sranipatch::OverlayState::running; },
        "Restart did not restore overlay in the replacement server");
      context.settings.SetBool(section, "runServer", false);
      wait([&] { return context.settings.string("status") == "Tracking stopped"; },
        "Disabling tracking did not stop the owned server");
      const auto paused_starts = lines(binaries / "starts");
      std::this_thread::sleep_for(1200ms);
      require(lines(binaries / "starts") == paused_starts, "Paused server must not restart automatically");
      require(context.settings.GetBool(section, "showOverlay"), "Stopping tracking must preserve overlay preference");
      context.settings.SetBool(section, "runServer", true);
      wait([&] { return lines(binaries / "starts") > paused_starts
                   && channel.read().server.overlay == sranipatch::OverlayState::running; },
        "Enabling tracking did not restore the server and overlay");
      module.stop();
      require(lines(binaries / "stops") == 3, "Restart, pause and SteamVR shutdown must stop owned servers gracefully");
      require(channel.read().driver.heartbeat_ns == 0, "Driver must clear its heartbeat on shutdown");
      require(context.settings.GetBool(section, "showOverlay"), "Shutdown must preserve overlay preference");

      sranipatch::ServerControl manual;
      manual.session_id = sranipatch::monotonic_ns();
      std::jthread publisher([&](const std::stop_token& stop) {
        while (!stop.stop_requested())
        {
          manual.heartbeat_ns = sranipatch::monotonic_ns();
          channel.publish_server(manual);
          std::this_thread::sleep_for(20ms);
        }
      });
      module.start(context);
      wait([&] { return context.settings.string("status") == "Connected to manually started server"; },
        "Driver did not attach to manually started server");
      const auto starts = lines(binaries / "starts");
      context.settings.SetBool(section, "restartServer", true);
      std::this_thread::sleep_for(300ms);
      require(lines(binaries / "starts") == starts, "Manual server must never be replaced");
      module.stop();
      require(channel.read().server.session_id == manual.session_id, "SteamVR shutdown must leave manual server untouched");
    }
    std::filesystem::remove_all(root);
  }
}

// Exceptions from the body are caught below; clang-tidy misidentifies MSVC exception inheritance.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char** argv)
{
  try
  {
    if (argc > 1 && std::string_view(argv[1]) == "--ipc")
      return server(argc, argv);
    require(argc == 2, "Expected driver module path");
    test(std::filesystem::absolute(argv[1]));
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
