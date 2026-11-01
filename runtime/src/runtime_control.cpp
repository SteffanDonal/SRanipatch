#include "sranipatch/runtime_control.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <type_traits>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace sranipatch
{
  namespace
  {
    constexpr std::uint32_t magic = 0x53524331;
    constexpr std::uint32_t version = 1;

    struct Block
    {
      std::uint32_t magic {}, version {}, size {};
      RuntimeControl state;
    };

    static_assert(std::is_trivially_copyable_v<Block>);
    static_assert(std::is_standard_layout_v<Block>);
    static_assert(sizeof(DriverControl) == 296);
    static_assert(sizeof(ServerControl) == 296);
    static_assert(sizeof(Block) == 608);
    static_assert(offsetof(Block, state) == 16);
  }

  struct ControlChannel::Impl
  {
    Block* block {};
#ifdef _WIN32
    HANDLE mapping {}, mutex {};

    ~Impl()
    {
      if (block)
        UnmapViewOfFile(block);
      if (mapping)
        CloseHandle(mapping);
      if (mutex)
        CloseHandle(mutex);
    }

    void lock()
    {
      const auto result = WaitForSingleObject(mutex, 100);
      if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED)
        throw std::runtime_error("Runtime control lock unavailable");
    }

    void unlock() { ReleaseMutex(mutex); }
#else
    int fd { -1 };

    ~Impl()
    {
      if (block)
        munmap(block, sizeof(Block));
      if (fd >= 0)
        close(fd);
    }

    void lock()
    {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
      do
      {
        if (flock(fd, LOCK_EX | LOCK_NB) == 0)
          return;
        if (errno != EWOULDBLOCK && errno != EINTR)
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      while (std::chrono::steady_clock::now() < deadline);
      throw std::runtime_error("Runtime control lock unavailable");
    }

    void unlock() { flock(fd, LOCK_UN); }
#endif
    struct Guard
    {
      Impl& impl;

      explicit Guard(Impl& value) : impl(value) { impl.lock(); }

      ~Guard() { impl.unlock(); }

      Guard(const Guard&) = delete;
      Guard& operator=(const Guard&) = delete;
    };

    void validate() const
    {
      if (block->magic != magic || block->version != version || block->size != sizeof(Block))
        throw std::runtime_error("Unsupported runtime control protocol");
    }
  };

  ControlChannel::ControlChannel(std::string name) : impl_(std::make_unique<Impl>())
  {
    if (name.empty() || name.size() > 64 || !std::all_of(name.begin(), name.end(), [](unsigned char c) {
          return std::isalnum(c) || c == '-' || c == '_';
        }))
      throw std::runtime_error("Invalid runtime control name");
#ifdef _WIN32
    const auto base = "Local\\sranipatch-control-" + name;
    impl_->mutex = CreateMutexA(nullptr, FALSE, (base + "-lock").c_str());
    if (!impl_->mutex)
      throw std::runtime_error("Cannot create runtime control lock");
    Impl::Guard guard(*impl_);
    impl_->mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
      static_cast<DWORD>(sizeof(Block)), base.c_str());
    if (!impl_->mapping)
      throw std::runtime_error("Cannot create runtime control mapping");
    impl_->block = static_cast<Block*>(MapViewOfFile(impl_->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Block)));
    if (!impl_->block)
      throw std::runtime_error("Cannot map runtime control");
#else
    const auto base = "/sranipatch-control-" + std::to_string(getuid()) + "-" + name;
    impl_->fd = shm_open(base.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (impl_->fd < 0)
      throw std::runtime_error("Cannot open runtime control");
    Impl::Guard guard(*impl_);
    struct stat info {};
    if (fstat(impl_->fd, &info) != 0 || info.st_uid != getuid() || (info.st_mode & 0077) != 0)
      throw std::runtime_error("Runtime control must be private to the current user");
    if (info.st_size == 0)
    {
      if (ftruncate(impl_->fd, static_cast<off_t>(sizeof(Block))) != 0)
        throw std::runtime_error("Cannot size runtime control");
    }
    else if (info.st_size != static_cast<off_t>(sizeof(Block)))
      throw std::runtime_error("Unsupported runtime control size");
    void* mapped = mmap(nullptr, sizeof(Block), PROT_READ | PROT_WRITE, MAP_SHARED, impl_->fd, 0);
    if (mapped == MAP_FAILED)
      throw std::runtime_error("Cannot map runtime control");
    impl_->block = static_cast<Block*>(mapped);
#endif
    if (impl_->block->magic == 0)
    {
      const Block initial { magic, version, sizeof(Block), {} };
      std::memcpy(impl_->block, &initial, sizeof(Block));
    }
    impl_->validate();
  }

  ControlChannel::~ControlChannel() = default;

  RuntimeControl ControlChannel::read()
  {
    Impl::Guard guard(*impl_);
    impl_->validate();
    return impl_->block->state;
  }

  void ControlChannel::publish_driver(const DriverControl& state)
  {
    Impl::Guard guard(*impl_);
    impl_->validate();
    impl_->block->state.driver = state;
  }

  void ControlChannel::publish_server(const ServerControl& state)
  {
    Impl::Guard guard(*impl_);
    impl_->validate();
    impl_->block->state.server = state;
  }
}
