#include "sranipatch/config.hpp"

#include <charconv>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace sranipatch
{
  namespace
  {
    std::string_view trim(std::string_view value)
    {
      constexpr std::string_view whitespace = " \t\r\n";
      const auto first = value.find_first_not_of(whitespace);
      if (first == std::string_view::npos)
        return {};
      return value.substr(first, value.find_last_not_of(whitespace) - first + 1);
    }

    template <std::size_t N>
    void checked_text(std::string_view key, std::array<char, N>& destination, std::string_view value)
    {
      if (value.empty() || value.size() >= N)
        throw std::runtime_error(std::string(key) + " is empty or too long");
      set_text(destination, value);
    }
  }  // namespace

  std::filesystem::path default_config_path()
  {
#ifdef _WIN32
    char* root {};
    std::size_t size {};
    if (_dupenv_s(&root, &size, "APPDATA") != 0 || !root || !*root)
    {
      std::free(root);
      return {};
    }
    const auto path = std::filesystem::path(root) / "sranipatch" / "config.ini";
    std::free(root);
    return path;
#else
    if (const char* root = std::getenv("XDG_CONFIG_HOME"); root && *root)
      return std::filesystem::path(root) / "sranipatch" / "config.ini";
    const char* home = std::getenv("HOME");
    if (!home || !*home)
      return {};
    return std::filesystem::path(home) / ".config" / "sranipatch" / "config.ini";
#endif
  }

  bool load_osc_config(const std::filesystem::path& path, Config& config)
  {
    if (path.empty() || !std::filesystem::exists(path))
      return false;
    std::ifstream input(path);
    if (!input)
      throw std::runtime_error("Cannot open configuration: " + path.string());
    Config parsed = config;
    std::string line;
    std::size_t line_number {};
    while (std::getline(input, line))
    {
      ++line_number;
      const auto text = trim(line);
      if (text.empty() || text.front() == '#' || text.front() == ';')
        continue;
      const auto separator = text.find('=');
      if (separator == std::string_view::npos)
        throw std::runtime_error("Invalid configuration line " + std::to_string(line_number));
      const auto key = trim(text.substr(0, separator));
      const auto value = trim(text.substr(separator + 1));
      if (key == "osc_host")
      {
        if (value.empty() || value == "\"\"" || value == "null")
          set_text(parsed.osc_host, "");
        else
          checked_text(key, parsed.osc_host, value);
      }
      else if (key == "osc_address")
      {
        if (value.empty() || value.front() != '/')
          throw std::runtime_error("osc_address must start with '/'");
        checked_text(key, parsed.osc_address, value);
      }
      else if (key == "osc_port")
      {
        std::uint32_t port {};
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), port);
        if (error != std::errc {} || end != value.data() + value.size() || port == 0 || port > 65535)
          throw std::runtime_error("osc_port must be between 1 and 65535");
        parsed.osc_port = port;
      }
      else
      {
        throw std::runtime_error("Unknown configuration key on line " + std::to_string(line_number) + ": " + std::string(key));
      }
    }
    if (!input.eof())
      throw std::runtime_error("Cannot read configuration: " + path.string());
    config = parsed;
    return true;
  }
}  // namespace sranipatch
