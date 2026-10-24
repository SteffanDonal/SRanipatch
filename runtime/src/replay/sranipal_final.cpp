#include "sof/dataset.hpp"
#include "sof/lip.hpp"
#include "sof/protocol.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace
{
  struct Options
  {
    std::filesystem::path dataset;
    std::filesystem::path output;
    std::filesystem::path raw_output;
    std::uint64_t frames {};
  };

  Options options(int argc, char** argv)
  {
    Options result;
    for (int i = 1; i < argc; ++i)
    {
      const std::string_view key(argv[i]);
      if (key == "--help")
      {
        std::cout << "sof-sranipal-final-replay --dataset capture.sofdata --output values.csv " "[--raw-output raw.csv] [--frames N]\n" "Place this executable in the SRanipal runtime directory.\n";
        std::exit(0);
      }
      if (++i == argc)
        throw std::runtime_error("Missing SRanipal final replay option value");
      const std::string_view value(argv[i]);
      if (key == "--dataset")
        result.dataset = value;
      else if (key == "--output")
        result.output = value;
      else if (key == "--raw-output")
        result.raw_output = value;
      else if (key == "--frames")
      {
        const auto [end, error] =
          std::from_chars(value.data(), value.data() + value.size(), result.frames);
        if (error != std::errc {} || end != value.data() + value.size() || !result.frames)
          throw std::runtime_error("SRanipal final replay frame count must be positive");
      }
      else
        throw std::runtime_error("Unknown SRanipal final replay option");
    }
    if (result.dataset.empty() || result.output.empty())
      throw std::runtime_error("SRanipal final replay requires --dataset and --output");
    return result;
  }

  std::string windows_error(std::string_view operation)
  {
    return std::string(operation) + " failed with Windows error " + std::to_string(GetLastError());
  }

  class Module
  {
  public:
    explicit Module(const std::filesystem::path& path)
    {
      value_ = LoadLibraryExW(path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
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

    std::byte* base() const noexcept
    {
      return reinterpret_cast<std::byte*>(value_);
    }

  private:
    HMODULE value_ {};
  };

  using Postprocess = void (*)(void*, float*, void*);
  Postprocess original_postprocess {};
  std::array<float, sof::lip_raw_count> captured_raw {};

  void capture_postprocess(void* state, float* output, void* raw_vector)
  {
    const auto* raw = *static_cast<const float**>(raw_vector);
    std::copy_n(raw, captured_raw.size(), captured_raw.begin());
    original_postprocess(state, output, raw_vector);
  }

  class RawCaptureHook
  {
  public:
    explicit RawCaptureHook(Module& module)
    {
      if (original_postprocess)
        throw std::runtime_error("SRanipal raw capture hook is already active");
      auto* call = module.base() + 0x7EBA;
      if (call[0] != std::byte { 0xE8 })
        throw std::runtime_error("LPredictionModule_v2 postprocessor call signature changed");
      std::int32_t old_displacement {};
      std::memcpy(&old_displacement, call + 1, sizeof(old_displacement));
      auto* target = call + 5 + old_displacement;
      if (target != module.base() + 0x7390)
        throw std::runtime_error("LPredictionModule_v2 postprocessor location changed");
      original_postprocess = reinterpret_cast<Postprocess>(target);

      SYSTEM_INFO system_info {};
      GetSystemInfo(&system_info);
      const auto granularity = static_cast<std::uintptr_t>(system_info.dwAllocationGranularity);
      const auto base = reinterpret_cast<std::uintptr_t>(module.base());
      for (std::uintptr_t distance = granularity; distance < 0x70000000 && !thunk_;
        distance += granularity)
      {
        if (base <= std::numeric_limits<std::uintptr_t>::max() - distance)
          // NOLINTNEXTLINE(performance-no-int-to-ptr): VirtualAlloc probes numeric addresses near the hook target.
          thunk_ = VirtualAlloc(reinterpret_cast<void*>(base + distance), 4096,
            MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (!thunk_ && base >= distance)
          // NOLINTNEXTLINE(performance-no-int-to-ptr): VirtualAlloc probes numeric addresses near the hook target.
          thunk_ = VirtualAlloc(reinterpret_cast<void*>(base - distance), 4096,
            MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
      }
      if (!thunk_)
        throw std::runtime_error(windows_error("VirtualAlloc(SRanipal raw capture thunk)"));
      auto* code = static_cast<std::byte*>(thunk_);
      code[0] = std::byte { 0x48 };
      code[1] = std::byte { 0xB8 };
      const auto hook = reinterpret_cast<std::uintptr_t>(&capture_postprocess);
      std::memcpy(code + 2, &hook, sizeof(hook));
      code[10] = std::byte { 0xFF };
      code[11] = std::byte { 0xE0 };
      FlushInstructionCache(GetCurrentProcess(), code, 12);

      const auto displacement =
        reinterpret_cast<std::intptr_t>(code) - reinterpret_cast<std::intptr_t>(call + 5);
      if (displacement < std::numeric_limits<std::int32_t>::min() || displacement > std::numeric_limits<std::int32_t>::max())
        throw std::runtime_error("SRanipal raw capture hook is outside relative-call range");
      const auto replacement = static_cast<std::int32_t>(displacement);
      DWORD old_protection {};
      if (!VirtualProtect(call + 1, sizeof(replacement), PAGE_EXECUTE_READWRITE, &old_protection))
        throw std::runtime_error(windows_error("VirtualProtect(postprocessor call)"));
      std::memcpy(call + 1, &replacement, sizeof(replacement));
      DWORD ignored {};
      VirtualProtect(call + 1, sizeof(replacement), old_protection, &ignored);
      FlushInstructionCache(GetCurrentProcess(), call, 5);
      call_ = call;
      old_displacement_ = old_displacement;
    }

    ~RawCaptureHook()
    {
      if (!call_)
        return;
      DWORD old_protection {};
      if (VirtualProtect(call_ + 1, sizeof(old_displacement_), PAGE_EXECUTE_READWRITE, &old_protection))
      {
        std::memcpy(call_ + 1, &old_displacement_, sizeof(old_displacement_));
        DWORD ignored {};
        VirtualProtect(call_ + 1, sizeof(old_displacement_), old_protection, &ignored);
        FlushInstructionCache(GetCurrentProcess(), call_, 5);
      }
      original_postprocess = nullptr;
      if (thunk_)
        VirtualFree(thunk_, 0, MEM_RELEASE);
    }

  private:
    std::byte* call_ {};
    std::int32_t old_displacement_ {};
    void* thunk_ {};
  };

  struct MemoryElement
  {
    std::uint32_t index {};
    std::uint16_t type {};
    std::uint16_t reserved {};
    std::uint64_t size {};
    void* data {};
  };

  static_assert(sizeof(MemoryElement) == 24);

  class Elements
  {
  public:
    Elements(MemoryElement* elements, std::uint16_t count) : elements_(elements, elements + count)
    {
      storage_.resize(count);
      for (std::size_t i = 0; i < elements_.size(); ++i)
      {
        storage_[i].resize(static_cast<std::size_t>(elements_[i].size));
        elements_[i].data = storage_[i].data();
      }
    }

    MemoryElement* data() noexcept
    {
      return elements_.data();
    }

    MemoryElement& operator[](std::size_t index)
    {
      return elements_.at(index);
    }

  private:
    std::vector<MemoryElement> elements_;
    std::vector<std::vector<std::byte>> storage_;
  };
}  // namespace

int main(int argc, char** argv)
{
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;
  try
  {
    const auto args = options(argc, argv);
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS))
      throw std::runtime_error(windows_error("SetDefaultDllDirectories"));
    const auto runtime = std::filesystem::absolute(argv[0]).parent_path();
    Module prediction(runtime / "LPredictionModule.dll");
    std::optional<RawCaptureHook> raw_capture_hook;
    if (!args.raw_output.empty())
      raw_capture_hook.emplace(prediction);
    const auto create = prediction.function<void* (*)()>("CreateLipPredictionModule_v2");
    const auto initial = prediction.function<int (*)(void*)>("?Initial@LipPredictionModule_v2@@UEAAHXZ");
    const auto start = prediction.function<int (*)(void*)>("?Start@LipPredictionModule_v2@@UEAAHXZ");
    const auto stop = prediction.function<int (*)(void*)>("?Stop@LipPredictionModule_v2@@UEAAHXZ");
    using Define = void (*)(void*, MemoryElement**, std::uint16_t*);
    const auto define_inputs = prediction.function<Define>(
      "?DefineInputs@LipPredictionModule_v2@@UEAAXPEAPEAUMemoryElement@ViveSR@@PEAF@Z"
    );
    const auto define_outputs = prediction.function<Define>(
      "?DefineOutputs@LipPredictionModule_v2@@UEAAXPEAPEAUMemoryElement@ViveSR@@PEAF@Z"
    );
    const auto process = prediction.function<bool (*)(void*, MemoryElement*, MemoryElement*)>(
      "?Process@LipPredictionModule_v2@@UEAA_NPEAUMemoryElement@ViveSR@@0@Z"
    );

    auto* instance = create();
    if (!instance)
      throw std::runtime_error("CreateLipPredictionModule_v2 returned null");
    auto** vtable = *reinterpret_cast<void***>(instance);
    using Destroy = void (*)(void*, unsigned);

    struct Owner
    {
      void* value;
      Destroy destroy;

      ~Owner()
      {
        destroy(value, 1);
      }
    } owner { instance, reinterpret_cast<Destroy>(vtable[0]) };

    MemoryElement* input_definitions {};
    MemoryElement* output_definitions {};
    std::uint16_t input_count {}, output_count {};
    define_inputs(instance, &input_definitions, &input_count);
    define_outputs(instance, &output_definitions, &output_count);
    if (input_count != 3 || output_count != 4)
      throw std::runtime_error("LPredictionModule_v2 returned an unexpected memory contract");
    Elements inputs(input_definitions, input_count);
    Elements outputs(output_definitions, output_count);
    if (inputs[0].size != std::uint64_t { 800 } * 400 || outputs[0].size < (sof::lip_parameter_count + 1) * sizeof(float))
      throw std::runtime_error("LPredictionModule_v2 returned unexpected buffer sizes");
    if (initial(instance) != 0 || start(instance) != 0)
      throw std::runtime_error("LPredictionModule_v2 initialization failed");

    sof::DatasetReader dataset(args.dataset);
    if (dataset.width() != sof::capture_width || dataset.height() != sof::capture_height)
      throw std::runtime_error("SRanipal final replay requires a 400x400 dataset");
    if (!args.output.parent_path().empty())
      std::filesystem::create_directories(args.output.parent_path());
    std::ofstream output(args.output);
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<float>::max_digits10);
    if (!output)
      throw std::runtime_error("Cannot create SRanipal final replay output");
    output << "sequence,timestamp_ns,confidence";
    for (const auto name : sof::lip_parameter_names)
      output << ',' << name;
    std::ofstream raw_output;
    if (!args.raw_output.empty())
    {
      if (!args.raw_output.parent_path().empty())
        std::filesystem::create_directories(args.raw_output.parent_path());
      raw_output.open(args.raw_output);
      raw_output.imbue(std::locale::classic());
      raw_output << std::setprecision(std::numeric_limits<float>::max_digits10);
      if (!raw_output)
        throw std::runtime_error("Cannot create SRanipal raw capture output");
      raw_output << "sequence,timestamp_ns";
      for (std::size_t i = 0; i < captured_raw.size(); ++i)
        raw_output << ",raw_" << i;
    }

    sof::Frame frame;
    std::uint64_t count {};
    while (dataset.read(frame))
    {
      if (frame.yuyv.size() != inputs[0].size)
        throw std::runtime_error("SRanipal final replay requires packed YUYV frames");
      std::copy(frame.yuyv.begin(), frame.yuyv.end(), static_cast<std::uint8_t*>(inputs[0].data));
      *static_cast<std::uint32_t*>(inputs[1].data) = static_cast<std::uint32_t>(frame.sequence);
      *static_cast<std::uint32_t*>(inputs[2].data) = 1;
      if (!process(instance, inputs.data(), outputs.data()))
        throw std::runtime_error("LPredictionModule_v2 processing failed");
      const auto* values = static_cast<const float*>(outputs[0].data);
      output << '\n'
             << frame.sequence << ',' << frame.timestamp_ns << ',' << values[37];
      for (std::size_t i = 0; i < sof::lip_parameter_names.size(); ++i)
        output << ',' << values[i];
      if (raw_output.is_open())
      {
        raw_output << '\n'
                   << frame.sequence << ',' << frame.timestamp_ns;
        for (const auto value : captured_raw)
          raw_output << ',' << value;
      }
      ++count;
      if (args.frames && count == args.frames)
        break;
    }
    if (stop(instance) != 0)
      throw std::runtime_error("LPredictionModule_v2 stop failed");
    output << '\n';
    if (raw_output.is_open())
      raw_output << '\n';
    if (!output)
      throw std::runtime_error("Cannot write SRanipal final replay output");
    if (raw_output.is_open() && !raw_output)
      throw std::runtime_error("Cannot write SRanipal raw capture output");
    std::cout << "Replayed " << count << " frames through final SRanipal lip processing to "
              << args.output << '\n';
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << "SRanipal final replay: " << error.what() << '\n';
    return 1;
  }
}
