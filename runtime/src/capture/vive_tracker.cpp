// Protocol adapted from LordOfDragons/vivefacialtracker (MIT).
// See third_party/vivefacialtracker-LICENSE.txt.
#include "sof/vive_tracker.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

namespace sof
{
  namespace
  {
    class Commands
    {
      TrackerExtension& extension_;
      std::vector<std::uint8_t> send_, receive_;

    public:
      explicit Commands(TrackerExtension& extension) : extension_(extension)
      {
        const auto size = extension.buffer_size();
        if (size != 384 && size != 64)
          throw std::runtime_error("VIVE extension selector 2 must have a 384- or 64-byte payload");
        send_.resize(size);
        receive_.resize(size);
      }

      void send(std::span<const std::uint8_t> command, bool response = true)
      {
        std::fill(send_.begin(), send_.end(), std::uint8_t { 0 });
        std::copy(command.begin(), command.end(), send_.begin());
        extension_.set(send_);
        if (!response)
          return;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        do
        {
          extension_.get(receive_);
          if (receive_[0] == 0x56)
          {
            if (!std::equal(send_.begin(), send_.begin() + 16, receive_.begin() + 1))
              throw std::runtime_error("VIVE extension response does not match the command");
            return;
          }
          if (receive_[0] != 0x55)
            throw std::runtime_error("VIVE extension returned an invalid response status");
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        while (std::chrono::steady_clock::now() < deadline);
        throw std::runtime_error("VIVE extension command timed out");
      }

      void probe()
      {
        std::vector<std::uint8_t> command(send_.size());
        command[0] = 0x51;
        command[1] = 0x52;
        if (command.size() >= 256)
        {
          command[254] = 0x53;
          command[255] = 0x54;
        }
        send(command);
      }

      void stream(bool enabled)
      {
        const std::array<std::uint8_t, 4> command { 0x50, 0x14, 0x00,
          enabled ? std::uint8_t { 1 } : std::uint8_t { 0 } };
        send(command, false);
      }

      void sensor(std::uint8_t address, std::uint8_t value)
      {
        // One-byte sensor register addresses and values, encoded big-endian.
        const std::array<std::uint8_t, 17> command { 0x50, 0xab, 0x60, 1, 1, 0, 0, 0, address,
          0x90, 1, 0, 1, 0, 0, 0, value };
        send(command);
      }
    };
  }  // namespace

  void enable_vive_tracker(TrackerExtension& extension)
  {
    Commands commands(extension);
    commands.probe();
    commands.stream(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    commands.probe();
    constexpr std::array<std::array<std::uint8_t, 2>, 11> registers { { { 0x00, 0x40 },
      { 0x08, 0x01 },
      { 0x70, 0x00 },
      { 0x02, 0xff },
      { 0x03, 0xff },
      { 0x04, 0xff },
      { 0x0e, 0x00 },
      { 0x05, 0xb2 },
      { 0x06, 0xb2 },
      { 0x07, 0xb2 },
      { 0x0f, 0x03 } } };
    for (const auto& reg : registers)
      commands.sensor(reg[0], reg[1]);
    commands.probe();
    commands.stream(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }

  void disable_vive_tracker(TrackerExtension& extension)
  {
    Commands commands(extension);
    commands.probe();
    commands.stream(false);
  }
}  // namespace sof
