#include "sof/lip.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
  struct Options
  {
    std::filesystem::path input;
    std::filesystem::path output;
  };

  Options options(int argc, char** argv)
  {
    Options result;
    for (int i = 1; i < argc; ++i)
    {
      const std::string_view key(argv[i]);
      if (key == "--help")
      {
        std::cout << "sof-postprocess-replay --input raw.csv --output final.csv\n";
        std::exit(0);
      }
      if (++i == argc)
        throw std::runtime_error("Missing postprocess replay option value");
      if (key == "--input")
        result.input = argv[i];
      else if (key == "--output")
        result.output = argv[i];
      else
        throw std::runtime_error("Unknown postprocess replay option");
    }
    if (result.input.empty() || result.output.empty())
      throw std::runtime_error("Postprocess replay requires --input and --output");
    return result;
  }

  std::string_view field(std::string_view& line)
  {
    const auto comma = line.find(',');
    const auto result = line.substr(0, comma);
    line = comma == std::string_view::npos ? std::string_view {} : line.substr(comma + 1);
    return result;
  }

  template <typename Value>
  Value number(std::string_view text)
  {
    Value result {};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (error != std::errc {} || end != text.data() + text.size())
      throw std::runtime_error("Invalid numeric CSV field");
    return result;
  }
}  // namespace

int main(int argc, char** argv)
{
  try
  {
    const auto args = options(argc, argv);
    std::ifstream input(args.input);
    input.imbue(std::locale::classic());
    if (!input)
      throw std::runtime_error("Cannot open raw replay input");
    if (!args.output.parent_path().empty())
      std::filesystem::create_directories(args.output.parent_path());
    std::ofstream output(args.output);
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<float>::max_digits10);
    if (!output)
      throw std::runtime_error("Cannot create postprocessed replay output");

    std::string line;
    if (!std::getline(input, line))
      throw std::runtime_error("Raw replay input is empty");
    std::string expected = "sequence,timestamp_ns";
    for (std::size_t i = 0; i < sof::lip_raw_count; ++i)
      expected += ",raw_" + std::to_string(i);
    if (line != expected)
      throw std::runtime_error("Raw replay input has an unexpected header");

    output << "sequence,timestamp_ns";
    for (const auto name : sof::lip_parameter_names)
      output << ',' << name;

    sof::LipPostprocessor postprocessor;
    std::uint64_t count {};
    while (std::getline(input, line))
    {
      if (line.empty())
        continue;
      std::string_view remaining(line);
      const auto sequence = number<std::uint64_t>(field(remaining));
      const auto timestamp = number<std::uint64_t>(field(remaining));
      std::array<float, sof::lip_raw_count> raw {};
      for (auto& value : raw)
      {
        value = number<float>(field(remaining));
        if (!std::isfinite(value))
          throw std::runtime_error("Raw replay input contains a non-finite value");
      }
      if (!remaining.empty())
        throw std::runtime_error("Raw replay row has too many fields");
      const auto values = postprocessor.process(raw);
      output << '\n'
             << sequence << ',' << timestamp;
      for (const auto value : values)
        output << ',' << value;
      ++count;
    }
    output << '\n';
    if (!input.eof())
      throw std::runtime_error("Cannot read raw replay input");
    if (!output)
      throw std::runtime_error("Cannot write postprocessed replay output");
    std::cout << "Postprocessed " << count << " frames to " << args.output << '\n';
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << "Postprocess replay: " << error.what() << '\n';
    return 1;
  }
}
