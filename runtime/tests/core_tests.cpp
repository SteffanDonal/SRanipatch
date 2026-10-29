#include "sranipatch/config.hpp"
#include "sranipatch/frame.hpp"
#include "sranipatch/ipc.hpp"
#include "sranipatch/osc.hpp"
#include "sranipatch/vive_tracker.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#else
#include "../src/steam_link.hpp"

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
    return "test-" + std::to_string(sranipatch::monotonic_ns());
  }

  struct TestChannel
  {
    std::string name { test_name() };

    ~TestChannel()
    {
#ifndef _WIN32
      const auto base = "/sranipatch-" + std::to_string(getuid()) + "-" + name;
      shm_unlink(base.c_str());
      shm_unlink((base + "-owner").c_str());
#endif
    }
  };

  void osc_encoding()
  {
    std::array<float, 37> values {};
    const std::vector<std::uint8_t> header {
      '/', 't', 'r', 'a', 'c', 'k', 'i', 'n', 'g', '/', 'f', 'a', 'c', 'e', '/', 'v', '1', 0, 0, 0,
      ',', 'b', 0, 0, 0, 0, 0, 102
    };
    auto expected = header;
    expected.resize(132, 0);
    require(sranipatch::encode_osc(values) == expected, "Neutral face OSC framing or padding is wrong");

    // Distinct inputs exercise the complete mapping, including unused tongue morphs.
    for (std::size_t i = 0; i < values.size(); ++i)
      values[i] = static_cast<float>(i + 1) / 64.0F;
    const std::array<std::uint8_t, 102> payload {
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      51, 55, 67, 71, 75, 75, 35, 3, 7, 11,
      0, 0, 0, 19, 95, 95, 99, 99, 0, 0,
      39, 39, 43, 43, 47, 47, 47, 47, 39, 43,
      39, 43, 0, 0, 43, 47, 23, 27, 31, 35,
      51, 55, 51, 55, 59, 63, 59, 63, 51, 55,
      7, 103, 0, 0, 0, 0, 119, 119, 123, 115,
      111, 127, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    std::copy(payload.begin(), payload.end(), expected.begin() + 28);
    require(sranipatch::encode_osc(values) == expected, "Face tracking key mapping differs");

    values.fill(0);
    values[9] = 1;
    values[14] = 1;
    auto packet = sranipatch::encode_osc(values);
    require(packet[28 + 50] == 255 && packet[28 + 52] == 0 && packet[28 + 76] == 255 && packet[28 + 77] == 0,
      "Upper overturn or right sadness leaked into lower or left outputs");
    values.fill(0);
    values[10] = 1;
    values[15] = 1;
    packet = sranipatch::encode_osc(values);
    require(packet[28 + 50] == 0 && packet[28 + 52] == 255 && packet[28 + 76] == 0 && packet[28 + 77] == 255,
      "Lower overturn or left sadness did not drive the corrected outputs");

    values.fill(0);
    values[0] = std::numeric_limits<float>::quiet_NaN();
    values[1] = -std::numeric_limits<float>::infinity();
    values[2] = std::numeric_limits<float>::infinity();
    values[3] = 0.5F;
    values[9] = 0.75F;
    values[19] = 0.25F;
    values[10] = -0.5F;
    values[21] = 0.75F;
    values[26] = 3;
    values[32] = -1;
    packet = sranipatch::encode_osc(values);
    require(packet[28 + 36] == 127 && packet[28 + 37] == 0 && packet[28 + 38] == 0 && packet[28 + 39] == 0,
      "Truncation or nonfinite handling is wrong");
    require(packet[28 + 58] == 0 && packet[28 + 52] == 0 && packet[28 + 64] == 255 && packet[28 + 86] == 255,
      "Mapped expressions must be clamped after evaluation");
    values[3] = std::numeric_limits<float>::infinity();
    values[26] = std::numeric_limits<float>::quiet_NaN();
    packet = sranipatch::encode_osc(values);
    require(packet[28 + 36] == 0 && packet[28 + 86] == 0, "Nonfinite mapped expressions must encode zero");
    must_throw([] { sranipatch::encode_osc({}); });
    must_throw([] { sranipatch::encode_osc(std::array<float, 36> {}); });
    must_throw([] { sranipatch::encode_osc(std::array<float, 38> {}); });
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
#ifdef _WIN32
    const std::array hosts { "127.0.0.1", "" };
#else
    const std::array hosts { "127.0.0.1" };
#endif
    for (const auto* host : hosts)
    {
      sranipatch::OscSender sender(host, ntohs(address.sin_port));
      require(std::string_view(sender.destination().host.data()) == "127.0.0.1" && sender.destination().port == ntohs(address.sin_port) && sender.destination().ready,
        "Sender did not report its resolved numeric destination");
      std::array<float, 37> values {};
      values[3] = 0.25F;
      require(sender.send(values), "OSC UDP send failed");
      std::array<char, 256> buffer {};
      const auto received = recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
      const auto expected = sranipatch::encode_osc(values);
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
    sranipatch::Frame frame;
    frame.width = 2;
    frame.height = 2;
    // Row padding must be skipped and VIVE chroma bytes ignored.
    const std::array<std::uint8_t, 12> source { 16, 128, 235, 128, 99, 99, 235, 128, 16, 128, 99, 99 };
    sranipatch::copy_yuyv_luma(source.data(), 6, frame);
    require(frame.luma == std::vector<std::uint8_t> { 16, 235, 235, 16 },
      "VIVE luminance decoding must ignore chroma and preserve raw luminance");
    frame.yuyv.resize(8);
    sranipatch::copy_yuyv_luma(source.data(), 6, frame);
    require(frame.yuyv == std::vector<std::uint8_t>({ 16, 128, 235, 128, 235, 128, 16, 128 }),
      "VIVE packed YUYV bytes were not preserved");
    sranipatch::copy_yuyv_luma(source.data() + 6, -6, frame);
    require(frame.luma[0] == 235 && frame.luma[2] == 16, "Negative luminance row stride is wrong");
    sranipatch::LatestFrame latest;
    frame.sequence = 1;
    latest.push(frame);
    frame.sequence = 2;
    latest.push(frame);
    sranipatch::Frame consumer;
    std::stop_source stop;
    require(latest.pop(consumer, stop.get_token()) && consumer.sequence == 2,
      "Latest frame was not retained");
    stop.request_stop();
    require(!latest.pop(consumer, stop.get_token()), "Stopped consumer should not wait for an empty mailbox");
  }

  void configuration()
  {
    const auto path = std::filesystem::temp_directory_path() / ("sranipatch-config-" + std::to_string(sranipatch::monotonic_ns()) + ".ini");

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
      output << "# OSC destination\nosc_host = 192.0.2.4\nosc_port=12345\n";
    }
    sranipatch::Config config;
    require(sranipatch::load_osc_config(path, config), "Configuration file was not loaded");
    require(std::string_view(config.osc_host.data()) == "192.0.2.4" && config.osc_port == 12345,
      "OSC configuration values differ");
    {
      std::ofstream output(path);
      output << "unknown=value\n";
    }
    must_throw([&] { sranipatch::load_osc_config(path, config); });
    must_throw([] { sranipatch::parse_osc_config("osc_address=/legacy\n", {}); });
    for (const auto* value : { "", "\"\"", "null" })
    {
      {
        std::ofstream output(path);
        output << "osc_host = " << value << '\n';
      }
      sranipatch::set_text(config.osc_host, "192.0.2.4");
      require(sranipatch::load_osc_config(path, config) && config.osc_host[0] == '\0',
        "Default OSC host value did not clear the explicit override");
    }
    {
      std::ofstream output(path);
      output << "osc_port=9337\n";
    }
    sranipatch::Config defaults;
    require(sranipatch::load_osc_config(path, defaults) && defaults.osc_host[0] == '\0',
      "Omitted OSC host did not retain automatic/default mode");
  }
#ifndef _WIN32
  void steam_link_discovery()
  {
    const auto proc = std::filesystem::temp_directory_path() / test_name();

    struct Cleanup
    {
      std::filesystem::path path;

      ~Cleanup()
      {
        std::error_code error;
        std::filesystem::remove_all(path, error);
      }
    } cleanup { proc };

    require(sranipatch::steam_link_peer(proc).empty(), "Missing proc directory should have no peer");
    std::filesystem::create_directories(proc / "123" / "fd");
    std::filesystem::create_directories(proc / "456" / "fd");
    std::filesystem::create_directories(proc / "net");
    std::filesystem::create_symlink("/bin/vrlink", proc / "123" / "exe");
    std::filesystem::create_symlink("socket:[333]", proc / "123" / "fd" / "4");
    std::filesystem::create_symlink("/bin/other", proc / "456" / "exe");
    std::filesystem::create_symlink("socket:[999]", proc / "456" / "fd" / "4");
    const auto write_table = [&](const char* name, const std::string& rows) {
      std::ofstream table(proc / "net" / name);
      table << "sl local_address rem_address st tx_queue tr retrnsmt uid timeout inode\n"
            << rows;
    };
    const auto row = [](const std::string& ip, const std::string& state, const std::string& inode) {
      return "0: 00000000:1234 " + ip + ":28A0 " + state + " 0:0 0:0 0 1000 0 " + inode + "\n";
    };
    const auto ignored = std::string("malformed\n") + row("1A4E230A", "01", "999") + row("1A4E230A", "07", "333") + row("0100007F", "01", "333") + row("00000000", "01", "333") + row("XXXXXXXX", "01", "333");
    write_table("udp", ignored);
    require(sranipatch::steam_link_peer(proc).empty(), "Unrelated, unconnected or invalid peer accepted");
    write_table("udp", ignored + row("1A4E230A", "01", "333"));
    require(sranipatch::steam_link_peer(proc) == "10.35.78.26", "IPv4 peer decoding failed");
    write_table("udp", row("054E230A", "01", "333"));
    require(sranipatch::steam_link_peer(proc) == "10.35.78.5", "Changed peer was not discovered");
    write_table("udp", "");
    write_table("udp6", row("0000000000000000FFFF00001A4E230A", "01", "333"));
    require(sranipatch::steam_link_peer(proc) == "10.35.78.26", "IPv4-mapped peer decoding failed");
    write_table("udp6", row("B80D0120000000000000000001000000", "01", "333"));
    require(sranipatch::steam_link_peer(proc) == "2001:db8::1", "IPv6 peer decoding failed");
    write_table("udp6", row("00000000000000000000000001000000", "01", "333") + row("00000000000000000000000000000000", "01", "333") + row("0000000000000000FFFF00000100007F", "01", "333"));
    require(sranipatch::steam_link_peer(proc).empty(), "IPv6 loopback or unspecified peer accepted");
    write_table("udp", row("1A4E230A", "01", "333"));
    std::filesystem::remove(proc / "123" / "fd" / "4");
    require(sranipatch::steam_link_peer(proc).empty(), "Closed Steam Link socket retained a stale peer");
    sranipatch::OscSender waiting("", 9337);
    require(waiting.send({}), "Waiting for Steam Link should not count as a send failure");
    require(!waiting.destination().ready && !waiting.destination().host[0] && waiting.destination().port == 9337,
      "Unresolved automatic destination must be reported as waiting");
  }
#endif
  void ipc_roundtrip()
  {
    const TestChannel channel;
    const auto& name = channel.name;
    auto server = std::make_unique<sranipatch::SharedMemory>(name, sranipatch::SharedMemory::Role::server);
    must_throw([&] { sranipatch::SharedMemory duplicate(name, sranipatch::SharedMemory::Role::server); });
    sranipatch::SharedMemory client(name, sranipatch::SharedMemory::Role::client);
    sranipatch::Status status;
    status.frames_inferred = 123;
    status.parameter_count = 1;
    status.parameters[0].value = 0.5F;
    require(server->publish(status), "Server snapshot publish failed");
    sranipatch::Status received;
    require(client.read(received) && received.frames_inferred == 123 && received.parameters[0].value == 0.5F,
      "IPC snapshot differs");
    sranipatch::Frame image;
    image.width = sranipatch::capture_width;
    image.height = sranipatch::capture_height;
    image.sequence = 10;
    image.luma.assign(static_cast<std::size_t>(image.width) * image.height, 123);
    require(server->publish_frame(image), "Preview publication failed");
    sranipatch::Frame received_frame;
    require(client.read_frame(received_frame) && received_frame.luma == image.luma && received_frame.sequence == 10,
      "Mapped preview pixels differ");
    require(!client.read_frame(received_frame), "Unchanged preview must not be copied again");
    server.reset();
    server = std::make_unique<sranipatch::SharedMemory>(name, sranipatch::SharedMemory::Role::server);
    require(client.read(received) && received.frames_inferred == 0,
      "A connected client did not see the restarted server's reset state");
    require(!client.read_frame(received_frame), "Restarted server exposed a stale preview");
    must_throw([] { sranipatch::SharedMemory invalid("../bad", sranipatch::SharedMemory::Role::server); });
    must_throw([] { sranipatch::SharedMemory missing(test_name(), sranipatch::SharedMemory::Role::client); });
  }

  void tracker_commands()
  {
    struct FakeExtension final : sranipatch::TrackerExtension
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
      sranipatch::enable_vive_tracker(extension);
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
      sranipatch::disable_vive_tracker(extension);
      require(extension.commands.back()[3] == 0, "VIVE stream was not disabled");
    }
    FakeExtension invalid;
    invalid.wrong_echo = true;
    must_throw([&] { sranipatch::enable_vive_tracker(invalid); });
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
#ifndef _WIN32
    steam_link_discovery();
#endif
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
