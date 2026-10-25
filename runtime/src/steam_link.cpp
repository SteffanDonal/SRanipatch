#include "steam_link.hpp"

#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string_view>
#include <unordered_set>

namespace sranipatch
{
  namespace
  {
    std::string decode_ip(std::string_view hex)
    {
      if (hex.size() != 8 && hex.size() != 32)
        return {};
      std::array<unsigned char, 16> bytes {};
      for (std::size_t offset = 0; offset < hex.size(); offset += 8)
      {
        std::uint32_t word {};
        const auto [end, error] = std::from_chars(hex.data() + offset, hex.data() + offset + 8, word, 16);
        if (error != std::errc {} || end != hex.data() + offset + 8)
          return {};
        for (std::size_t byte = 0; byte < 4; ++byte)
          bytes[offset / 2 + byte] = static_cast<unsigned char>(word >> (byte * 8));
      }
      const bool mapped = hex.size() == 32 && std::all_of(bytes.begin(), bytes.begin() + 10, [](auto b) { return b == 0; }) && bytes[10] == 255 && bytes[11] == 255;
      const auto* ip = bytes.data() + (mapped ? 12 : 0);
      const bool v4 = hex.size() == 8 || mapped;
      if (v4)
      {
        if (ip[0] == 127 || std::all_of(ip, ip + 4, [](auto b) { return b == 0; }))
          return {};
      }
      else if (std::all_of(bytes.begin(), bytes.begin() + 15, [](auto b) { return b == 0; }) && bytes[15] <= 1)
      {
        return {};
      }
      std::array<char, INET6_ADDRSTRLEN> text {};
      return inet_ntop(v4 ? AF_INET : AF_INET6, ip, text.data(), text.size()) ? text.data() : "";
    }
  }  // namespace

  std::string steam_link_peer(const std::filesystem::path& proc)
  {
    std::unordered_set<std::string> inodes;
    std::error_code error;
    const auto options = std::filesystem::directory_options::skip_permission_denied;
    for (auto processes = std::filesystem::directory_iterator(proc, options, error);
      !error && processes != std::filesystem::directory_iterator {}; processes.increment(error))
    {
      std::error_code process_error;
      if (std::filesystem::read_symlink(processes->path() / "exe", process_error).filename() != "vrlink" || process_error)
        continue;
      for (auto fds = std::filesystem::directory_iterator(processes->path() / "fd", options, process_error);
        !process_error && fds != std::filesystem::directory_iterator {}; fds.increment(process_error))
      {
        std::error_code link_error;
        const auto link = std::filesystem::read_symlink(fds->path(), link_error).string();
        if (!link_error && link.starts_with("socket:[") && link.ends_with(']'))
          inodes.insert(link.substr(8, link.size() - 9));
      }
    }
    if (inodes.empty())
      return {};
    for (const auto* name : { "udp", "udp6" })
    {
      std::ifstream table(proc / "net" / name);
      std::string line;
      std::getline(table, line);
      while (std::getline(table, line))
      {
        std::istringstream row(line);
        std::array<std::string, 10> fields;
        bool valid = true;
        for (auto& field : fields)
        {
          if (!(row >> field))
          {
            valid = false;
            break;
          }
        }
        if (!valid || fields[3] != "01" || !inodes.contains(fields[9]))
          continue;
        const auto colon = fields[2].find(':');
        if (colon == std::string::npos)
          continue;
        auto ip = decode_ip(std::string_view(fields[2]).substr(0, colon));
        if (!ip.empty())
          return ip;
      }
    }
    return {};
  }
}  // namespace sranipatch
