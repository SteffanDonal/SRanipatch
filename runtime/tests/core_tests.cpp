#include "sof/config.hpp"
#include "sof/frame.hpp"
#include "sof/ipc.hpp"
#include "sof/osc.hpp"
#include "sof/vive_tracker.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace
{
  void require(bool condition, const char* message)
  {
    if (!condition)
      throw std::runtime_error(message);
  }

  template <typename F>
  void must_throw(F action)
  {
    bool threw {};
    try
    {
      action();
    }
    catch (const std::exception&)
    {
      threw = true;
    }
    require(threw, "Expected operation to throw");
  }

  std::string test_name()
  {
    return "test-" + std::to_string(sof::monotonic_ns());
  }

  struct TestChannel
  {
    std::string name { test_name() };

    ~TestChannel()
    {
#ifndef _WIN32
      const auto base = "/sof-" + std::to_string(getuid()) + "-" + name;
      shm_unlink(base.c_str());
      shm_unlink((base + "-owner").c_str());
#endif
    }
  };

  void osc_encoding()
  {
    const std::array<float, 2> values { 1.0F, -0.5F };
    const std::vector<std::uint8_t> expected { '/', 'x', 0, 0, ',', 'f', 'f', 0,
      0x3f, 0x80, 0, 0, 0xbf, 0, 0, 0 };
    require(sof::encode_osc("/x", values) == expected, "OSC wire bytes / float endianness are wrong");
    require(sof::encode_osc("/long", {}).size() == 12, "OSC string padding is wrong");
    must_throw([] { sof::encode_osc("invalid", {}); });
    must_throw([] { sof::encode_osc(std::string_view("/x\0bad", 6), {}); });
  }

  void udp_delivery()
  {
#ifdef _WIN32
    WSADATA data {};
    require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "WSAStartup failed");
    const SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    require(socket != INVALID_SOCKET, "Receiver socket failed");
    DWORD timeout = 1000;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    using SocketLength = int;
#else
    const int socket = ::socket(AF_INET, SOCK_DGRAM, 0);
    require(socket >= 0, "Receiver socket failed");
    timeval timeout { 1, 0 };
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    using SocketLength = socklen_t;
#endif
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
      "Receiver bind failed");
    SocketLength length = sizeof(address);
    require(getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) == 0, "getsockname failed");
    {
      sof::OscSender sender("127.0.0.1", ntohs(address.sin_port), "/test");
      const std::array<float, 1> values { 0.25F };
      require(sender.send(values), "OSC UDP send failed");
      std::array<char, 256> buffer {};
      const auto received = recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
      const auto expected = sof::encode_osc("/test", values);
      require(received >= 0 && static_cast<std::size_t>(received) == expected.size(),
        "UDP packet length differs");
      for (std::size_t i = 0; i < expected.size(); ++i)
        require(static_cast<std::uint8_t>(buffer[i]) == expected[i], "UDP packet data differs");
    }
#ifdef _WIN32
    closesocket(socket);
    WSACleanup();
#else
    close(socket);
#endif
  }

  void frames()
  {
    sof::Frame frame;
    frame.width = 2;
    frame.height = 2;
    // Row padding must be skipped and VIVE chroma bytes ignored.
    const std::array<std::uint8_t, 12> source { 16, 128, 235, 128, 99, 99, 235, 128, 16, 128, 99, 99 };
    sof::copy_yuyv_luma(source.data(), 6, frame);
    require(frame.luma == std::vector<std::uint8_t> { 16, 235, 235, 16 },
      "VIVE luminance decoding must ignore chroma and preserve raw luminance");
    frame.yuyv.resize(8);
    sof::copy_yuyv_luma(source.data(), 6, frame);
    require(frame.yuyv == std::vector<std::uint8_t>({ 16, 128, 235, 128, 235, 128, 16, 128 }),
      "VIVE packed YUYV bytes were not preserved");
    sof::copy_yuyv_luma(source.data() + 6, -6, frame);
    require(frame.luma[0] == 235 && frame.luma[2] == 16, "Negative luminance row stride is wrong");
    sof::LatestFrame latest;
    frame.sequence = 1;
    latest.push(frame);
    frame.sequence = 2;
    latest.push(frame);
    sof::Frame consumer;
    std::stop_source stop;
    require(latest.pop(consumer, stop.get_token()) && consumer.sequence == 2,
      "Latest frame was not retained");
    stop.request_stop();
    require(!latest.pop(consumer, stop.get_token()), "Stopped consumer should not wait for an empty mailbox");
  }

  void configuration()
  {
    const auto path = std::filesystem::temp_directory_path() / ("sof-config-" + std::to_string(sof::monotonic_ns()) + ".ini");

    struct Cleanup
    {
      std::filesystem::path path;

      ~Cleanup()
      {
        std::error_code error;
        std::filesystem::remove(path, error);
      }
    } cleanup { path };

    {
      std::ofstream output(path);
      output << "# OSC destination\nosc_host = 192.0.2.4\nosc_port=12345\n" "osc_address = /face/lips\n";
    }
    sof::Config config;
    require(sof::load_osc_config(path, config), "Configuration file was not loaded");
    require(std::string_view(config.osc_host.data()) == "192.0.2.4" && config.osc_port == 12345 && std::string_view(config.osc_address.data()) == "/face/lips",
      "OSC configuration values differ");
    {
      std::ofstream output(path);
      output << "unknown=value\n";
    }
    must_throw([&] { sof::load_osc_config(path, config); });
  }

  void ipc_roundtrip()
  {
    const TestChannel channel;
    const auto& name = channel.name;
    auto server = std::make_unique<sof::SharedMemory>(name, sof::SharedMemory::Role::server);
    must_throw([&] { sof::SharedMemory duplicate(name, sof::SharedMemory::Role::server); });
    sof::SharedMemory client(name, sof::SharedMemory::Role::client);
    sof::Status status;
    status.frames_inferred = 123;
    status.parameter_count = 1;
    status.parameters[0].value = 0.5F;
    require(server->publish(status), "Server snapshot publish failed");
    sof::Status received;
    require(client.read(received) && received.frames_inferred == 123 && received.parameters[0].value == 0.5F,
      "IPC snapshot differs");
    sof::Frame image;
    image.width = sof::capture_width;
    image.height = sof::capture_height;
    image.sequence = 10;
    image.luma.assign(static_cast<std::size_t>(image.width) * image.height, 123);
    require(server->publish_frame(image), "Preview publication failed");
    sof::Frame received_frame;
    require(client.read_frame(received_frame) && received_frame.luma == image.luma && received_frame.sequence == 10,
      "Mapped preview pixels differ");
    require(!client.read_frame(received_frame), "Unchanged preview must not be copied again");
    server.reset();
    server = std::make_unique<sof::SharedMemory>(name, sof::SharedMemory::Role::server);
    require(client.read(received) && received.frames_inferred == 0,
      "A connected client did not see the restarted server's reset state");
    require(!client.read_frame(received_frame), "Restarted server exposed a stale preview");
    must_throw([] { sof::SharedMemory invalid("../bad", sof::SharedMemory::Role::server); });
    must_throw([] { sof::SharedMemory missing(test_name(), sof::SharedMemory::Role::client); });
  }

  void tracker_commands()
  {
    struct FakeExtension final : sof::TrackerExtension
    {
      std::size_t size { 64 };
      std::vector<std::vector<std::uint8_t>> commands;
      bool pending {}, wrong_echo {};

      std::size_t buffer_size() const override
      {
        return size;
      }

      void set(std::span<const std::uint8_t> data) override
      {
        require(data.size() == size, "Vendor command has the wrong buffer size");
        commands.emplace_back(data.begin(), data.end());
        pending = true;
      }

      void get(std::span<std::uint8_t> data) override
      {
        std::fill(data.begin(), data.end(), std::uint8_t { 0 });
        if (pending)
        {
          data[0] = 0x55;
          pending = false;
          return;
        }
        data[0] = 0x56;
        std::copy_n(commands.back().begin(), 16, data.begin() + 1);
        if (wrong_echo)
          data[1] ^= 1;
      }
    };

    for (const std::size_t size : { std::size_t { 64 }, std::size_t { 384 } })
    {
      FakeExtension extension;
      extension.size = size;
      sof::enable_vive_tracker(extension);
      require(extension.commands.size() == 16, "VIVE initialization did not send all sensor commands");
      require(extension.commands[1][1] == 0x14 && extension.commands[1][3] == 0 && extension.commands.back()[1] == 0x14 && extension.commands.back()[3] == 1,
        "VIVE stream enable was not ordered after initialization");
      const std::array<std::uint8_t, 17> expected { 0x50, 0xab, 0x60, 1, 1, 0, 0, 0, 0x70,
        0x90, 1, 0, 1, 0, 0, 0, 0 };
      require(std::equal(expected.begin(), expected.end(), extension.commands[5].begin()),
        "Sensor register command has invalid widths, byte order or page address");
      if (size == 384)
        require(extension.commands.front()[254] == 0x53 && extension.commands.front()[255] == 0x54,
          "384-byte VIVE handshake footer is missing");
      sof::disable_vive_tracker(extension);
      require(extension.commands.back()[3] == 0, "VIVE stream was not disabled");
    }
    FakeExtension invalid;
    invalid.wrong_echo = true;
    must_throw([&] { sof::enable_vive_tracker(invalid); });
  }
}  // namespace

// Exceptions from the body are caught below; clang-tidy misidentifies MSVC exception inheritance.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main()
{
  try
  {
    osc_encoding();
    udp_delivery();
    frames();
    configuration();
    tracker_commands();
    ipc_roundtrip();
    std::cout << "All checks passed\n";
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
