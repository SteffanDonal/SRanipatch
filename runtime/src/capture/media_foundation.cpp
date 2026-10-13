#include "sof/capture.hpp"
#include "sof/vive_tracker.hpp"

#include <windows.h>
#include <dshow.h>
#include <ks.h>
#include <ksmedia.h>
#include <ksproxy.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <vidcap.h>
#include <wrl.h>

#include <algorithm>
#include <charconv>
#include <condition_variable>
#include <cwctype>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
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
      {
        std::ostringstream message;
        message << operation << " failed (HRESULT 0x" << std::hex << static_cast<unsigned long>(result)
                << ')';
        throw std::runtime_error(message.str());
      }
    }

    std::wstring attribute_string(IMFAttributes* attributes, const GUID& key)
    {
      wchar_t* value {};
      UINT32 length {};
      check(attributes->GetAllocatedString(key, &value, &length), "Get device identity");
      std::wstring result(value, length);
      CoTaskMemFree(value);
      return result;
    }

    std::wstring normalized(std::wstring value)
    {
      std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return std::towlower(c); });
      const auto start = value.find(L"usb#");
      if (start != std::wstring::npos)
        value.erase(0, start);
      const auto suffix = value.find(L"#{");
      if (suffix != std::wstring::npos)
        value.resize(suffix);
      return value;
    }

    bool is_vive(const std::wstring& path)
    {
      const auto identity = normalized(path);
      return identity.find(L"vid_0bb4&pid_0321") != std::wstring::npos;
    }

    constexpr GUID vive_extension_guid {
      0x2ccb0bda, 0x6331, 0x4fdb, { 0x85, 0x0e, 0x79, 0x05, 0x4d, 0xbd, 0x56, 0x71 }
    };

    class WindowsTrackerExtension final : public TrackerExtension
    {
      ComPtr<IKsControl> control_;
      ComPtr<IBaseFilter> filter_;
      ULONG node_ {};
      ULONG size_ {};

      KSP_NODE property(ULONG flags) const
      {
        KSP_NODE value {};
        value.Property.Set = vive_extension_guid;
        value.Property.Id = 2;
        value.Property.Flags = flags | KSPROPERTY_TYPE_TOPOLOGY;
        value.NodeId = node_;
        return value;
      }

      bool select(ComPtr<IKsControl> control, ULONG node)
      {
        control_ = std::move(control);
        node_ = node;
        auto request = property(KSPROPERTY_TYPE_GET);
        ULONG size {};
        const auto result = control_->KsProperty(&request.Property, sizeof(request), nullptr, 0, &size);
        if ((result == HRESULT_FROM_WIN32(ERROR_MORE_DATA) || result == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) || SUCCEEDED(result)) && (size == 384 || size == 64))
        {
          size_ = size;
          std::cout << "VIVE extension: node " << node_ << ", selector 2, " << size_ << " bytes\n";
          return true;
        }
        control_.Reset();
        return false;
      }

      bool topology(IUnknown* object)
      {
        ComPtr<IKsTopologyInfo> topology;
        if (FAILED(object->QueryInterface(IID_PPV_ARGS(&topology))))
          return false;
        DWORD count {};
        check(topology->get_NumNodes(&count), "Get KS node count");
        for (DWORD i = 0; i < count; ++i)
        {
          GUID type {};
          check(topology->get_NodeType(i, &type), "Get KS node type");
          if (type != KSNODETYPE_DEV_SPECIFIC && type != vive_extension_guid)
            continue;
          ComPtr<IKsControl> control;
          if (SUCCEEDED(topology->CreateNodeInstance(i, IID_PPV_ARGS(&control))) && select(control, i))
            return true;
        }
        return false;
      }

      bool media_source(IMFMediaSource* source)
      {
        if (topology(source))
          return true;
        ComPtr<IKsControl> control;
        if (FAILED(source->QueryInterface(IID_PPV_ARGS(&control))))
          return false;
        KSPROPERTY request { KSPROPSETID_Topology, KSPROPERTY_TOPOLOGY_NODES, KSPROPERTY_TYPE_GET };
        ULONG size {};
        control->KsProperty(&request, sizeof(request), nullptr, 0, &size);
        if (size < sizeof(KSMULTIPLE_ITEM) || size > 65536)
          return false;
        std::vector<std::uint8_t> data(size);
        ULONG received {};
        if (FAILED(control->KsProperty(&request, sizeof(request), data.data(), size, &received)))
          return false;
        if (received < sizeof(KSMULTIPLE_ITEM))
          return false;
        const auto* header = reinterpret_cast<const KSMULTIPLE_ITEM*>(data.data());
        if (header->Count > (received - sizeof(KSMULTIPLE_ITEM)) / sizeof(GUID))
          return false;
        const auto* types = reinterpret_cast<const GUID*>(data.data() + sizeof(KSMULTIPLE_ITEM));
        for (ULONG i = 0; i < header->Count; ++i)
          if ((types[i] == KSNODETYPE_DEV_SPECIFIC || types[i] == vive_extension_guid) && select(control, i))
            return true;
        return false;
      }

    public:
      WindowsTrackerExtension(IMFMediaSource* source, const std::wstring& path)
      {
        if (media_source(source))
          return;
        // Some Windows frame-server sources don't expose KS topology. Bind only
        // the matching DirectShow filter for controls; samples still come from MF.
        ComPtr<ICreateDevEnum> devices;
        check(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devices)),
          "Create KS device enumerator");
        ComPtr<IEnumMoniker> monikers;
        if (devices->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &monikers, 0) != S_OK)
          throw std::runtime_error("VIVE camera KS control filter is unavailable");
        ComPtr<IMoniker> moniker;
        while (monikers->Next(1, &moniker, nullptr) == S_OK)
        {
          ComPtr<IPropertyBag> properties;
          if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&properties))))
          {
            VARIANT value;
            VariantInit(&value);
            const auto result = properties->Read(L"DevicePath", &value, nullptr);
            const bool matched =
              SUCCEEDED(result) && value.vt == VT_BSTR && normalized(value.bstrVal) == normalized(path);
            VariantClear(&value);
            if (matched)
            {
              check(moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&filter_)),
                "Open VIVE KS control filter");
              if (topology(filter_.Get()))
              {
                std::cout << "VIVE controls: KS proxy; video: Media Foundation\n";
                return;
              }
              break;
            }
          }
          moniker.Reset();
        }
        throw std::runtime_error("VIVE selector 2 was not found in the camera's KS topology");
      }

      std::size_t buffer_size() const override
      {
        return size_;
      }

      void get(std::span<std::uint8_t> data) override
      {
        auto request = property(KSPROPERTY_TYPE_GET);
        ULONG received {};
        check(control_->KsProperty(&request.Property, sizeof(request), data.data(),
                static_cast<ULONG>(data.size()), &received),
          "VIVE GET_CUR");
        if (received != data.size())
          throw std::runtime_error("VIVE GET_CUR returned an incomplete payload");
      }

      void set(std::span<const std::uint8_t> data) override
      {
        auto request = property(KSPROPERTY_TYPE_SET);
        ULONG received {};
        check(control_->KsProperty(&request.Property, sizeof(request),
                const_cast<std::uint8_t*>(data.data()), static_cast<ULONG>(data.size()),
                &received),
          "VIVE SET_CUR");
      }
    };

    struct SampleState
    {
      std::mutex mutex;
      std::condition_variable_any ready;
      ComPtr<IMFSample> sample;
      HRESULT result { S_OK };
      DWORD flags {};
      bool complete {};
      bool flushed {};
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
        {
          std::lock_guard lock(state->mutex);
          state->flushed = true;
        }
        state->ready.notify_all();
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
      std::unique_ptr<WindowsTrackerExtension> tracker_;
      bool tracker_initialized_ {};
      bool grayscale_ {};

      void shutdown() noexcept
      {
        if (reader_)
        {
          if (SUCCEEDED(reader_->Flush(all_streams)))
          {
            std::unique_lock lock(state_->mutex);
            state_->ready.wait_for(lock, std::chrono::seconds(1), [this] { return state_->flushed; });
          }
        }
        {
          std::lock_guard lock(state_->mutex);
          state_->sample.Reset();
        }
        if (tracker_initialized_)
        {
          try
          {
            disable_vive_tracker(*tracker_);
          }
          catch (const std::exception& error)
          {
            std::cerr << "VIVE shutdown: " << error.what() << '\n';
          }
        }
        reader_.Reset();
        tracker_.reset();
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
          if (device == "vive")
          {
            UINT32 matches {};
            for (UINT32 i = 0; i < devices.count; ++i)
              if (is_vive(attribute_string(devices.items[i],
                    MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK)))
              {
                index = i;
                ++matches;
              }
            if (matches != 1)
              throw std::runtime_error("Expected one VIVE Facial Tracker; use --list and --device " "INDEX for multiple trackers");
          }
          else
          {
            const auto [end, error] =
              std::from_chars(device.data(), device.data() + device.size(), index);
            if (error != std::errc {} || end != device.data() + device.size() || index >= devices.count)
              throw std::runtime_error(
                "Windows --device must be an available camera index (starting at 0)"
              );
          }
          const auto path = attribute_string(devices.items[index],
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
          grayscale_ = is_vive(path);
          std::wcout << L"Opening camera " << index << L": "
                     << attribute_string(devices.items[index], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME)
                     << L'\n';
          check(devices.items[index]->ActivateObject(IID_PPV_ARGS(&source_)), "Activate camera");
          if (grayscale_)
          {
            tracker_ = std::make_unique<WindowsTrackerExtension>(source_.Get(), path);
            tracker_initialized_ = true;
            enable_vive_tracker(*tracker_);
            std::cout << "VIVE sensor registers configured; stream enabled\n";
          }
          callback_ = Microsoft::WRL::Make<ReaderCallback>();
          callback_->state = state_;
          ComPtr<IMFAttributes> reader_attributes;
          check(MFCreateAttributes(&reader_attributes, 2), "MFCreateAttributes");
          check(reader_attributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback_.Get()),
            "Set callback");
          check(reader_attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING,
                  grayscale_ ? FALSE : TRUE),
            "Enable RGB conversion");
          check(reader_attributes->SetUINT32(MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN, TRUE),
            "Keep media source alive for explicit shutdown");
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
            GUID subtype {};
            if (FAILED(native->GetGUID(MF_MT_SUBTYPE, &subtype)))
              continue;
            if (FAILED(MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &width, &height)) || FAILED(MFGetAttributeRatio(native.Get(), MF_MT_FRAME_RATE, &numerator, &denominator)))
              continue;
            if (width == config.width && height == config.height && denominator != 0 && numerator == static_cast<std::uint64_t>(config.fps) * denominator && (!grayscale_ || subtype == MFVideoFormat_YUY2))
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
          check(rgb->SetGUID(MF_MT_SUBTYPE, grayscale_ ? MFVideoFormat_YUY2 : MFVideoFormat_RGB32),
            "Set output pixel format");
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
            check(MFGetStrideForBitmapInfoHeader(grayscale_ ? MFVideoFormat_YUY2.Data1
                                                            : MFVideoFormat_RGB32.Data1,
                    width_, &stride_),
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
          if (row_size < width_ * (grayscale_ ? 2 : 4) || length < row_size * height_)
          {
            buffer->Unlock();
            throw std::runtime_error("Incomplete RGB32 frame");
          }
          if (pitch < 0)
            data += (height_ - 1) * row_size;
        }
        if (static_cast<std::int64_t>(pitch < 0 ? -static_cast<std::int64_t>(pitch) : pitch) < width_ * (grayscale_ ? 2 : 4))
        {
          if (is_2d)
            planar->Unlock2D();
          else
            buffer->Unlock();
          throw std::runtime_error("Invalid RGB32 stride");
        }
        if (grayscale_)
          yuyv_luma_to_rgb(data, pitch, frame);
        else
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

  void list_capture_devices()
  {
    check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx");

    struct Cleanup
    {
      ~Cleanup()
      {
        MFShutdown();
        CoUninitialize();
      }
    } cleanup;

    check(MFStartup(MF_VERSION), "MFStartup");
    ComPtr<IMFAttributes> attributes;
    check(MFCreateAttributes(&attributes, 1), "Create capture attributes");
    check(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID),
      "Set capture source type");
    Devices devices;
    check(MFEnumDeviceSources(attributes.Get(), &devices.items, &devices.count), "List capture devices");
    for (UINT32 i = 0; i < devices.count; ++i)
    {
      const auto path =
        attribute_string(devices.items[i], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
      std::wcout << i << L": " << attribute_string(devices.items[i], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME)
                 << (is_vive(path) ? L" [VIVE Facial Tracker]" : L"") << L'\n';
      ComPtr<IMFMediaSource> source;
      const auto activated = devices.items[i]->ActivateObject(IID_PPV_ARGS(&source));
      if (FAILED(activated))
      {
        std::cout << "  Camera unavailable: 0x" << std::hex << activated << std::dec << '\n';
        continue;
      }
      ComPtr<IMFSourceReader> reader;
      const auto created = MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &reader);
      if (SUCCEEDED(created))
      {
        for (DWORD mode = 0;; ++mode)
        {
          ComPtr<IMFMediaType> type;
          if (reader->GetNativeMediaType(video_stream, mode, &type) != S_OK)
            break;
          UINT32 width {}, height {}, numerator {}, denominator {};
          GUID subtype {};
          MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height);
          MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &numerator, &denominator);
          type->GetGUID(MF_MT_SUBTYPE, &subtype);
          char fourcc[5] {};
          for (int c = 0; c < 4; ++c)
            fourcc[c] = static_cast<char>((subtype.Data1 >> (c * 8)) & 0xff);
          std::cout << "  " << width << 'x' << height << " @ " << numerator << '/' << denominator
                    << " fps, " << fourcc << '\n';
        }
      }
      reader.Reset();
      source->Shutdown();
    }
  }
}  // namespace sof
