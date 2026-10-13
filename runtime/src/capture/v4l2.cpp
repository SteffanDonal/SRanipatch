#include "sof/capture.hpp"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace sof
{
  namespace
  {
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

    class V4l2 final : public Capture
    {
      struct Buffer
      {
        void* data { MAP_FAILED };
        std::size_t size {};
      };

      int fd_ { -1 };
      std::vector<Buffer> buffers_;
      std::uint32_t width_ {}, height_ {}, stride_ {};
      std::uint64_t sequence_ {};
      bool streaming_ {};

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
        if (fd_ >= 0)
          close(fd_);
      }

    public:
      explicit V4l2(const Config& config)
      {
        try
        {
          fd_ = open(config.device.data(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
          if (fd_ < 0)
            throw std::runtime_error("Cannot open V4L2 device: " + std::string(std::strerror(errno)));
          v4l2_capability caps {};
          checked_ioctl(fd_, VIDIOC_QUERYCAP, &caps, "VIDIOC_QUERYCAP");
          const auto capabilities =
            (caps.capabilities & V4L2_CAP_DEVICE_CAPS) ? caps.device_caps : caps.capabilities;
          if (!(capabilities & V4L2_CAP_VIDEO_CAPTURE) || !(capabilities & V4L2_CAP_STREAMING))
            throw std::runtime_error("Camera must support single-plane V4L2 streaming capture");
          v4l2_format format {};
          format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
          format.fmt.pix.width = config.width;
          format.fmt.pix.height = config.height;
          format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
          format.fmt.pix.field = V4L2_FIELD_NONE;
          checked_ioctl(fd_, VIDIOC_S_FMT, &format, "VIDIOC_S_FMT");
          if (format.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV)
            throw std::runtime_error("Camera does not offer YUYV; add a decoder for its native format");
          width_ = format.fmt.pix.width;
          height_ = format.fmt.pix.height;
          stride_ = format.fmt.pix.bytesperline;
          if (width_ != config.width || height_ != config.height || width_ % 2 || stride_ < width_ * 2)
            throw std::runtime_error("Camera did not accept the requested dimensions / YUYV stride");
          v4l2_streamparm rate {};
          rate.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
          rate.parm.capture.timeperframe.numerator = 1;
          rate.parm.capture.timeperframe.denominator = config.fps;
          checked_ioctl(fd_, VIDIOC_S_PARM, &rate, "VIDIOC_S_PARM");
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
        try
        {
          const auto needed = static_cast<std::size_t>(height_ - 1) * stride_ + width_ * 2;
          if (buffer.index >= buffers_.size() || buffer.bytesused < needed || buffer.bytesused > buffers_[buffer.index].size || (buffer.flags & V4L2_BUF_FLAG_ERROR))
            throw std::runtime_error("Invalid / incomplete V4L2 frame");
          frame.width = width_;
          frame.height = height_;
          frame.timestamp_ns = monotonic_ns();
          frame.sequence = ++sequence_;
          yuyv_to_rgb(static_cast<const std::uint8_t*>(buffers_[buffer.index].data), stride_, frame);
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

  std::unique_ptr<Capture> open_capture(const Config& config)
  {
    return std::make_unique<V4l2>(config);
  }
}  // namespace sof
