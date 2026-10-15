#include "sof/ipc.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>
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

namespace sof
{
  struct SharedMemory::Impl
  {
    SharedBlock* block {};
#ifdef _WIN32
    HANDLE mapping {}, mutex {}, owner {};
    bool owns_server {};

    ~Impl()
    {
      if (block)
        UnmapViewOfFile(block);
      if (mapping)
        CloseHandle(mapping);
      if (mutex)
        CloseHandle(mutex);
      if (owns_server)
        ReleaseMutex(owner);
      if (owner)
        CloseHandle(owner);
    }

    bool lock(bool blocking = false)
    {
      const auto result = WaitForSingleObject(mutex, blocking ? 5000 : 0);
      return result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    }

    void unlock()
    {
      ReleaseMutex(mutex);
    }
#else
    int fd { -1 }, owner { -1 };

    ~Impl()
    {
      if (block)
        munmap(block, sizeof(SharedBlock));
      if (fd >= 0)
        close(fd);
      if (owner >= 0)
        close(owner);
    }

    bool lock(bool blocking = false)
    {
      int result;
      do
      {
        result = flock(fd, LOCK_EX | (blocking ? 0 : LOCK_NB));
      }
      while (result < 0 && errno == EINTR);
      return result == 0;
    }

    void unlock()
    {
      flock(fd, LOCK_UN);
    }
#endif
    struct Guard
    {
      Impl& impl;

      ~Guard()
      {
        impl.unlock();
      }
    };

    void validate() const
    {
      if (block->magic != protocol_magic || block->version != protocol_version || block->size != sizeof(SharedBlock))
        throw std::runtime_error("Shared-memory protocol mismatch; use matching client/server builds");
    }
  };

  SharedMemory::SharedMemory(std::string name, Role role) : impl_(std::make_unique<Impl>())
  {
    if (name.empty() || name.size() > 64 || !std::all_of(name.begin(), name.end(), [](unsigned char c) {
          return std::isalnum(c) || c == '-' || c == '_';
        }))
      throw std::runtime_error("IPC name must contain 1-64 letters, digits, '_' or '-'");
    const bool server = role == Role::server;
#ifdef _WIN32
    const auto base = std::string("Local\\sof-") + name;
    if (server)
    {
      impl_->owner = CreateMutexA(nullptr, FALSE, (base + "-owner").c_str());
      if (!impl_->owner)
        throw std::runtime_error("Cannot create server ownership mutex");
      if (GetLastError() == ERROR_ALREADY_EXISTS)
        throw std::runtime_error("A server already owns this IPC name");
      const auto acquired = WaitForSingleObject(impl_->owner, 0);
      if (acquired != WAIT_OBJECT_0 && acquired != WAIT_ABANDONED)
        throw std::runtime_error("A server already owns this IPC name");
      impl_->owns_server = true;
    }
    impl_->mutex = server ? CreateMutexA(nullptr, FALSE, (base + "-lock").c_str())
                          : OpenMutexA(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, (base + "-lock").c_str());
    if (!impl_->mutex)
      throw std::runtime_error("Server IPC lock is unavailable");
    impl_->mapping = server ? CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                static_cast<DWORD>(sizeof(SharedBlock)), base.c_str())
                            : OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, base.c_str());
    if (!impl_->mapping)
      throw std::runtime_error("Server shared memory is unavailable");
    impl_->block = static_cast<SharedBlock*>(
      MapViewOfFile(impl_->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedBlock))
    );
    if (!impl_->block)
      throw std::runtime_error("Cannot map server shared memory");
#else
    const auto base = "/sof-" + std::to_string(getuid()) + "-" + name;
    auto open_region = [](const std::string& path, bool create) {
      const int fd = shm_open(path.c_str(), O_RDWR | O_CLOEXEC | (create ? O_CREAT : 0), 0600);
      if (fd < 0)
        throw std::runtime_error("Shared memory unavailable: " + path);
      struct stat info {};
      if (fstat(fd, &info) < 0 || info.st_uid != getuid() || (info.st_mode & 0077) != 0)
      {
        close(fd);
        throw std::runtime_error("Shared memory must be private to the current user");
      }
      return fd;
    };
    if (server)
    {
      impl_->owner = open_region(base + "-owner", true);
      if (flock(impl_->owner, LOCK_EX | LOCK_NB) != 0)
        throw std::runtime_error("A server already owns this IPC name");
    }
    impl_->fd = open_region(base, server);
    // Lock before changing the mapping length; never truncate an active client's region.
    if (!impl_->lock(true))
      throw std::runtime_error("Cannot lock shared memory");
    {
      Impl::Guard guard { *impl_ };
      struct stat info {};
      if (fstat(impl_->fd, &info) < 0)
        throw std::runtime_error("Cannot inspect shared memory");
      if (info.st_size != static_cast<off_t>(sizeof(SharedBlock)))
      {
        if (!server)
          throw std::runtime_error("Shared-memory size mismatch; use matching client/server builds");
        if (info.st_size != 0)
        {
          // A protocol upgrade can change the block size. Replace the named
          // region while stale clients retain a safe handle to the old one.
          if (shm_unlink(base.c_str()) < 0)
          {
            throw std::runtime_error("Cannot replace obsolete shared memory");
          }
          close(impl_->fd);
          impl_->fd = shm_open(base.c_str(), O_RDWR | O_CLOEXEC | O_CREAT | O_EXCL, 0600);
          if (impl_->fd < 0 || !impl_->lock(true))
            throw std::runtime_error("Cannot create replacement shared memory");
        }
        if (ftruncate(impl_->fd, static_cast<off_t>(sizeof(SharedBlock))) < 0)
          throw std::runtime_error("Cannot size shared memory");
      }
      void* mapped = mmap(nullptr, sizeof(SharedBlock), PROT_READ | PROT_WRITE, MAP_SHARED, impl_->fd, 0);
      if (mapped == MAP_FAILED)
        throw std::runtime_error("Cannot map shared memory");
      impl_->block = static_cast<SharedBlock*>(mapped);
    }
#endif
    if (!impl_->lock(true))
      throw std::runtime_error("Cannot initialize shared memory");
    Impl::Guard guard { *impl_ };
    if (server)
    {
      const auto initial = std::make_unique<SharedBlock>();
      std::memcpy(impl_->block, initial.get(), sizeof(SharedBlock));
    }
    else
    {
      impl_->validate();
    }
  }

  SharedMemory::~SharedMemory() = default;

  bool SharedMemory::read(Status& status, Controls& controls)
  {
    if (!impl_->lock())
      return false;
    Impl::Guard guard { *impl_ };
    impl_->validate();
    status = impl_->block->status;
    controls = impl_->block->controls;
    if (status.parameter_count > max_parameters)
      throw std::runtime_error("Invalid IPC parameter count");
    return true;
  }

  bool SharedMemory::publish(const Status& status, Controls& controls)
  {
    if (!impl_->lock())
      return false;
    Impl::Guard guard { *impl_ };
    impl_->validate();
    impl_->block->status = status;
    controls = impl_->block->controls;
    return true;
  }

  bool SharedMemory::control(bool paused, bool osc_enabled, std::uint64_t& revision)
  {
    if (!impl_->lock())
      return false;
    Impl::Guard guard { *impl_ };
    impl_->validate();
    auto& controls = impl_->block->controls;
    controls.paused = paused ? 1U : 0U;
    controls.osc_enabled = osc_enabled ? 1U : 0U;
    revision = ++controls.revision;
    return true;
  }

  bool SharedMemory::publish_frame(const Frame& frame)
  {
    if (frame.width != capture_width || frame.height != capture_height || frame.luma.size() != static_cast<std::size_t>(capture_width) * capture_height)
      throw std::runtime_error("Preview is not a 400x400 luminance VIVE frame");
    if (!impl_->lock())
      return false;
    Impl::Guard guard { *impl_ };
    impl_->validate();
    auto& preview = impl_->block->preview;
    preview.timestamp_ns = frame.timestamp_ns;
    std::memcpy(preview.luma.data(), frame.luma.data(), preview.luma.size());
    preview.sequence = frame.sequence;
    return true;
  }

  bool SharedMemory::read_frame(Frame& frame)
  {
    if (!impl_->lock())
      return false;
    Impl::Guard guard { *impl_ };
    impl_->validate();
    const auto& preview = impl_->block->preview;
    if (!preview.sequence || preview.sequence == frame.sequence)
      return false;
    frame.width = capture_width;
    frame.height = capture_height;
    frame.timestamp_ns = preview.timestamp_ns;
    frame.luma.resize(preview.luma.size());
    std::memcpy(frame.luma.data(), preview.luma.data(), preview.luma.size());
    frame.sequence = preview.sequence;
    return true;
  }
}  // namespace sof
