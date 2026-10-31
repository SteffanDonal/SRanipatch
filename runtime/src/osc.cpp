#include "sranipatch/osc.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include "steam_link.hpp"

#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace sranipatch
{
  namespace
  {
    void encode_legacy_into(std::vector<std::uint8_t>& packet, std::span<const float> values)
    {
      if (values.size() > max_parameters)
        throw std::runtime_error("OSC requires at most 128 floats");
      packet.assign(legacy_osc_address.begin(), legacy_osc_address.end());
      packet.push_back(0);
      while (packet.size() % 4 != 0)
        packet.push_back(0);
      packet.push_back(',');
      packet.insert(packet.end(), values.size(), 'f');
      packet.push_back(0);
      while (packet.size() % 4 != 0)
        packet.push_back(0);
      for (const float value : values)
      {
        const auto bits = std::bit_cast<std::uint32_t>(value);
        packet.push_back(static_cast<std::uint8_t>(bits >> 24));
        packet.push_back(static_cast<std::uint8_t>(bits >> 16));
        packet.push_back(static_cast<std::uint8_t>(bits >> 8));
        packet.push_back(static_cast<std::uint8_t>(bits));
      }
    }

    void encode_into(std::vector<std::uint8_t>& packet, std::span<const float> s, bool output_vrc_native)
    {
      if (!output_vrc_native)
      {
        encode_legacy_into(packet, s);
        return;
      }
      if (s.size() != 37)
        throw std::runtime_error("Face tracking OSC requires exactly 37 lip parameters");

      // VRChat v1 tracking-key order. Unsupported expressions remain zero.
      std::array<float, 102> mapped {};
      mapped[30] = mapped[70] = mapped[72] = mapped[78] = s[12];
      mapped[31] = mapped[71] = mapped[73] = mapped[79] = s[13];
      mapped[32] = s[16];
      mapped[33] = s[17];
      mapped[34] = mapped[35] = s[18];
      mapped[36] = s[3] + s[4];
      mapped[37] = s[0];
      mapped[38] = s[1];
      mapped[39] = s[2];
      mapped[43] = s[4];
      mapped[44] = mapped[45] = s[23];
      mapped[46] = mapped[47] = s[24];
      mapped[50] = mapped[51] = s[9];
      mapped[52] = mapped[53] = s[10];
      mapped[54] = mapped[55] = mapped[56] = mapped[57] = s[11];
      mapped[58] = mapped[60] = s[19] - s[9];
      mapped[59] = mapped[61] = s[20] - s[9];
      mapped[64] = s[21] - s[10];
      mapped[65] = s[22] - s[10];
      mapped[66] = s[5];
      mapped[67] = s[6];
      mapped[68] = s[7];
      mapped[69] = s[8];
      mapped[74] = mapped[76] = s[14];
      mapped[75] = mapped[77] = s[15];
      mapped[80] = s[25] - s[23];
      mapped[81] = s[25];
      mapped[86] = (s[26] + s[32]) / 2.0F;
      mapped[87] = s[29];
      mapped[88] = s[30];
      mapped[89] = s[28];
      mapped[90] = s[27];
      mapped[91] = s[31];

      // Padded address, blob tag, big-endian blob length, payload and padding.
      packet.assign(132, 0);
      std::copy(face_tracking_address.begin(), face_tracking_address.end(), packet.begin());
      packet[20] = ',';
      packet[21] = 'b';
      packet[27] = static_cast<std::uint8_t>(mapped.size());
      for (std::size_t i = 0; i < mapped.size(); ++i)
      {
        const float value = mapped[i];
        if (std::isfinite(value))
          packet[28 + i] = static_cast<std::uint8_t>(std::clamp(value, 0.0F, 1.0F) * 255.0F);
      }
    }
  }  // namespace

  std::vector<std::uint8_t> encode_osc(std::span<const float> values, bool output_vrc_native)
  {
    std::vector<std::uint8_t> packet;
    encode_into(packet, values, output_vrc_native);
    return packet;
  }

  struct OscSender::Impl
  {
#ifdef _WIN32
    SOCKET socket { INVALID_SOCKET };
    bool winsock {};

    ~Impl()
    {
      if (socket != INVALID_SOCKET)
        closesocket(socket);
      if (winsock)
        WSACleanup();
    }
#else
    int socket { -1 };

    ~Impl()
    {
      if (socket >= 0)
        close(socket);
    }
#endif
    std::vector<std::uint8_t> packet;
    bool output_vrc_native {};
    OscDestination destination;
    void connect_host(const std::string& host, std::uint16_t port);
#ifndef _WIN32
    bool automatic {};
    std::uint16_t port {};
    std::string peer;
    std::chrono::steady_clock::time_point next_discovery {};

    void refresh()
    {
      const auto now = std::chrono::steady_clock::now();
      if (!automatic || now < next_discovery)
        return;
      next_discovery = now + std::chrono::seconds(2);
      const auto wanted = steam_link_peer();
      if (wanted == peer && socket >= 0)
        return;
      if (socket >= 0)
      {
        close(socket);
        socket = -1;
      }
      peer.clear();
      destination.ready = 0;
      if (wanted.empty())
        return;
      try
      {
        connect_host(wanted, port);
        peer = wanted;
      }
      catch (const std::runtime_error&)
      {
        if (socket >= 0)
        {
          close(socket);
          socket = -1;
        }
      }
    }
#endif
  };

  OscSender::OscSender(const std::string& host, std::uint16_t port, bool output_vrc_native)
    : impl_(std::make_unique<Impl>())
  {
#ifdef _WIN32
    WSADATA data {};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
      throw std::runtime_error("WSAStartup failed");
    impl_->winsock = true;
#endif
    impl_->destination.port = port;
    impl_->output_vrc_native = output_vrc_native;
    impl_->packet.reserve(1024);
#ifdef _WIN32
    impl_->connect_host(host.empty() ? "127.0.0.1" : host, port);
#else
    impl_->automatic = host.empty();
    impl_->port = port;
    if (impl_->automatic)
      impl_->refresh();
    else
      impl_->connect_host(host, port);
#endif
  }

  void OscSender::Impl::connect_host(const std::string& host, std::uint16_t destination_port)
  {
    addrinfo hints {};
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    addrinfo* resolved {};
    const auto service = std::to_string(destination_port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &resolved) != 0)
      throw std::runtime_error("OSC host must be a numeric IPv4 or IPv6 address");
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> guard(resolved, freeaddrinfo);
    socket = ::socket(resolved->ai_family, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
    if (socket == INVALID_SOCKET)
      throw std::runtime_error("Cannot create OSC socket");
    if (::connect(socket, resolved->ai_addr, static_cast<int>(resolved->ai_addrlen)) != 0)
      throw std::runtime_error("Cannot connect OSC socket");
    u_long nonblocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonblocking) != 0)
      throw std::runtime_error("Cannot make OSC socket nonblocking");
#else
    if (socket < 0)
      throw std::runtime_error("Cannot create OSC socket");
    if (::connect(socket, resolved->ai_addr, resolved->ai_addrlen) != 0)
      throw std::runtime_error("Cannot connect OSC socket");
    const int flags = fcntl(socket, F_GETFL, 0);
    if (flags < 0 || fcntl(socket, F_SETFL, flags | O_NONBLOCK) < 0)
      throw std::runtime_error("Cannot make OSC socket nonblocking");
#endif
    std::array<char, NI_MAXHOST> numeric_host {};
#ifdef _WIN32
    using AddressLength = int;
    using HostLength = DWORD;
#else
    using AddressLength = socklen_t;
    using HostLength = socklen_t;
#endif
    if (getnameinfo(resolved->ai_addr, static_cast<AddressLength>(resolved->ai_addrlen), numeric_host.data(),
          static_cast<HostLength>(numeric_host.size()), nullptr, 0, NI_NUMERICHOST)
      != 0)
      throw std::runtime_error("Cannot read resolved OSC address");
    set_text(destination.host, numeric_host.data());
    destination.ready = 1;
  }

  OscSender::~OscSender() = default;

  void OscSender::refresh()
  {
#ifndef _WIN32
    impl_->refresh();
#endif
  }

  const OscDestination& OscSender::destination() const
  {
    return impl_->destination;
  }

  bool OscSender::waiting_for_peer() const
  {
#ifdef _WIN32
    return false;
#else
    return impl_->automatic && !impl_->destination.ready;
#endif
  }

  bool OscSender::send(std::span<const float> values)
  {
#ifndef _WIN32
    impl_->refresh();
    if (impl_->socket < 0)
      return true;  // Waiting for Steam Link is not a failed send.
#endif
    encode_into(impl_->packet, values, impl_->output_vrc_native);
#ifdef _WIN32
    const auto sent = ::send(impl_->socket, reinterpret_cast<const char*>(impl_->packet.data()),
      static_cast<int>(impl_->packet.size()), 0);
#else
    const auto sent = ::send(impl_->socket, impl_->packet.data(), impl_->packet.size(), MSG_DONTWAIT);
#endif
    return sent >= 0 && static_cast<std::size_t>(sent) == impl_->packet.size();
  }
}
