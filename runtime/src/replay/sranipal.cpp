#include "sof/dataset.hpp"
#include "sof/lip.hpp"
#include "sof/protocol.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <locale>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace
{
  struct Options
  {
    std::filesystem::path runtime;
    std::filesystem::path dataset;
    std::filesystem::path output;
    bool swap_cameras {};
  };

  Options options(int argc, char** argv)
  {
    Options result;
    for (int i = 1; i < argc; ++i)
    {
      const std::string_view key(argv[i]);
      if (key == "--help")
      {
        std::cout << "sof-sranipal-replay --runtime SRanipal_latest --dataset capture.sofdata " "--output raw.csv [--swap-cameras]\n";
        std::exit(0);
      }
      if (key == "--swap-cameras")
      {
        result.swap_cameras = true;
        continue;
      }
      if (++i == argc)
        throw std::runtime_error("Missing SRanipal replay option value");
      const std::string_view value(argv[i]);
      if (key == "--runtime")
        result.runtime = value;
      else if (key == "--dataset")
        result.dataset = value;
      else if (key == "--output")
        result.output = value;
      else
        throw std::runtime_error("Unknown SRanipal replay option");
    }
    if (result.runtime.empty() || result.dataset.empty() || result.output.empty())
      throw std::runtime_error("SRanipal replay requires --runtime, --dataset and --output");
    return result;
  }

  std::string windows_error(std::string_view operation)
  {
    return std::string(operation) + " failed with Windows error " + std::to_string(GetLastError());
  }

  class DllDirectories
  {
  public:
    explicit DllDirectories(const std::filesystem::path& runtime)
    {
      if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS))
        throw std::runtime_error(windows_error("SetDefaultDllDirectories"));
      add(runtime);
      add(runtime / "model" / "LPrediction");
    }

    ~DllDirectories()
    {
      for (const auto cookie : cookies_)
        RemoveDllDirectory(cookie);
    }

  private:
    std::vector<DLL_DIRECTORY_COOKIE> cookies_;

    void add(const std::filesystem::path& path)
    {
      const auto cookie = AddDllDirectory(path.c_str());
      if (!cookie)
        throw std::runtime_error(windows_error("AddDllDirectory"));
      cookies_.push_back(cookie);
    }
  };

  class Module
  {
  public:
    explicit Module(const std::filesystem::path& path)
    {
      value_ = LoadLibraryExW(path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
      if (!value_)
        throw std::runtime_error(windows_error("LoadLibraryExW(" + path.string() + ")"));
    }

    ~Module()
    {
      if (value_)
        FreeLibrary(value_);
    }

    template <typename Function>
    Function function(const char* name) const
    {
      const auto address = GetProcAddress(value_, name);
      if (!address)
        throw std::runtime_error(windows_error(std::string("GetProcAddress(") + name + ")"));
      return reinterpret_cast<Function>(address);
    }

  private:
    HMODULE value_ {};
  };

  class Mat
  {
  public:
    using Construct = void* (*)(void*, int, int, int, void*, std::size_t);
    using Destroy = void (*)(void*);

    Mat(Construct construct, Destroy destroy, int rows, int columns, std::uint8_t* data,
      std::size_t step)
      : destroy_(destroy)
    {
      construct(storage_.data(), rows, columns, 0, data, step);
    }

    ~Mat()
    {
      destroy_(storage_.data());
    }

    void* get() noexcept
    {
      return storage_.data();
    }

  private:
    alignas(16) std::array<std::byte, 128> storage_ {};
    Destroy destroy_;
  };
}  // namespace

int main(int argc, char** argv)
{
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;
  try
  {
    const auto args = options(argc, argv);
    DllDirectories directories(args.runtime);
    std::cout << "Loading original SRanipal libraries\n";
    Module predictor_module(args.runtime / "LipPredictionTVM.dll");
    Module opencv_module(args.runtime / "opencv_world310.dll");
    const auto create = predictor_module.function<void* (*)()>("CreateLipPrediction");
    const auto mat_construct = opencv_module.function<Mat::Construct>(
      "??0Mat@cv@@QEAA@HHHPEAX_K@Z"
    );
    const auto mat_destroy =
      opencv_module.function<Mat::Destroy>("??1Mat@cv@@QEAA@XZ");
    auto* predictor = create();
    if (!predictor)
      throw std::runtime_error("CreateLipPrediction returned null");
    std::cout << "Created original lip predictor\n";
    auto** vtable = *reinterpret_cast<void***>(predictor);
    using DestroyPredictor = void (*)(void*, unsigned);

    struct PredictorOwner
    {
      void* value;
      DestroyPredictor destroy;

      ~PredictorOwner()
      {
        destroy(value, 1);
      }
    } owner { predictor, reinterpret_cast<DestroyPredictor>(vtable[0]) };

    using Initialize = void (*)(void*, const char*, const char*);
    using Predict = void (*)(void*, void*, void*, std::vector<float>*);
    const auto initialize = reinterpret_cast<Initialize>(vtable[1]);
    const auto predict = reinterpret_cast<Predict>(vtable[2]);
    const auto model = args.runtime / "model" / "LPrediction";
    const auto graph = (model / "lip_opencl.json").string();
    const auto parameters = (model / "lip_opencl.params").string();
    std::cout << "Initializing original lip predictor\n";
    initialize(predictor, graph.c_str(), parameters.c_str());
    std::cout << "Initialized original lip predictor\n";

    sof::DatasetReader dataset(args.dataset);
    if (dataset.width() != sof::capture_width || dataset.height() != sof::capture_height)
      throw std::runtime_error("SRanipal replay requires 400x400 dataset frames");
    if (!args.output.parent_path().empty())
      std::filesystem::create_directories(args.output.parent_path());
    std::ofstream output(args.output);
    output.imbue(std::locale::classic());
    if (!output)
      throw std::runtime_error("Cannot create SRanipal replay output");
    output << "sequence,timestamp_ns";
    for (std::size_t i = 0; i < sof::lip_raw_count; ++i)
      output << ",raw_" << i;
    sof::Frame frame;
    std::uint64_t count {};
    while (dataset.read(frame))
    {
      std::vector<std::uint8_t> expanded;
      auto* stereo = frame.yuyv.data();
      if (frame.yuyv.empty())
      {
        expanded.resize(frame.luma.size() * 2);
        for (std::size_t i = 0; i < frame.luma.size(); ++i)
        {
          expanded[i * 2] = frame.luma[i];
          expanded[i * 2 + 1] = frame.luma[i];
        }
        stereo = expanded.data();
      }
      Mat image(mat_construct, mat_destroy, static_cast<int>(frame.height),
        static_cast<int>(frame.width * 2), stereo, static_cast<std::size_t>(frame.width) * 2);

      struct Rect
      {
        int x, y, width, height;
      };

      std::vector<Rect> regions { { 0, 0, 400, 400 }, { 400, 0, 400, 400 } };
      if (args.swap_cameras)
        std::swap(regions[0], regions[1]);
      std::vector<float> raw;
      if (!count)
        std::cout << "Running first original inference\n";
      predict(predictor, image.get(), &regions, &raw);
      if (!count)
        std::cout << "First original inference returned " << raw.size() << " values\n";
      if (raw.size() != sof::lip_raw_count)
        throw std::runtime_error("Original SRanipal predictor returned " + std::to_string(raw.size()) + " values instead of 30");
      output << '\n'
             << frame.sequence << ',' << frame.timestamp_ns;
      for (const auto value : raw)
        output << ',' << value;
      ++count;
    }
    output << '\n';
    if (!output)
      throw std::runtime_error("Cannot write SRanipal replay output");
    std::cout << "Replayed " << count << " frames through original SRanipal to " << args.output
              << '\n';
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << "SRanipal replay: " << error.what() << '\n';
    return 1;
  }
}
