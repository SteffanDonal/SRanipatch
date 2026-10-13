#include "sof/capture.hpp"
#include "sof/ipc.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
  std::uint32_t integer(std::string_view text)
  {
    std::uint32_t value {};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc {} || end != text.data() + text.size() || value == 0 || value > 100000)
      throw std::runtime_error("Invalid numeric capture option");
    return value;
  }

  void save_ppm(const std::filesystem::path& path, const sof::Frame& frame)
  {
    if (!path.parent_path().empty())
      std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << "P6\n"
         << frame.width << ' ' << frame.height << "\n255\n";
    file.write(reinterpret_cast<const char*>(frame.rgb.data()),
      static_cast<std::streamsize>(frame.rgb.size()));
    if (!file)
      throw std::runtime_error("Cannot save captured frame");
  }
}  // namespace

int main(int argc, char** argv)
{
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;
  try
  {
    std::uint32_t frames = 120;
    std::filesystem::path output;
    std::string ipc_name;
    for (int i = 1; i < argc; ++i)
    {
      const std::string_view key(argv[i]);
      if (key == "--help")
      {
        std::cout << "sof-capture [--ipc NAME] [--frames N] [--output frame.ppm]\n";
        return 0;
      }
      if (++i == argc)
        throw std::runtime_error("Missing capture option value");
      const std::string_view value(argv[i]);
      if (key == "--frames")
        frames = integer(value);
      else if (key == "--ipc")
        ipc_name = value;
      else if (key == "--output")
        output = value;
      else
        throw std::runtime_error("Unknown capture option");
    }
    std::unique_ptr<sof::Capture> capture;
    std::unique_ptr<sof::SharedMemory> ipc;
    if (ipc_name.empty())
      capture = sof::open_capture();
    else
      ipc = std::make_unique<sof::SharedMemory>(ipc_name, sof::SharedMemory::Role::client);
    sof::Frame frame;
    std::stop_source stop;
    std::uint64_t first {}, last {};
    for (std::uint32_t i = 0; i < frames;)
    {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (!(capture ? capture->read(frame, stop.get_token()) : ipc->read_frame(frame)))
      {
        if (std::chrono::steady_clock::now() >= deadline)
          throw std::runtime_error("Camera frame timed out");
        if (ipc)
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      if (i++ == 0)
        first = frame.timestamp_ns;
      last = frame.timestamp_ns;
    }
    const auto [minimum, maximum] = std::minmax_element(frame.rgb.begin(), frame.rgb.end());
    std::cout << "Captured " << frames << " frames, " << frame.width << 'x' << frame.height << " RGB, "
              << (last > first
                     ? static_cast<double>(frames - 1) * 1.0e9 / static_cast<double>(last - first)
                     : 0)
              << " fps, pixel range " << static_cast<int>(*minimum) << ".." << static_cast<int>(*maximum)
              << '\n';
    if (!output.empty())
    {
      save_ppm(output, frame);
      std::cout << "Saved " << output << '\n';
    }
    if (ipc)
    {
      sof::Status status;
      sof::Controls controls;
      if (ipc->read(status, controls))
        std::cout << "Server captured " << status.frames_captured << " frames; inference "
                  << (status.inference_enabled ? "enabled" : "disabled") << '\n';
    }
    if (capture)
    {
      capture.reset();
      std::cout << "Camera stopped cleanly\n";
    }
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << "Capture: " << error.what() << '\n';
    return 1;
  }
}
