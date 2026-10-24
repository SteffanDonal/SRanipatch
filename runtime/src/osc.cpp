#include "sranipatch/osc.hpp"

#include <bit>
#include <cstddef>
#include <stdexcept>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace sranipatch
{
  namespace
  {
    void append_string(std::vector<std::uint8_t>& packet, std::string_view text)
    {
      packet.insert(packet.end(), text.begin(), text.end());
      packet.push_back(0);
      while (packet.size() % 4 != 0)
        packet.push_back(0);
    }

    void encode_into(std::vector<std::uint8_t>& packet, std::string_view address, std::span<const float> values)
    {
      if (address.empty() || address.front() != '/' || address.find('\0') != std::string_view::npos || address.size() > 1024 || values.size() > 128)
        throw std::runtime_error("OSC requires a slash-prefixed address and at most 128 floats");
      packet.clear();
      append_string(packet, address);
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
  }  // namespace

  std::vector<std::uint8_t> encode_osc(std::string_view address, std::span<const float> values)
  {
    std::vector<std::uint8_t> packet;
    encode_into(packet, address, values);
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
    std::string address;
    std::vector<std::uint8_t> packet;
  };

  OscSender::OscSender(const std::string& host, std::uint16_t port, std::string address)
    : impl_(std::make_unique<Impl>())
  {
#ifdef _WIN32
    WSADATA data {};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
      throw std::runtime_error("WSAStartup failed");
    impl_->winsock = true;
#endif
    impl_->address = std::move(address);
    impl_->packet.reserve(2048);
    encode_into(impl_->packet, impl_->address, {});
    addrinfo hints {};
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    addrinfo* resolved {};
    const auto service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &resolved) != 0)
      throw std::runtime_error("OSC host must be a numeric IPv4 or IPv6 address");
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> guard(resolved, freeaddrinfo);
    impl_->socket = ::socket(resolved->ai_family, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
    if (impl_->socket == INVALID_SOCKET)
      throw std::runtime_error("Cannot create OSC socket");
    if (::connect(impl_->socket, resolved->ai_addr, static_cast<int>(resolved->ai_addrlen)) != 0)
      throw std::runtime_error("Cannot connect OSC socket");
    u_long nonblocking = 1;
    if (ioctlsocket(impl_->socket, FIONBIO, &nonblocking) != 0)
      throw std::runtime_error("Cannot make OSC socket nonblocking");
#else
    if (impl_->socket < 0)
      throw std::runtime_error("Cannot create OSC socket");
    if (::connect(impl_->socket, resolved->ai_addr, resolved->ai_addrlen) != 0)
      throw std::runtime_error("Cannot connect OSC socket");
    const int flags = fcntl(impl_->socket, F_GETFL, 0);
    if (flags < 0 || fcntl(impl_->socket, F_SETFL, flags | O_NONBLOCK) < 0)
      throw std::runtime_error("Cannot make OSC socket nonblocking");
#endif
  }

  OscSender::~OscSender() = default;

  bool OscSender::send(std::span<const float> values)
  {
    encode_into(impl_->packet, impl_->address, values);
#ifdef _WIN32
    const auto sent = ::send(impl_->socket, reinterpret_cast<const char*>(impl_->packet.data()),
      static_cast<int>(impl_->packet.size()), 0);
#else
    const auto sent = ::send(impl_->socket, impl_->packet.data(), impl_->packet.size(), MSG_DONTWAIT);
#endif
    return sent >= 0 && static_cast<std::size_t>(sent) == impl_->packet.size();
  }
}  // namespace sranipatch
