#include "sof/dataset.hpp"

#include <array>
#include <limits>
#include <stdexcept>
#include <string>

namespace sof
{
  namespace
  {
    constexpr std::array<char, 8> magic { 'S', 'O', 'F', 'D', 'A', 'T', 'A', '\0' };
    constexpr std::uint32_t version = 1;
    constexpr std::uint32_t header_size = 32;

    template <typename Stream, typename Value>
    void write_little_endian(Stream& stream, Value value)
    {
      std::array<char, sizeof(Value)> bytes {};
      for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<char>((value >> (i * 8)) & 0xff);
      stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    template <typename Value>
    Value read_little_endian(std::istream& stream)
    {
      std::array<unsigned char, sizeof(Value)> bytes {};
      stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      if (!stream)
        throw std::runtime_error("Dataset is truncated");
      Value value {};
      for (std::size_t i = 0; i < bytes.size(); ++i)
        value |= static_cast<Value>(bytes[i]) << (i * 8);
      return value;
    }

    std::size_t pixel_count(std::uint32_t width, std::uint32_t height)
    {
      if (!width || !height || width > 16384 || height > 16384 || static_cast<std::uint64_t>(width) * height > std::numeric_limits<std::size_t>::max())
        throw std::runtime_error("Dataset has invalid frame dimensions");
      return static_cast<std::size_t>(width) * height;
    }
  }  // namespace

  DatasetWriter::DatasetWriter(const std::filesystem::path& path, std::uint32_t width,
    std::uint32_t height, DatasetPixels pixels)
    : width_(width), height_(height), pixels_(pixels)
  {
    pixel_count(width_, height_);
    if (!path.parent_path().empty())
      std::filesystem::create_directories(path.parent_path());
    file_.open(path, std::ios::binary | std::ios::trunc);
    if (!file_)
      throw std::runtime_error("Cannot create dataset: " + path.string());
    file_.write(magic.data(), static_cast<std::streamsize>(magic.size()));
    write_little_endian(file_, version);
    write_little_endian(file_, header_size);
    write_little_endian(file_, width_);
    write_little_endian(file_, height_);
    write_little_endian(file_, static_cast<std::uint32_t>(pixels_));
    write_little_endian(file_, std::uint32_t {});
    if (!file_)
      throw std::runtime_error("Cannot write dataset header");
  }

  void DatasetWriter::write(const Frame& frame)
  {
    if (frame.width != width_ || frame.height != height_ || frame.luma.size() != pixel_count(width_, height_))
      throw std::runtime_error("Dataset frame dimensions differ from its header");
    const auto& pixels = pixels_ == DatasetPixels::yuyv ? frame.yuyv : frame.luma;
    const auto expected = pixel_count(width_, height_) * (pixels_ == DatasetPixels::yuyv ? std::size_t { 2 } : std::size_t { 1 });
    if (pixels.size() != expected)
      throw std::runtime_error("Dataset frame is missing its configured pixel data");
    write_little_endian(file_, frame.sequence);
    write_little_endian(file_, frame.timestamp_ns);
    file_.write(reinterpret_cast<const char*>(pixels.data()),
      static_cast<std::streamsize>(pixels.size()));
    if (!file_)
      throw std::runtime_error("Cannot write dataset frame");
    ++frame_count_;
  }

  std::uint64_t DatasetWriter::frame_count() const noexcept
  {
    return frame_count_;
  }

  DatasetReader::DatasetReader(const std::filesystem::path& path)
  {
    file_.open(path, std::ios::binary);
    if (!file_)
      throw std::runtime_error("Cannot open dataset: " + path.string());
    std::array<char, magic.size()> actual {};
    file_.read(actual.data(), static_cast<std::streamsize>(actual.size()));
    if (!file_ || actual != magic)
      throw std::runtime_error("Dataset header has invalid magic");
    if (read_little_endian<std::uint32_t>(file_) != version || read_little_endian<std::uint32_t>(file_) != header_size)
      throw std::runtime_error("Dataset version is not supported");
    width_ = read_little_endian<std::uint32_t>(file_);
    height_ = read_little_endian<std::uint32_t>(file_);
    const auto pixels = read_little_endian<std::uint32_t>(file_);
    if (pixels != static_cast<std::uint32_t>(DatasetPixels::luma8) && pixels != static_cast<std::uint32_t>(DatasetPixels::yuyv))
      throw std::runtime_error("Dataset pixel format is not supported");
    pixels_ = static_cast<DatasetPixels>(pixels);
    read_little_endian<std::uint32_t>(file_);
    pixel_count(width_, height_);
  }

  bool DatasetReader::read(Frame& frame)
  {
    const auto next = file_.peek();
    if (next == std::char_traits<char>::eof())
    {
      if (file_.eof())
        return false;
      throw std::runtime_error("Cannot read dataset");
    }
    frame.sequence = read_little_endian<std::uint64_t>(file_);
    frame.timestamp_ns = read_little_endian<std::uint64_t>(file_);
    frame.width = width_;
    frame.height = height_;
    frame.luma.resize(pixel_count(width_, height_));
    if (pixels_ == DatasetPixels::yuyv)
    {
      frame.yuyv.resize(frame.luma.size() * 2);
      file_.read(reinterpret_cast<char*>(frame.yuyv.data()),
        static_cast<std::streamsize>(frame.yuyv.size()));
      for (std::size_t i = 0; i < frame.luma.size(); ++i)
        frame.luma[i] = frame.yuyv[i * 2];
    }
    else
    {
      frame.yuyv.clear();
      file_.read(reinterpret_cast<char*>(frame.luma.data()),
        static_cast<std::streamsize>(frame.luma.size()));
    }
    if (!file_)
      throw std::runtime_error("Dataset frame is truncated");
    return true;
  }

  std::uint32_t DatasetReader::width() const noexcept
  {
    return width_;
  }

  std::uint32_t DatasetReader::height() const noexcept
  {
    return height_;
  }
}  // namespace sof
