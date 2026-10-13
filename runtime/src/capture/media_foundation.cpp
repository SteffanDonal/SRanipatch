#include "sof/capture.hpp"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl.h>

#include <charconv>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

namespace sof
{
  namespace
  {
    using Microsoft::WRL::ComPtr;
    constexpr DWORD video_stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    constexpr DWORD all_streams = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);

    void check(HRESULT result, const char* operation)
    {
      if (FAILED(result))
        throw std::runtime_error(std::string(operation) + " failed (HRESULT " + std::to_string(static_cast<unsigned long>(result)) + ")");
    }

    struct SampleState
    {
      std::mutex mutex;
      std::condition_variable_any ready;
      ComPtr<IMFSample> sample;
      HRESULT result { S_OK };
      DWORD flags {};
      bool complete {};
      std::uint64_t timestamp_ns {};
    };

    class ReaderCallback final
      : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          IMFSourceReaderCallback>
    {
    public:
      std::shared_ptr<SampleState> state;

      STDMETHODIMP OnReadSample(HRESULT result, DWORD, DWORD flags, LONGLONG, IMFSample* sample) override
      {
        {
          std::lock_guard lock(state->mutex);
          state->result = result;
          state->flags = flags;
          state->sample = sample;
          state->timestamp_ns = monotonic_ns();
          state->complete = true;
        }
        state->ready.notify_one();
        return S_OK;
      }

      STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override
      {
        return S_OK;
      }

      STDMETHODIMP OnFlush(DWORD) override
      {
        return S_OK;
      }
    };

    struct Devices
    {
      IMFActivate** items {};
      UINT32 count {};

      ~Devices()
      {
        for (UINT32 i = 0; i < count; ++i)
          items[i]->Release();
        CoTaskMemFree(items);
      }
    };

    class MediaFoundation final : public Capture
    {
      bool com_ {}, mf_ {};
      ComPtr<IMFMediaSource> source_;
      ComPtr<IMFSourceReader> reader_;
      ComPtr<ReaderCallback> callback_;
      std::shared_ptr<SampleState> state_ { std::make_shared<SampleState>() };
      bool pending_ {};
      UINT32 width_ {}, height_ {};
      LONG stride_ {};
      std::uint64_t sequence_ {};

      void shutdown() noexcept
      {
        if (reader_)
          reader_->Flush(all_streams);
        reader_.Reset();
        if (source_)
          source_->Shutdown();
        source_.Reset();
        callback_.Reset();
        if (mf_)
          MFShutdown();
        if (com_)
          CoUninitialize();
      }

    public:
      explicit MediaFoundation(const Config& config)
      {
        try
        {
          check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx");
          com_ = true;
          check(MFStartup(MF_VERSION), "MFStartup");
          mf_ = true;
          ComPtr<IMFAttributes> attributes;
          check(MFCreateAttributes(&attributes, 1), "MFCreateAttributes");
          check(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                  MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID),
            "Set capture source type");
          Devices devices;
          check(MFEnumDeviceSources(attributes.Get(), &devices.items, &devices.count),
            "MFEnumDeviceSources");
          UINT32 index {};
          const std::string_view device(config.device.data());
          const auto [end, error] = std::from_chars(device.data(), device.data() + device.size(), index);
          if (error != std::errc {} || end != device.data() + device.size() || index >= devices.count)
            throw std::runtime_error(
              "Windows --device must be an available camera index (starting at 0)"
            );
          check(devices.items[index]->ActivateObject(IID_PPV_ARGS(&source_)), "Activate camera");
          callback_ = Microsoft::WRL::Make<ReaderCallback>();
          callback_->state = state_;
          ComPtr<IMFAttributes> reader_attributes;
          check(MFCreateAttributes(&reader_attributes, 2), "MFCreateAttributes");
          check(reader_attributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback_.Get()),
            "Set callback");
          check(reader_attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE),
            "Enable RGB conversion");
          check(MFCreateSourceReaderFromMediaSource(source_.Get(), reader_attributes.Get(), &reader_),
            "Create reader");
          check(reader_->SetStreamSelection(all_streams, FALSE), "Deselect streams");
          check(reader_->SetStreamSelection(video_stream, TRUE), "Select video");
          bool matched = false;
          for (DWORD i = 0;; ++i)
          {
            ComPtr<IMFMediaType> native;
            const auto result = reader_->GetNativeMediaType(video_stream, i, &native);
            if (result == MF_E_NO_MORE_TYPES)
              break;
            check(result, "GetNativeMediaType");
            UINT32 width {}, height {}, numerator {}, denominator {};
            if (FAILED(MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &width, &height)) || FAILED(MFGetAttributeRatio(native.Get(), MF_MT_FRAME_RATE, &numerator, &denominator)))
              continue;
            if (width == config.width && height == config.height && denominator != 0 && numerator == static_cast<std::uint64_t>(config.fps) * denominator)
            {
              check(reader_->SetCurrentMediaType(video_stream, nullptr, native.Get()),
                "Set native capture mode");
              matched = true;
              break;
            }
          }
          if (!matched)
            throw std::runtime_error("Camera does not offer the requested width, height and fps");
          ComPtr<IMFMediaType> rgb;
          check(MFCreateMediaType(&rgb), "MFCreateMediaType");
          check(rgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Set video type");
          check(rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32), "Set RGB32 type");
          check(reader_->SetCurrentMediaType(video_stream, nullptr, rgb.Get()), "Set RGB32 output");
          ComPtr<IMFMediaType> actual;
          check(reader_->GetCurrentMediaType(video_stream, &actual), "Get output type");
          check(MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &width_, &height_), "Get dimensions");
          if (width_ != config.width || height_ != config.height)
            throw std::runtime_error("Unexpected Media Foundation output dimensions");
          UINT32 stride {};
          if (SUCCEEDED(actual->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)))
            stride_ = static_cast<LONG>(stride);
          else
            check(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, width_, &stride_),
              "Get stride");
        }
        catch (...)
        {
          shutdown();
          throw;
        }
      }

      ~MediaFoundation() override
      {
        shutdown();
      }

      bool read(Frame& frame, std::stop_token stop) override
      {
        if (stop.stop_requested())
          return false;
        if (!pending_)
        {
          {
            std::lock_guard lock(state_->mutex);
            state_->complete = false;
            state_->sample.Reset();
          }
          check(reader_->ReadSample(video_stream, 0, nullptr, nullptr, nullptr, nullptr), "ReadSample");
          pending_ = true;
        }
        ComPtr<IMFSample> sample;
        {
          std::unique_lock lock(state_->mutex);
          if (!state_->ready.wait_for(lock, stop, std::chrono::milliseconds(200),
                [this] { return state_->complete; }))
            return false;
          pending_ = false;
          check(state_->result, "Camera sample");
          if (state_->flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED))
            throw std::runtime_error("Camera disconnected or changed format");
          sample = state_->sample;
          frame.timestamp_ns = state_->timestamp_ns;
        }
        if (!sample)
          return false;
        frame.width = width_;
        frame.height = height_;
        frame.sequence = ++sequence_;
        frame.rgb.resize(static_cast<std::size_t>(width_) * height_ * 3);
        ComPtr<IMFMediaBuffer> buffer;
        check(sample->ConvertToContiguousBuffer(&buffer), "Get sample buffer");
        ComPtr<IMF2DBuffer> planar;
        BYTE* data {};
        LONG pitch = stride_;
        const bool is_2d = SUCCEEDED(buffer.As(&planar));
        if (is_2d)
          check(planar->Lock2D(&data, &pitch), "Lock2D");
        else
        {
          DWORD length {};
          check(buffer->Lock(&data, nullptr, &length), "Lock buffer");
          const auto row_size =
            static_cast<std::size_t>(pitch < 0 ? -static_cast<std::int64_t>(pitch) : pitch);
          if (row_size < width_ * 4 || length < row_size * height_)
          {
            buffer->Unlock();
            throw std::runtime_error("Incomplete RGB32 frame");
          }
          if (pitch < 0)
            data += (height_ - 1) * row_size;
        }
        if (static_cast<std::int64_t>(pitch < 0 ? -static_cast<std::int64_t>(pitch) : pitch) < width_ * 4)
        {
          if (is_2d)
            planar->Unlock2D();
          else
            buffer->Unlock();
          throw std::runtime_error("Invalid RGB32 stride");
        }
        for (UINT32 y = 0; y < height_; ++y)
        {
          const BYTE* row = data + static_cast<std::ptrdiff_t>(y) * pitch;
          auto* dest = frame.rgb.data() + static_cast<std::size_t>(y) * width_ * 3;
          for (UINT32 x = 0; x < width_; ++x)
          {
            dest[x * 3] = row[x * 4 + 2];
            dest[x * 3 + 1] = row[x * 4 + 1];
            dest[x * 3 + 2] = row[x * 4];
          }
        }
        if (is_2d)
          check(planar->Unlock2D(), "Unlock2D");
        else
          check(buffer->Unlock(), "Unlock buffer");
        return true;
      }
    };
  }  // namespace

  std::unique_ptr<Capture> open_capture(const Config& config)
  {
    return std::make_unique<MediaFoundation>(config);
  }
}  // namespace sof
