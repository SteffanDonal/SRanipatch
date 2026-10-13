#include "sof/capture.hpp"
#include "sof/vive_tracker.hpp"

#include <fcntl.h>
#include <linux/usb/video.h>
#include <linux/uvcvideo.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace sof
{
  namespace
  {
    constexpr std::uint8_t vive_extension_unit = 4;
    constexpr std::uint8_t vive_extension_selector = 2;

    int call_ioctl(int fd, unsigned long request, void* argument)
    {
      int result;
      do
      {
        result = ioctl(fd, request, argument);
      }
      while (result < 0 && errno == EINTR);
      return result;
    }

    void checked_ioctl(int fd, unsigned long request, void* argument, const char* operation)
    {
      if (call_ioctl(fd, request, argument) < 0)
        throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
    }

    std::string read_sysfs_value(const std::filesystem::path& path)
    {
      std::ifstream input(path);
      std::string value;
      input >> value;
      return value;
    }

    bool is_vive_video_node(const std::filesystem::path& entry)
    {
      std::error_code error;
      auto current = std::filesystem::canonical(entry / "device", error);
      if (error)
        return false;
      while (current.has_parent_path() && current != current.root_path())
      {
        if (read_sysfs_value(current / "idVendor") == "0bb4" && read_sysfs_value(current / "idProduct") == "0321")
          return true;
        current = current.parent_path();
      }
      return false;
    }

    std::filesystem::path find_vive_video_device()
    {
      constexpr auto video_class = "/sys/class/video4linux";
      std::vector<std::filesystem::path> matches;
      std::error_code error;
      std::filesystem::directory_iterator entries(video_class, error);
      if (error)
        throw std::runtime_error("Cannot enumerate Linux video devices: " + error.message());

      for (const auto& entry : entries)
      {
        if (!is_vive_video_node(entry.path()))
          continue;
        const auto device = std::filesystem::path { "/dev" } / entry.path().filename();
        const int fd = open(device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
          throw std::runtime_error("Cannot open VIVE video node " + device.string() + ": " + std::strerror(errno));
        v4l2_capability caps {};
        const bool queried = call_ioctl(fd, VIDIOC_QUERYCAP, &caps) == 0;
        close(fd);
        if (!queried)
          continue;
        const auto capabilities =
          (caps.capabilities & V4L2_CAP_DEVICE_CAPS) ? caps.device_caps : caps.capabilities;
        if ((capabilities & V4L2_CAP_VIDEO_CAPTURE) && (capabilities & V4L2_CAP_STREAMING))
          matches.push_back(device);
      }
      if (matches.size() != 1)
        throw std::runtime_error(
          "Expected exactly one VIVE Facial Tracker capture node (USB 0BB4:0321); found " + std::to_string(matches.size())
        );
      return matches.front();
    }

    class UvcExtension final : public TrackerExtension
    {
      int fd_;
      std::size_t size_;

      void query(std::uint8_t request, std::uint8_t* data, std::size_t size, const char* operation)
      {
        if (size > UINT16_MAX)
          throw std::runtime_error("VIVE extension payload is too large");
        uvc_xu_control_query control { vive_extension_unit, vive_extension_selector, request,
          static_cast<std::uint16_t>(size), data };
        checked_ioctl(fd_, UVCIOC_CTRL_QUERY, &control, operation);
      }

    public:
      explicit UvcExtension(int fd) : fd_(fd)
      {
        std::uint16_t size {};
        query(UVC_GET_LEN, reinterpret_cast<std::uint8_t*>(&size), sizeof(size), "VIVE GET_LEN");
        size_ = size;
        std::cout << "VIVE extension: unit 4, selector 2, " << size_ << " bytes\n";
      }

      std::size_t buffer_size() const override
      {
        return size_;
      }

      void get(std::span<std::uint8_t> data) override
      {
        query(UVC_GET_CUR, data.data(), data.size(), "VIVE GET_CUR");
      }

      void set(std::span<const std::uint8_t> data) override
      {
        query(UVC_SET_CUR, const_cast<std::uint8_t*>(data.data()), data.size(), "VIVE SET_CUR");
      }
    };

    class V4l2 final : public Capture
    {
      struct Buffer
      {
        void* data { MAP_FAILED };
        std::size_t size {};
      };

      int fd_ { -1 };
      std::unique_ptr<UvcExtension> extension_;
      std::vector<Buffer> buffers_;
      std::uint32_t width_ {}, height_ {}, stride_ {};
      std::uint64_t sequence_ {};
      std::uint64_t dropped_ {};
      bool streaming_ {};

      void dropped_frame()
      {
        ++dropped_;
        if (dropped_ == 1 || dropped_ % capture_fps == 0)
          std::cerr << "V4L2: discarded " << dropped_ << " invalid or incomplete frame(s)\n";
      }

      void close_device() noexcept
      {
        if (streaming_)
        {
          auto type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
          call_ioctl(fd_, VIDIOC_STREAMOFF, &type);
        }
        for (const auto& buffer : buffers_)
          if (buffer.data != MAP_FAILED)
            munmap(buffer.data, buffer.size);
        if (extension_)
        {
          try
          {
            disable_vive_tracker(*extension_);
          }
          catch (const std::exception& error)
          {
            std::cerr << "VIVE shutdown: " << error.what() << '\n';
          }
          extension_.reset();
        }
        if (fd_ >= 0)
          close(fd_);
      }

    public:
      V4l2()
      {
        try
        {
          const auto device = find_vive_video_device();
          fd_ = open(device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
          if (fd_ < 0)
            throw std::runtime_error("Cannot open V4L2 device: " + std::string(std::strerror(errno)));
          v4l2_capability caps {};
          checked_ioctl(fd_, VIDIOC_QUERYCAP, &caps, "VIDIOC_QUERYCAP");
          const auto capabilities =
            (caps.capabilities & V4L2_CAP_DEVICE_CAPS) ? caps.device_caps : caps.capabilities;
          if (!(capabilities & V4L2_CAP_VIDEO_CAPTURE) || !(capabilities & V4L2_CAP_STREAMING))
            throw std::runtime_error("Camera must support single-plane V4L2 streaming capture");
          std::cout << "Opening " << caps.card << " at " << device << '\n';
          extension_ = std::make_unique<UvcExtension>(fd_);
          enable_vive_tracker(*extension_);
          std::cout << "VIVE sensor registers configured; stream enabled\n";
          v4l2_format format {};
          format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
          format.fmt.pix.width = capture_width;
          format.fmt.pix.height = capture_height;
          format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
          format.fmt.pix.field = V4L2_FIELD_NONE;
          checked_ioctl(fd_, VIDIOC_S_FMT, &format, "VIDIOC_S_FMT");
          if (format.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV)
            throw std::runtime_error("Camera does not offer YUYV; add a decoder for its native format");
          width_ = format.fmt.pix.width;
          height_ = format.fmt.pix.height;
          stride_ = format.fmt.pix.bytesperline;
          if (width_ != capture_width || height_ != capture_height || stride_ < width_ * 2)
            throw std::runtime_error("VIVE camera did not accept 400x400 YUYV");
          v4l2_streamparm rate {};
          rate.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
          rate.parm.capture.timeperframe.numerator = 1;
          rate.parm.capture.timeperframe.denominator = capture_fps;
          checked_ioctl(fd_, VIDIOC_S_PARM, &rate, "VIDIOC_S_PARM");
          const auto& interval = rate.parm.capture.timeperframe;
          if (!interval.numerator || interval.denominator != capture_fps * interval.numerator)
            throw std::runtime_error("VIVE camera did not accept 60 fps");
          v4l2_requestbuffers request {};
          request.count = 4;
          request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
          request.memory = V4L2_MEMORY_MMAP;
          checked_ioctl(fd_, VIDIOC_REQBUFS, &request, "VIDIOC_REQBUFS");
          if (request.count < 2)
            throw std::runtime_error("Insufficient V4L2 buffers");
          buffers_.resize(request.count);
          for (std::uint32_t i = 0; i < request.count; ++i)
          {
            v4l2_buffer buffer {};
            buffer.type = request.type;
            buffer.memory = request.memory;
            buffer.index = i;
            checked_ioctl(fd_, VIDIOC_QUERYBUF, &buffer, "VIDIOC_QUERYBUF");
            buffers_[i].size = buffer.length;
            buffers_[i].data =
              mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buffer.m.offset);
            if (buffers_[i].data == MAP_FAILED)
              throw std::runtime_error("V4L2 mmap failed");
            checked_ioctl(fd_, VIDIOC_QBUF, &buffer, "VIDIOC_QBUF");
          }
          auto type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
          checked_ioctl(fd_, VIDIOC_STREAMON, &type, "VIDIOC_STREAMON");
          streaming_ = true;
        }
        catch (...)
        {
          close_device();
          throw;
        }
      }

      ~V4l2() override
      {
        close_device();
      }

      bool read(Frame& frame, std::stop_token stop) override
      {
        if (stop.stop_requested())
          return false;
        pollfd ready { fd_, POLLIN, 0 };
        const int result = poll(&ready, 1, 200);
        if (result < 0 && errno == EINTR)
          return false;
        if (result < 0 || (ready.revents & (POLLERR | POLLHUP | POLLNVAL)))
          throw std::runtime_error("V4L2 camera disconnected / poll failed");
        if (result == 0)
          return false;
        v4l2_buffer buffer {};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        if (call_ioctl(fd_, VIDIOC_DQBUF, &buffer) < 0)
        {
          if (errno == EAGAIN)
            return false;
          throw std::runtime_error("VIDIOC_DQBUF failed");
        }
        if (buffer.index >= buffers_.size())
          throw std::runtime_error("V4L2 returned an invalid buffer index");
        const auto needed = static_cast<std::size_t>(height_ - 1) * stride_ + width_ * 2;
        frame.sequence = ++sequence_;
        if (buffer.bytesused < needed || buffer.bytesused > buffers_[buffer.index].size || (buffer.flags & V4L2_BUF_FLAG_ERROR))
        {
          dropped_frame();
          checked_ioctl(fd_, VIDIOC_QBUF, &buffer, "VIDIOC_QBUF");
          return false;
        }
        try
        {
          frame.width = width_;
          frame.height = height_;
          frame.timestamp_ns = monotonic_ns();
          yuyv_luma_to_rgb(static_cast<const std::uint8_t*>(buffers_[buffer.index].data), stride_, frame);
        }
        catch (...)
        {
          call_ioctl(fd_, VIDIOC_QBUF, &buffer);
          throw;
        }
        checked_ioctl(fd_, VIDIOC_QBUF, &buffer, "VIDIOC_QBUF");
        return true;
      }
    };
  }  // namespace

  std::unique_ptr<Capture> open_capture()
  {
    return std::make_unique<V4l2>();
  }
}  // namespace sof
