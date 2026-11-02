#pragma once

#include <filesystem>
#include <memory>
#include <sstream>
#include <string_view>
#include <utility>

namespace sranipatch
{
  enum class LogDelivery
  {
    buffered,
    durable,
    deferred
  };

  std::filesystem::path diagnostic_directory();

  struct LogRetention
  {
    std::size_t file_bytes { 4 * 1024 * 1024 };
    unsigned file_count { 5 };
  };

  class DiagnosticLog
  {
  public:
    DiagnosticLog(const std::filesystem::path& directory, std::string_view component,
      LogRetention retention = {}) noexcept;
    ~DiagnosticLog();
    DiagnosticLog(const DiagnosticLog&) = delete;
    DiagnosticLog& operator=(const DiagnosticLog&) = delete;
    bool write(std::string_view event, std::string_view details, LogDelivery delivery) noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
  };

  class DiagnosticSession
  {
  public:
    explicit DiagnosticSession(std::string_view component, const std::filesystem::path& directory = {}) noexcept;
    ~DiagnosticSession();
    DiagnosticSession(const DiagnosticSession&) = delete;
    DiagnosticSession& operator=(const DiagnosticSession&) = delete;

  private:
    std::unique_ptr<DiagnosticLog> log_;
  };

  void write_diagnostic(std::string_view event, std::string_view details, LogDelivery delivery) noexcept;

  template <typename... Values>
  void diagnostic(LogDelivery delivery, std::string_view event, const Values&... values) noexcept
  {
    try
    {
      std::ostringstream details;
      (details << ... << values);
      write_diagnostic(event, details.str(), delivery);
    }
    catch (...)
    {}
  }

  template <typename... Values>
  void log_event(std::string_view event, const Values&... values) noexcept
  {
    diagnostic(LogDelivery::durable, event, values...);
  }

  template <typename... Values>
  void log_summary(std::string_view event, const Values&... values) noexcept
  {
    diagnostic(LogDelivery::buffered, event, values...);
  }

  // Callbacks enqueue durable records; only the writer thread touches the filesystem.
  template <typename... Values>
  void log_callback(std::string_view event, const Values&... values) noexcept
  {
    diagnostic(LogDelivery::deferred, event, values...);
  }
}
