#include "sranipatch/live_osc_config.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{
  using namespace std::chrono_literals;

  void require(bool condition, const char* message)
  {
    if (!condition)
      throw std::runtime_error(message);
  }

  struct ConfigFile
  {
    std::filesystem::path path { std::filesystem::temp_directory_path() / ("sranipatch-reload-" + std::to_string(sranipatch::monotonic_ns())) };

    ~ConfigFile()
    {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }

    void write(std::string_view contents) const
    {
      std::ofstream output(path);
      output << contents;
      if (!output)
        throw std::runtime_error("Cannot write test config");
    }
  };

  void output_mode_reloads_and_resets()
  {
    ConfigFile file;
    file.write("osc_host=127.0.0.1\n");
    sranipatch::LiveOscConfig live(file.path, {}, {});
    require(live.config().output_vrc_native == 0, "Native output must default to false");
    auto now = std::chrono::steady_clock::now();
    for (const auto* setting : { "true", "false", "true", "" })
    {
      const auto text = std::string("osc_host=127.0.0.1\n")
        + (*setting ? std::string("output_vrc_native=") + setting + "\n" : "");
      auto* previous = &live.sender();
      file.write(text);
      require(!live.poll(now), "Mode change must be debounced");
      require(live.poll(now + 300ms), "Mode change was not applied");
      require((live.config().output_vrc_native != 0) == (std::string_view(setting) == "true"), "Wrong output mode after reload");
      require(&live.sender() != previous, "Mode-only change did not replace sender");
      now += 1s;
    }
  }

  void saved_config_replaces_destination_after_debounce()
  {
    ConfigFile file;
    file.write("osc_host=127.0.0.1\nosc_port=12345\n");
    sranipatch::Config defaults;
    sranipatch::LiveOscConfig live(file.path, defaults, {});
    require(live.config().osc_port == 12345, "Startup file was not loaded");
    auto now = std::chrono::steady_clock::now();
    file.write("osc_host=127.0.0.2\nosc_port=23456\n");
    require(!live.poll(now), "Save should be debounced");
    require(!live.poll(now + 100ms), "Save applied before debounce finished");
    require(live.poll(now + 300ms), "Saved config was not applied promptly");
    require(live.config().osc_port == 23456 && std::string_view(live.sender().destination().host.data()) == "127.0.0.2" && live.sender().destination().port == 23456 && live.sender().destination().ready,
      "Config and sender were not replaced together");
  }

  void failed_reload_keeps_working_output_and_recovers()
  {
    ConfigFile file;
    file.write("osc_host=127.0.0.1\nosc_port=12345\n");
    sranipatch::Config defaults;
    sranipatch::LiveOscConfig live(file.path, defaults, {});
    auto now = std::chrono::steady_clock::now();
    for (const auto* invalid : { "osc_port=0\n", "osc_host=not-an-ip\n", "unknown=value\n", "output_vrc_native=yes\n", "output_vrc_native=\n" })
    {
      file.write(invalid);
      live.poll(now);
      bool rejected {};
      try
      {
        live.poll(now + 300ms);
      }
      catch (const std::exception&)
      {
        rejected = true;
      }
      require(rejected, "Invalid config was not reported");
      require(live.config().osc_port == 12345 && live.sender().destination().ready
          && std::string_view(live.sender().destination().host.data()) == "127.0.0.1",
        "Invalid config disturbed working output");
      require(!live.poll(now + 600ms), "Unchanged invalid config was retried");
      now += 1s;
    }
    std::filesystem::remove(file.path);
    live.poll(now);
    bool reported_missing {};
    try
    {
      live.poll(now + 300ms);
    }
    catch (const std::exception&)
    {
      reported_missing = true;
    }
    require(reported_missing && live.sender().destination().ready
        && std::string_view(live.sender().destination().host.data()) == "127.0.0.1",
      "Missing config must report an error and keep working output");
    now += 1s;
    file.write("osc_host=127.0.0.2\nosc_port=23456\n");
    live.poll(now);
    require(live.poll(now + 300ms) && live.config().osc_port == 23456, "Recreated config was not applied");
  }

  void removed_settings_restore_defaults_and_cli_overrides_win()
  {
    ConfigFile file;
    file.write("osc_host=127.0.0.1\nosc_port=12345\n");
    sranipatch::Config defaults;
    sranipatch::LiveOscConfig live(file.path, defaults, {});
    auto now = std::chrono::steady_clock::now();
    file.write("");
    live.poll(now);
    require(live.poll(now + 300ms) && live.config().osc_host[0] == '\0' && live.config().osc_port == 9337,
      "Removed settings did not restore defaults");
    sranipatch::set_text(defaults.osc_host, "127.0.0.3");
    defaults.osc_port = 34567;
    sranipatch::LiveOscConfig overridden(file.path, defaults, { true, true });
    file.write("osc_host=127.0.0.2\nosc_port=23456\n");
    overridden.poll(now);
    overridden.poll(now + 300ms);
    require(overridden.config().osc_port == 34567 && std::string_view(overridden.config().osc_host.data()) == "127.0.0.3"
        && overridden.sender().destination().port == 34567
        && std::string_view(overridden.sender().destination().host.data()) == "127.0.0.3",
      "File changes overrode CLI options or restarted unchanged output");
  }

  void unreadable_file_is_rejected()
  {
    ConfigFile file;
    file.write("osc_host=127.0.0.1\n");
    sranipatch::Config defaults;
    sranipatch::LiveOscConfig live(file.path, defaults, {});
    std::filesystem::remove(file.path);
    std::filesystem::create_directory(file.path);
    const auto now = std::chrono::steady_clock::now();
    live.poll(now);
    bool rejected {};
    try
    {
      live.poll(now + 300ms);
    }
    catch (const std::exception&)
    {
      rejected = true;
    }
    require(rejected && live.sender().destination().ready
        && std::string_view(live.sender().destination().host.data()) == "127.0.0.1",
      "Unreadable file replaced working output");
  }

  void editor_replacement_and_quick_saves_are_debounced()
  {
    ConfigFile file;
    file.write("osc_host=127.0.0.1\n");
    sranipatch::Config defaults;
    sranipatch::LiveOscConfig live(file.path, defaults, {});
    const auto stamp = std::filesystem::last_write_time(file.path);
    ConfigFile replacement;
    replacement.write("osc_host=127.0.0.2\n");
    std::filesystem::remove(file.path);
    std::filesystem::rename(replacement.path, file.path);
    std::filesystem::last_write_time(file.path, stamp);
    const auto now = std::chrono::steady_clock::now();
    live.poll(now);
    require(live.poll(now + 300ms) && std::string_view(live.sender().destination().host.data()) == "127.0.0.2",
      "Replacement with unchanged size and timestamp was missed");
    file.write("osc_host=127.0.0.3\n");
    live.poll(now + 1s);
    file.write("osc_host=127.0.0.4\n");
    require(!live.poll(now + 1100ms) && !live.poll(now + 1200ms), "A quick second save was not debounced");
    require(live.poll(now + 1400ms) && std::string_view(live.sender().destination().host.data()) == "127.0.0.4",
      "Latest save was missed");
  }

  void file_created_after_startup_respects_individual_cli_overrides()
  {
    ConfigFile file;
    sranipatch::Config defaults;
    sranipatch::set_text(defaults.osc_host, "127.0.0.1");
    sranipatch::set_text(defaults.model, "custom.mnn");
    defaults.threads = 3;
    defaults.osc_port = 34567;
    sranipatch::LiveOscConfig live(file.path, defaults, { false, true });
    file.write("osc_host=127.0.0.2\nosc_port=12345\n");
    const auto now = std::chrono::steady_clock::now();
    live.poll(now);
    require(live.poll(now + 300ms) && live.config().osc_port == 34567 && std::string_view(live.sender().destination().host.data()) == "127.0.0.2" && live.config().threads == 3 && std::string_view(live.config().model.data()) == "custom.mnn",
      "Created config lost individual overrides or changed inference settings");
  }
}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape): Exceptions from the body are caught below.
int main()
{
  try
  {
    output_mode_reloads_and_resets();
    saved_config_replaces_destination_after_debounce();
    failed_reload_keeps_working_output_and_recovers();
    removed_settings_restore_defaults_and_cli_overrides_win();
    unreadable_file_is_rejected();
    editor_replacement_and_quick_saves_are_debounced();
    file_created_after_startup_respects_individual_cli_overrides();
    std::cout << "Config reload checks passed\n";
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
