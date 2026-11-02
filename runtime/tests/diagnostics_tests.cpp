#include "sranipatch/diagnostics.hpp"

#include "sranipatch/process.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>

namespace
{
  using namespace std::chrono_literals;

  void require(bool value, const char* message)
  {
    if (!value)
      throw std::runtime_error(message);
  }

  std::string read(const std::filesystem::path& path)
  {
    std::ifstream file(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
  }

  void wait_for(sranipatch::ChildProcess& child)
  {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!child.poll() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(10ms);
    const auto exit = child.poll();
    require(exit && *exit == 0, "Diagnostic writer child failed or timed out");
  }

  void persistence(const std::filesystem::path& directory)
  {
    {
      sranipatch::DiagnosticLog log(directory, "server");
      require(log.write("critical", "session=42 error=0x80070005", sranipatch::LogDelivery::durable), "Critical write failed");
      const auto text = read(directory / "server.log");
      require(text.find("Z pid=") != std::string::npos && text.find("event=critical session=42") != std::string::npos,
        "Critical event was not visible before logger shutdown");
      require(log.write("callback", "queued", sranipatch::LogDelivery::deferred), "Deferred write failed");
      require(log.write("summary", "buffered", sranipatch::LogDelivery::buffered), "Buffered write failed");
    }
    const auto text = read(directory / "server.log");
    require(text.find("event=callback queued") != std::string::npos, "Shutdown lost deferred callback record");
    require(text.find("event=summary buffered") != std::string::npos, "Shutdown lost buffered summary");
    {
      sranipatch::DiagnosticLog log(directory, "server");
      require(log.write("restart", "session=43", sranipatch::LogDelivery::durable), "Restart write failed");
    }
    const auto restarted = read(directory / "server.log");
    require(restarted.starts_with(text) && restarted.find("event=restart session=43") != std::string::npos,
      "Restart discarded the preceding session");
  }

  void rotation(const std::filesystem::path& directory)
  {
    for (int session = 0; session < 3; ++session)
    {
      sranipatch::DiagnosticLog log(directory, "rotation", { .file_bytes = 512, .file_count = 3 });
      for (int i = 0; i < 30; ++i)
        require(log.write("rotate", std::string(100, 'x'), sranipatch::LogDelivery::durable), "Rotation write failed");
      require(log.write("last", "session=" + std::to_string(session), sranipatch::LogDelivery::durable), "Final rotation write failed");
    }
    std::uintmax_t total {};
    unsigned files {};
    for (const auto& entry : std::filesystem::directory_iterator(directory))
    {
      if (entry.path().filename().string().starts_with("rotation.log"))
      {
        ++files;
        require(entry.file_size() <= 512, "Rotated file exceeded size limit");
        total += entry.file_size();
        const auto text = read(entry.path());
        require(!text.empty() && text.back() == '\n', "Rotation split a record");
      }
    }
    require(files == 3 && total <= 1536, "Retention exceeded its budget across restarts");
    require(read(directory / "rotation.log").find("event=last session=2") != std::string::npos,
      "Rotation lost newest evidence");
  }

  void failures(const std::filesystem::path& directory)
  {
    const auto obstruction = directory / "not-a-directory";
    std::ofstream(obstruction) << "file";
    sranipatch::DiagnosticLog unavailable(obstruction, "server");
    require(!unavailable.write("error", "still running", sranipatch::LogDelivery::durable), "Invalid destination unexpectedly worked");
    const auto broken = directory / "broken";
    sranipatch::DiagnosticLog log(broken, "server");
    require(log.write("before", "", sranipatch::LogDelivery::durable), "Initial write failed");
    std::filesystem::remove(broken / "server.log");
    std::filesystem::create_directory(broken / "server.log");
    require(!log.write("after", "", sranipatch::LogDelivery::durable), "Writer failure did not release durable caller");
    require(!log.write("later", "", sranipatch::LogDelivery::deferred), "Failed writer accepted callback work");
  }

  void text_and_session(const std::filesystem::path& directory)
  {
    sranipatch::DiagnosticSession session("session", directory);
    sranipatch::log_event("fault", "error=first\nforged\r\t", std::string(5000, 'z'));
    const auto text = read(directory / "session.log");
    require(text.find("event=process.start component=session") != std::string::npos, "Global session did not initialize");
    require(text.find("error=first forged  ") != std::string::npos, "Multiline errors can forge records");
    require(text.find("[truncated]") != std::string::npos, "Oversized diagnostic was not bounded");
  }

  void multiprocess(const std::filesystem::path& directory, const std::filesystem::path& executable)
  {
    sranipatch::ChildProcess first, second;
    first.start(executable, { "--writer", directory.string() });
    second.start(executable, { "--writer", directory.string() });
    wait_for(first);
    wait_for(second);
    std::istringstream input(read(directory / "shared.log"));
    std::string line;
    unsigned records {};
    while (std::getline(input, line))
    {
      require(line.find("event=concurrent value=") != std::string::npos && line.ends_with(std::string(100, 'a')),
        "Concurrent processes interleaved or damaged a record");
      ++records;
    }
    require(records == 80, "Concurrent processes lost records");

    sranipatch::ChildProcess abrupt;
    abrupt.start(executable, { "--abrupt", directory.string() });
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (read(directory / "abrupt.log").find("event=before_termination") == std::string::npos
      && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(10ms);
    abrupt.stop(0ms);
    require(read(directory / "abrupt.log").find("event=before_termination") != std::string::npos,
      "Forced process termination lost durable evidence");
  }
}

// Exceptions from the body are caught below; clang-tidy misidentifies MSVC exception inheritance.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char** argv)
{
  try
  {
    if (argc >= 3 && std::string_view(argv[1]) == "--writer")
    {
      sranipatch::DiagnosticLog log(argv[2], "shared");
      for (int i = 0; i < 40; ++i)
        require(log.write("concurrent", "value=" + std::string(100, 'a'), sranipatch::LogDelivery::durable), "Child write failed");
      return 0;
    }
    if (argc >= 3 && std::string_view(argv[1]) == "--abrupt")
    {
      sranipatch::DiagnosticLog log(argv[2], "abrupt");
      require(log.write("before_termination", "", sranipatch::LogDelivery::durable), "Child durable write failed");
      std::this_thread::sleep_for(20s);
      return 1;
    }
    const auto directory = std::filesystem::temp_directory_path()
      / ("sranipatch-diagnostics-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    persistence(directory);
    rotation(directory);
    failures(directory);
    text_and_session(directory);
    multiprocess(directory, std::filesystem::absolute(argv[0]));
    // Only this test's newly created directory is removed.
    std::filesystem::remove_all(directory);
    std::cout << "Diagnostic logging tests passed\n";
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
