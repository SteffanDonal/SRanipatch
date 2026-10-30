#include "overlay.hpp"

#include <glad/gl.h>
#include <imgui.h>
#include <openvr.h>

#include <cfloat>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>

namespace sranipatch
{
  namespace
  {
    void check(vr::EVROverlayError error)
    {
      if (error != vr::VROverlayError_None)
        throw std::runtime_error(std::string("OpenVR overlay: ") + vr::VROverlay()->GetOverlayErrorNameFromEnum(error));
    }
  }  // namespace

  struct Overlay::Impl
  {
    bool initialized {}, submitted {}, quit {};
    vr::VROverlayHandle_t handle { vr::k_ulOverlayHandleInvalid };
    vr::VROverlayHandle_t thumbnail { vr::k_ulOverlayHandleInvalid };
    GLuint texture {}, framebuffer {};
    int height {};

    ~Impl()
    {
      if (initialized)
      {
        if (handle != vr::k_ulOverlayHandleInvalid)
        {
          vr::VROverlay()->ClearOverlayTexture(handle);
          vr::VROverlay()->DestroyOverlay(handle);
        }
        if (thumbnail != vr::k_ulOverlayHandleInvalid)
        {
          vr::VROverlay()->ClearOverlayTexture(thumbnail);
          vr::VROverlay()->DestroyOverlay(thumbnail);
        }
        vr::VR_Shutdown();
      }
      if (framebuffer)
        glDeleteFramebuffers(1, &framebuffer);
      if (texture)
        glDeleteTextures(1, &texture);
    }

    void initialize(SurfaceSize size, const std::filesystem::path& thumbnail_path)
    {
      height = size.height;
      vr::EVRInitError error = vr::VRInitError_None;
      vr::VR_Init(&error, vr::VRApplication_Overlay);
      if (error != vr::VRInitError_None)
        throw std::runtime_error(std::string("Cannot initialize OpenVR overlay: ") + vr::VR_GetVRInitErrorAsEnglishDescription(error));
      initialized = true;
      if (!vr::VROverlay())
        throw std::runtime_error("OpenVR overlay interface is unavailable");
      check(vr::VROverlay()->CreateDashboardOverlay("sranipatch.dashboard", "SRanipatch", &handle,
        &thumbnail));
      const auto thumbnail_utf8 = std::filesystem::absolute(thumbnail_path).u8string();
      const std::string thumbnail_filename(thumbnail_utf8.begin(), thumbnail_utf8.end());
      check(vr::VROverlay()->SetOverlayFromFile(thumbnail, thumbnail_filename.c_str()));
      check(vr::VROverlay()->SetOverlayWidthInMeters(handle, 1.8F));
      check(vr::VROverlay()->SetOverlayInputMethod(handle, vr::VROverlayInputMethod_Mouse));
      const vr::HmdVector2_t mouse_scale { { static_cast<float>(size.width), static_cast<float>(height) } };
      check(vr::VROverlay()->SetOverlayMouseScale(handle, &mouse_scale));
      check(vr::VROverlay()->SetOverlayFlag(handle, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true));
      check(vr::VROverlay()->SetOverlayFlag(handle, vr::VROverlayFlags_MinimalControlBar, true));
      check(vr::VROverlay()->SetOverlayFlag(handle, vr::VROverlayFlags_EnableControlBarClose, true));

      glGenTextures(1, &texture);
      glBindTexture(GL_TEXTURE_2D, texture);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size.width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glGenFramebuffers(1, &framebuffer);
      glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
      if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        throw std::runtime_error("Cannot create OpenVR overlay render target");
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void event(const vr::VREvent_t& event)
    {
      auto& io = ImGui::GetIO();
      switch (event.eventType)
      {
        case vr::VREvent_Quit:
          vr::VRSystem()->AcknowledgeQuit_Exiting();
          quit = true;
          break;
        case vr::VREvent_OverlayClosed:
          quit = true;
          break;
        case vr::VREvent_MouseMove:
          io.AddMousePosEvent(event.data.mouse.x, static_cast<float>(height) - event.data.mouse.y);
          break;
        case vr::VREvent_MouseButtonDown:
        case vr::VREvent_MouseButtonUp:
        {
          const auto button = event.data.mouse.button;
          const int index = button == vr::VRMouseButton_Left ? 0
            : button == vr::VRMouseButton_Right              ? 1
            : button == vr::VRMouseButton_Middle             ? 2
                                                             : -1;
          if (index >= 0)
            io.AddMouseButtonEvent(index, event.eventType == vr::VREvent_MouseButtonDown);
          break;
        }
        case vr::VREvent_ScrollDiscrete:
          io.AddMouseWheelEvent(event.data.scroll.xdelta, event.data.scroll.ydelta);
          break;
        case vr::VREvent_FocusLeave:
        case vr::VREvent_OverlayHidden:
          io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
          for (int button = 0; button < 3; ++button)
            io.AddMouseButtonEvent(button, false);
          break;
        default:
          break;
      }
    }
  };

  Overlay::Overlay(SurfaceSize size, const std::filesystem::path& thumbnail) : impl_(std::make_unique<Impl>())
  {
    impl_->initialize(size, thumbnail);
  }

  Overlay::~Overlay() = default;

  bool Overlay::poll()
  {
    vr::VREvent_t event {};
    while (vr::VRSystem()->PollNextEvent(&event, sizeof(event)))
      impl_->event(event);
    while (vr::VROverlay()->PollNextOverlayEvent(impl_->handle, &event, sizeof(event)))
      impl_->event(event);
    return !impl_->quit;
  }

  bool Overlay::visible() const
  {
    return !impl_->submitted || vr::VROverlay()->IsOverlayVisible(impl_->handle);
  }

  void Overlay::bind_target() const
  {
    glBindFramebuffer(GL_FRAMEBUFFER, impl_->framebuffer);
  }

  void Overlay::submit()
  {
    glFlush();
    // OpenVR encodes an OpenGL texture name in its opaque pointer handle.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const vr::Texture_t texture { reinterpret_cast<void*>(static_cast<std::uintptr_t>(impl_->texture)),
      vr::TextureType_OpenGL, vr::ColorSpace_Gamma };
    check(vr::VROverlay()->SetOverlayTexture(impl_->handle, &texture));
    impl_->submitted = true;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  }

  void Overlay::wait_frame() const
  {
    if (vr::VROverlay()->WaitFrameSync(20) != vr::VROverlayError_None)
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}
