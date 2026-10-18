#include "sof/dataset.hpp"
#include "sof/inference.hpp"
#include "sof/lip.hpp"

#include <array>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <stdexcept>
#include <string_view>

namespace
{
  struct Options
  {
    std::filesystem::path dataset;
    std::filesystem::path output;
    std::string model { "../assets/models/lip_model.mnn" };
    sof::InferencePrecision precision { sof::InferencePrecision::normal };
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
        std::cout << "sof-replay --dataset capture.sofdata --output values.csv " "[--model lip_model.mnn] [--precision normal|low] [--frames N]\n";
        std::exit(0);
      }
      if (++i == argc)
        throw std::runtime_error("Missing replay option value");
      const std::string_view value(argv[i]);
      if (key == "--dataset")
        result.dataset = value;
      else if (key == "--output")
        result.output = value;
      else if (key == "--model")
        result.model = value;
      else if (key == "--frames")
      {
        const auto [end, error] =
          std::from_chars(value.data(), value.data() + value.size(), result.frames);
        if (error != std::errc {} || end != value.data() + value.size() || !result.frames)
          throw std::runtime_error("Replay frame count must be positive");
      }
      else if (key == "--precision")
      {
        if (value == "normal")
          result.precision = sof::InferencePrecision::normal;
        else if (value == "low")
          result.precision = sof::InferencePrecision::low;
        else
          throw std::runtime_error("Replay precision must be normal or low");
      }
      else
        throw std::runtime_error("Unknown replay option");
    }
    if (result.dataset.empty() || result.output.empty())
      throw std::runtime_error("Replay requires --dataset and --output");
    return result;
  }
}  // namespace

int main(int argc, char** argv)
{
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;
  try
  {
    const auto args = options(argc, argv);
    sof::DatasetReader dataset(args.dataset);
    if (dataset.width() != sof::capture_width || dataset.height() != sof::capture_height)
      throw std::runtime_error("Lip replay requires 400x400 dataset frames");
    sof::Config config;
    sof::set_text(config.model, args.model);
    auto inference = sof::open_lip_mnn(config, args.precision);
    if (!args.output.parent_path().empty())
      std::filesystem::create_directories(args.output.parent_path());
    std::ofstream output(args.output);
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<float>::max_digits10);
    if (!output)
      throw std::runtime_error("Cannot create replay output");
    output << "sequence,timestamp_ns";
    std::array<sof::Parameter, sof::max_parameters> parameters {};
    std::array<float, sof::lip_raw_count> raw {};
    sof::Frame frame;
    std::uint64_t count {};
    while (dataset.read(frame))
    {
      const auto parameter_count = inference->run(frame, parameters, raw);
      if (!count)
      {
        for (std::size_t i = 0; i < raw.size(); ++i)
          output << ",raw_" << i;
        for (std::uint32_t i = 0; i < parameter_count; ++i)
          output << ',' << parameters[i].name.data();
      }
      output << '\n'
             << frame.sequence << ',' << frame.timestamp_ns;
      for (const auto value : raw)
        output << ',' << value;
      for (std::uint32_t i = 0; i < parameter_count; ++i)
        output << ',' << parameters[i].value;
      ++count;
      if (args.frames && count == args.frames)
        break;
    }
    output << '\n';
    if (!output)
      throw std::runtime_error("Cannot write replay output");
    std::cout << "Replayed " << count << " frames to " << args.output << '\n';
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << "Replay: " << error.what() << '\n';
    return 1;
  }
}
