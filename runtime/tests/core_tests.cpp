#include "sof/frame.hpp"
#include "sof/ipc.hpp"
#include "sof/osc.hpp"

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
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
    // Row padding must be skipped. Limited-range black / white, neutral chroma.
    const std::array<std::uint8_t, 12> source { 16, 128, 235, 128, 99, 99, 235, 128, 16, 128, 99, 99 };
    sof::yuyv_to_rgb(source.data(), 6, frame);
    require(frame.rgb == std::vector<std::uint8_t> { 0, 0, 0, 255, 255, 255, 255, 255, 255, 0, 0, 0 },
      "YUYV conversion or padded row handling is wrong");
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

  void ipc_roundtrip()
  {
    const TestChannel channel;
    const auto& name = channel.name;
    auto server = std::make_unique<sof::SharedMemory>(name, sof::SharedMemory::Role::server);
    must_throw([&] { sof::SharedMemory duplicate(name, sof::SharedMemory::Role::server); });
    sof::SharedMemory client(name, sof::SharedMemory::Role::client);
    sof::Status status;
    sof::Controls controls;
    status.frames_inferred = 123;
    status.parameter_count = 1;
    status.parameters[0].value = 0.5F;
    require(server->publish(status, controls), "Server snapshot publish failed");
    sof::Status received;
    require(client.read(received, controls) && received.frames_inferred == 123 && received.parameters[0].value == 0.5F,
      "IPC snapshot differs");
    std::uint64_t revision {};
    require(client.control(true, false, revision), "Control write failed");
    require(server->publish(status, controls) && controls.revision == revision && controls.paused == 1 && controls.osc_enabled == 0,
      "Server did not receive controls");
    server.reset();
    server = std::make_unique<sof::SharedMemory>(name, sof::SharedMemory::Role::server);
    require(client.read(received, controls) && received.frames_inferred == 0 && controls.revision == 0,
      "A connected client did not see the restarted server's reset state");
    must_throw([] { sof::SharedMemory invalid("../bad", sof::SharedMemory::Role::server); });
    must_throw([] { sof::SharedMemory missing(test_name(), sof::SharedMemory::Role::client); });
  }

  void process_roundtrip(const char* executable)
  {
    const TestChannel channel;
    const auto& name = channel.name;
    sof::SharedMemory server(name, sof::SharedMemory::Role::server);
    sof::Status status;
    sof::Controls controls;
    status.frames_inferred = 42;
    status.parameter_count = 1;
    status.parameters[0].value = 0.75F;
    require(server.publish(status, controls), "Initial publish failed");
#ifdef _WIN32
    std::string command = "\"" + std::string(executable) + "\" " + name;
    STARTUPINFOA startup {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process {};
    require(CreateProcessA(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
              nullptr, &startup, &process)
        != 0,
      "Cannot start IPC peer");
    CloseHandle(process.hThread);
#else
    const auto pid = fork();
    require(pid >= 0, "fork failed");
    if (pid == 0)
    {
      execl(executable, executable, name.c_str(), nullptr);
      _exit(127);
    }
#endif
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool acknowledged {};
    while (std::chrono::steady_clock::now() < deadline)
    {
      if (server.read(status, controls) && controls.revision == 1 && controls.paused && !controls.osc_enabled)
      {
        status.applied_revision = controls.revision;
        status.state = sof::ServerState::paused;
        status.osc_enabled = 0;
        if (server.publish(status, controls))
        {
          acknowledged = true;
          break;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
#ifdef _WIN32
    const auto waited = WaitForSingleObject(process.hProcess, 6000);
    DWORD code { 1 };
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    require(waited == WAIT_OBJECT_0 && code == 0, "IPC peer failed");
#else
    int code {};
    require(waitpid(pid, &code, 0) == pid && WIFEXITED(code) && WEXITSTATUS(code) == 0, "IPC peer failed");
#endif
    require(acknowledged, "No controls received from child process");
  }
}  // namespace

int main(int argc, char** argv)
{
  try
  {
    if (argc == 2)
      process_roundtrip(argv[1]);
    else
    {
      osc_encoding();
      udp_delivery();
      frames();
      ipc_roundtrip();
    }
    std::cout << "All checks passed\n";
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
