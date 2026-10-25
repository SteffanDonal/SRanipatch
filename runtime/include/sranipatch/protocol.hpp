#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace sranipatch
{
  inline constexpr std::uint32_t protocol_magic = 0x53525031;
  inline constexpr std::uint32_t protocol_version = 9;
  inline constexpr std::size_t max_parameters = 128;
  inline constexpr std::uint32_t capture_width = 400;
  inline constexpr std::uint32_t capture_height = 400;
  inline constexpr std::uint32_t capture_fps = 60;

  inline std::uint64_t monotonic_ns()
  {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()
    )
        .count());
  }

  template <std::size_t N>
  void set_text(std::array<char, N>& dest, std::string_view text)
  {
    dest.fill('\0');
    text.copy(dest.data(), text.size() < N - 1 ? text.size() : N - 1);
  }

  // NOLINTNEXTLINE(performance-enum-size): Fixed-width shared-memory protocol field.
  enum class ServerState : std::uint32_t
  {
    starting,
    waiting_for_camera,
    running,
    stopped,
    fault,
    waiting_for_steam_link
  };

  struct Parameter
  {
    std::array<char, 48> name {};
    float value {};
  };

  struct Config
  {
    std::array<char, 256> model {};
    std::array<char, 64> osc_host {};
    std::array<char, 128> osc_address {};
    std::uint32_t threads { 2 };
    std::uint32_t osc_port { 9337 };
    float mean { 0.0F };
    float scale { 1.0F / 255.0F };
  };

  struct OscDestination
  {
    std::array<char, 64> host {};
    std::uint32_t port {};
    std::uint32_t ready {};
  };

  struct Status
  {
    std::uint64_t heartbeat_ns {};
    std::uint64_t frames_captured {};
    std::uint64_t frames_inferred {};
    std::uint64_t frames_skipped {};
    std::uint64_t osc_failures {};
    std::uint64_t session_id {};
    ServerState state { ServerState::starting };
    std::uint32_t parameter_count {};
    float capture_fps {};
    float inference_fps {};
    float inference_ms {};
    float frame_age_ms {};
    Config config {};
    std::array<Parameter, max_parameters> parameters {};
    std::array<char, 256> error {};
    OscDestination osc_destination {};
  };

  struct Preview
  {
    std::uint64_t sequence {};
    std::uint64_t timestamp_ns {};
    std::array<std::uint8_t, std::size_t { capture_width } * capture_height> luma {};
  };

  struct SharedBlock
  {
    std::uint32_t magic { protocol_magic };
    std::uint32_t version { protocol_version };
    std::uint32_t size { sizeof(SharedBlock) };
    std::uint32_t reserved {};
    Status status {};
    Preview preview {};
  };

  static_assert(std::is_trivially_copyable_v<SharedBlock>);
  static_assert(std::is_standard_layout_v<SharedBlock>);
  static_assert(sizeof(Parameter) == 52);
  static_assert(sizeof(Config) == 464);
  static_assert(sizeof(OscDestination) == 72);
  static_assert(sizeof(Status) == 7520);
  static_assert(sizeof(SharedBlock) == 167552);
  static_assert(offsetof(SharedBlock, status) == 16);
  static_assert(offsetof(SharedBlock, preview) == 7536);
}  // namespace sranipatch
