#include "sranipatch/diagnostics.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <iomanip>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace sranipatch
{
  namespace
  {
    std::atomic<DiagnosticLog*> current_log {};
    constexpr std::size_t queue_limit = 1024;
    constexpr std::size_t text_limit = 2048;

    std::filesystem::path environment_path(const char* name)
    {
#ifdef _WIN32
      // Preserve non-ASCII Windows profile paths.
      std::wstring key;
      for (const char* c = name; *c; ++c)
        key.push_back(static_cast<wchar_t>(*c));
      wchar_t* value {};
      std::size_t size {};
      if (_wdupenv_s(&value, &size, key.c_str()) != 0)
        return {};
      std::unique_ptr<wchar_t, decltype(&std::free)> owner(value, &std::free);
      return value ? std::filesystem::path(value) : std::filesystem::path {};
#else
      const char* value = std::getenv(name);
      return value ? std::filesystem::path(value) : std::filesystem::path {};
#endif
    }

    unsigned long process_id()
    {
#ifdef _WIN32
      return GetCurrentProcessId();
#else
      return static_cast<unsigned long>(getpid());
#endif
    }

    std::string clean(std::string_view text)
    {
      std::string result(text.substr(0, text_limit));
      for (char& c : result)
        if (static_cast<unsigned char>(c) < 32 || c == 127)
          c = ' ';
      if (text.size() > text_limit)
        result += " [truncated]";
      return result;
    }

    std::string record(std::string_view event, std::string_view details)
    {
      const auto now = std::chrono::system_clock::now();
      const auto seconds = std::chrono::floor<std::chrono::seconds>(now);
      const auto time = std::chrono::system_clock::to_time_t(seconds);
      std::tm utc {};
#ifdef _WIN32
      gmtime_s(&utc, &time);
#else
      gmtime_r(&time, &utc);
#endif
      const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds).count();
      std::ostringstream line;
      line << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0') << std::setw(3)
           << milliseconds << "Z pid=" << process_id() << " event=" << clean(event) << ' ' << clean(details) << '\n';
      return line.str();
    }

    void unavailable() noexcept
    {
      std::fputs("SRanipatch: diagnostic logging unavailable; tracking will continue\n", stderr);
    }

    class File
    {
    public:
#ifdef _WIN32
      HANDLE handle { INVALID_HANDLE_VALUE };

      explicit File(const std::filesystem::path& path, bool append)
      {
        handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
          throw std::runtime_error("Cannot open diagnostic file");
        if (append)
        {
          LARGE_INTEGER offset {};
          if (!SetFilePointerEx(handle, offset, nullptr, FILE_END))
          {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
            throw std::runtime_error("Cannot seek diagnostic file");
          }
        }
      }

      ~File()
      {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
      }

      void lock()
      {
        OVERLAPPED position {};
        if (!LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &position))
          throw std::runtime_error("Cannot lock diagnostic file");
      }

      void unlock() noexcept
      {
        OVERLAPPED position {};
        UnlockFileEx(handle, 0, MAXDWORD, MAXDWORD, &position);
      }

      void write(std::string_view line)
      {
        DWORD written {};
        if (!WriteFile(handle, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) || written != line.size())
          throw std::runtime_error("Cannot write diagnostic file");
      }

      void sync()
      {
        if (!FlushFileBuffers(handle))
          throw std::runtime_error("Cannot flush diagnostic file");
      }
#else
      int handle { -1 };

      explicit File(const std::filesystem::path& path, bool append)
      {
        handle = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | (append ? O_APPEND : 0), 0600);
        if (handle < 0)
          throw std::runtime_error("Cannot open diagnostic file");
      }

      ~File()
      {
        if (handle >= 0) ::close(handle);
      }

      void lock()
      {
        int result;
        do
        {
          result = flock(handle, LOCK_EX);
        }
        while (result < 0 && errno == EINTR);
        if (result < 0)
          throw std::runtime_error("Cannot lock diagnostic file");
      }

      void unlock() noexcept { flock(handle, LOCK_UN); }

      void write(std::string_view line)
      {
        while (!line.empty())
        {
          const auto written = ::write(handle, line.data(), line.size());
          if (written < 0 && errno == EINTR)
            continue;
          if (written <= 0)
            throw std::runtime_error("Cannot write diagnostic file");
          line.remove_prefix(static_cast<std::size_t>(written));
        }
      }

      void sync()
      {
        if (fsync(handle) != 0)
          throw std::runtime_error("Cannot flush diagnostic file");
      }
#endif
      File(const File&) = delete;
      File& operator=(const File&) = delete;
    };

    struct FileLock
    {
      File& file;

      explicit FileLock(File& value) : file(value) { file.lock(); }

      ~FileLock() { file.unlock(); }
    };
  }

  std::filesystem::path diagnostic_directory()
  {
#ifdef _WIN32
    const auto root = environment_path("LOCALAPPDATA");
    return root.empty() ? root : root / "sranipatch" / "logs";
#else
    auto root = environment_path("XDG_STATE_HOME");
    if (root.empty())
    {
      root = environment_path("HOME");
      if (root.empty())
        return {};
      root /= ".local/state";
    }
    return root / "sranipatch" / "logs";
#endif
  }

  struct DiagnosticLog::Impl
  {
    struct Entry
    {
      std::string text;
      LogDelivery delivery;
      std::uint64_t sequence;
    };

    std::filesystem::path path;
    std::size_t file_bytes;
    unsigned file_count;
    std::mutex mutex;
    std::condition_variable ready, completed;
    std::deque<Entry> queue;
    std::uint64_t sequence {}, written {}, dropped {};
    bool stopping {}, failed {};
    File lock_file;
    std::thread worker;

    Impl(const std::filesystem::path& directory, std::string_view component, LogRetention retention)
      : path(directory / (std::string(component) + ".log")), file_bytes(retention.file_bytes), file_count(retention.file_count),
        lock_file(directory / (std::string(component) + ".lock"), false), worker([this] { run(); }) {}

    ~Impl()
    {
      {
        std::lock_guard lock(mutex);
        stopping = true;
      }
      ready.notify_one();
      worker.join();
    }

    void append(std::string_view line, bool durable)
    {
      FileLock lock(lock_file);
      std::error_code error;
      const auto size = std::filesystem::file_size(path, error);
      if (!error && size + line.size() > file_bytes)
      {
        for (unsigned i = file_count - 1; i > 0; --i)
        {
          auto destination = path;
          destination += "." + std::to_string(i);
          auto source = path;
          if (i != 1)
            source += "." + std::to_string(i - 1);
          std::filesystem::remove(destination);
          if (std::filesystem::exists(source))
            std::filesystem::rename(source, destination);
        }
        if (file_count == 1)
          std::filesystem::remove(path);
      }
      File file(path, true);
      // Even pathological error text must respect the rotation budget.
      if (line.size() > file_bytes)
      {
        file.write(line.substr(0, file_bytes - 1));
        file.write("\n");
      }
      else
        file.write(line);
      if (durable)
        file.sync();
    }

    void run() noexcept
    {
      try
      {
        for (;;)
        {
          std::deque<Entry> batch;
          std::uint64_t lost {};
          bool closing {};
          {
            std::unique_lock lock(mutex);
            ready.wait_for(lock, std::chrono::seconds(1), [this] {
              return stopping || std::any_of(queue.begin(), queue.end(), [](const Entry& entry) {
                return entry.delivery != LogDelivery::buffered;
              });
            });
            batch.swap(queue);
            lost = std::exchange(dropped, 0);
            closing = stopping;
          }
          completed.notify_all();
          if (lost)
            append(record("log.records_dropped", std::to_string(lost)), true);
          for (const auto& entry : batch)
          {
            append(entry.text, closing || entry.delivery != LogDelivery::buffered);
            {
              std::lock_guard lock(mutex);
              written = entry.sequence;
            }
            completed.notify_all();
          }
          if (closing)
            break;
        }
      }
      catch (...)
      {
        {
          std::lock_guard lock(mutex);
          failed = true;
          queue.clear();
        }
        unavailable();
        completed.notify_all();
      }
    }
  };

  DiagnosticLog::DiagnosticLog(const std::filesystem::path& directory, std::string_view component,
    LogRetention retention) noexcept
  {
    try
    {
      if (directory.empty() || component.empty() || component.find_first_not_of("abcdefghijklmnopqrstuvwxyz-") != std::string_view::npos
        || retention.file_bytes < 256 || retention.file_count == 0)
        throw std::runtime_error("Invalid diagnostic log options");
      std::filesystem::create_directories(directory);
      impl_ = std::make_unique<Impl>(directory, component, retention);
    }
    catch (...)
    {
      unavailable();
    }
  }

  DiagnosticLog::~DiagnosticLog() = default;

  bool DiagnosticLog::write(std::string_view event, std::string_view details, LogDelivery delivery) noexcept
  {
    try
    {
      if (!impl_)
        return false;
      auto text = record(event, details);
      std::unique_lock lock(impl_->mutex);
      if (impl_->failed || impl_->stopping)
        return false;
      if (impl_->queue.size() >= queue_limit)
      {
        if (delivery == LogDelivery::durable)
        {
          impl_->ready.notify_one();
          impl_->completed.wait(lock, [&] { return impl_->failed || impl_->queue.size() < queue_limit; });
          if (impl_->failed)
            return false;
        }
        else
        {
          ++impl_->dropped;
          return false;
        }
      }
      const auto sequence = ++impl_->sequence;
      impl_->queue.push_back({ std::move(text), delivery, sequence });
      if (delivery != LogDelivery::buffered)
        impl_->ready.notify_one();
      if (delivery == LogDelivery::durable)
        impl_->completed.wait(lock, [&] { return impl_->failed || impl_->written >= sequence; });
      return !impl_->failed;
    }
    catch (...)
    {
      return false;
    }
  }

  DiagnosticSession::DiagnosticSession(std::string_view component, const std::filesystem::path& directory) noexcept
  {
    try
    {
      log_ = std::make_unique<DiagnosticLog>(directory.empty() ? diagnostic_directory() : directory, component);
      current_log.store(log_.get(), std::memory_order_release);
      log_event("process.start", "component=", component);
    }
    catch (...)
    {
      unavailable();
    }
  }

  DiagnosticSession::~DiagnosticSession()
  {
    log_event("process.stop");
    current_log.store(nullptr, std::memory_order_release);
  }

  void write_diagnostic(std::string_view event, std::string_view details, LogDelivery delivery) noexcept
  {
    if (auto* log = current_log.load(std::memory_order_acquire))
      log->write(event, details, delivery);
  }
}
