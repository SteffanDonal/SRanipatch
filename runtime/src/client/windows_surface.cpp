#include "windows_surface.hpp"
#define GLFW_EXPOSE_NATIVE_WIN32
#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <commctrl.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <exception>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>

namespace sranipatch
{
  namespace
  {
    constexpr GLenum write_discard_access = 0x0002;

    void check(HRESULT result)
    {
      if (FAILED(result))
        throw std::runtime_error("Windows GPU presentation failed: " + std::to_string(result));
    }

    template <typename Function>
    Function load(const char* name)
    {
      const auto function = glfwGetProcAddress(name);
      if (!function)
        throw std::runtime_error(std::string("Missing GPU interop function: ") + name);
      return reinterpret_cast<Function>(function);
    }

    struct ClientGeometry
    {
      int width {}, height {};
    };

    struct WindowPump
    {
      ClientGeometry& geometry;

      explicit WindowPump(ClientGeometry& value) : geometry(value) {}

      HWND window {};
      void *render_fiber {}, *event_fiber {};
      UINT_PTR timer {};
      UINT wake_message {};
      HANDLE clock_timer {}, stop_clock {};
      std::jthread clock;
      std::atomic<bool> wake_pending {}, clock_failed {};
      bool converted {}, stopping {}, polling {};
      bool preparing {}, changing_geometry {};
      std::function<void()> commit;
      std::exception_ptr error;
      HHOOK retrieval_hook {};
      static thread_local WindowPump* hook_owner;
      bool commit_pending {};

      void wake_during_native_loop(bool active) noexcept
      {
        if (stopping || !clock_timer)
          return;
        if (!active)
        {
          CancelWaitableTimer(clock_timer);
          if (timer)
            KillTimer(window, timer);
          timer = 0;
          return;
        }
        LARGE_INTEGER due {};
        due.QuadPart = -10000;
        if (!SetWaitableTimer(clock_timer, &due, 1, nullptr, nullptr, FALSE))
          clock_failed = true;
        if (!timer)
          timer = SetTimer(window, reinterpret_cast<UINT_PTR>(this), USER_TIMER_MINIMUM, nullptr);
        if (!timer)
          clock_failed = true;
      }

      void publish() noexcept
      {
        if (commit_pending && !changing_geometry && !stopping && !error)
        {
          try
          {
            commit();
            commit_pending = false;
          }
          catch (...)
          {
            error = std::current_exception();
          }
        }
      }

      // Windows fixes the hook callback signature.
      // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
      static LRESULT CALLBACK retrieve(int code, WPARAM wparam, LPARAM lparam)
      {
        if (code == HC_ACTION && hook_owner && GetCurrentFiber() == hook_owner->event_fiber)
          if (hook_owner->commit_pending)
            hook_owner->changing_geometry = false;
        return CallNextHookEx(nullptr, code, wparam, lparam);
      }

      void yield() const
      {
        if (!stopping && (!changing_geometry || preparing) && GetCurrentFiber() == event_fiber)
          SwitchToFiber(render_fiber);
      }

      // Windows fixes the subclass callback signature and transports our pointer in DWORD_PTR.
      // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
      static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR,
        DWORD_PTR data)
      {
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto& self = *reinterpret_cast<WindowPump*>(data);
        if (message == WM_ERASEBKGND)
          return 1;
        if (message == WM_PAINT)
        {
          // The swapchain owns client pixels, including DWM's retained animation image.
          PAINTSTRUCT paint {};
          BeginPaint(hwnd, &paint);
          self.publish();
          EndPaint(hwnd, &paint);
          return 0;
        }
        if (message == WM_NCCALCSIZE && wparam)
        {
          self.changing_geometry = true;
          const auto result = DefSubclassProc(hwnd, message, wparam, lparam);
          // The backdrop handler has converted the proposed rectangle to client bounds.
          // NOLINTNEXTLINE(performance-no-int-to-ptr)
          const auto& bounds = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam)->rgrc[0];
          self.geometry = { bounds.right - bounds.left, bounds.bottom - bounds.top };
          if (self.geometry.width > 0 && self.geometry.height > 0 && GetCurrentFiber() == self.event_fiber)
          {
            self.preparing = true;
            self.yield();
            self.preparing = false;
            self.commit_pending = true;
          }
          return result;
        }
        if (message == WM_WINDOWPOSCHANGED)
        {
          const auto result = DefSubclassProc(hwnd, message, wparam, lparam);
          self.changing_geometry = false;
          if (self.commit_pending)
          {
            InvalidateRect(hwnd, nullptr, FALSE);
          }
          return result;
        }
        if (message == WM_SIZE && wparam != SIZE_MINIMIZED)
          self.geometry = { LOWORD(lparam), HIWORD(lparam) };
        if (message == WM_ENTERSIZEMOVE || message == WM_ENTERMENULOOP)
          self.wake_during_native_loop(true);
        if (message == WM_EXITSIZEMOVE || message == WM_EXITMENULOOP)
          self.wake_during_native_loop(false);
        if (message == WM_NCLBUTTONDOWN)
        {
          self.wake_during_native_loop(true);
          // Caption drag detection initially waits only for mouse messages (SDL's workaround).
          POINT point {};
          if (wparam == HTCAPTION && GetCursorPos(&point) && ScreenToClient(hwnd, &point))
            PostMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(point.x, point.y));
          const auto result = DefSubclassProc(hwnd, message, wparam, lparam);
          self.wake_during_native_loop(false);
          return result;
        }
        if (message == self.wake_message)
        {
          self.wake_pending = false;
          // Posted messages outrank mouse input. Drain input before yielding again.
          if (!(GetQueueStatus(QS_INPUT) & 0xffff0000U))
            self.yield();
          return 0;
        }
        if (message == WM_TIMER && wparam == self.timer)
        {
          self.yield();
          return 0;
        }
        return DefSubclassProc(hwnd, message, wparam, lparam);
      }

      static void WINAPI dispatch(void* data)
      {
        auto& self = *static_cast<WindowPump*>(data);
        for (;;)
        {
          self.changing_geometry = false;
          self.polling = true;
          if (IsIconic(self.window) && !self.stopping)
            glfwWaitEventsTimeout(0.05);
          else
            glfwPollEvents();
          self.polling = false;
          if (self.commit_pending)
            self.changing_geometry = false;
          SwitchToFiber(self.render_fiber);
        }
      }

      void initialize(GLFWwindow* glfw_window)
      {
        window = glfwGetWin32Window(glfw_window);
        if (hook_owner)
          throw std::runtime_error("Windows event scheduler already active on this thread");
        hook_owner = this;
        retrieval_hook = SetWindowsHookExW(WH_GETMESSAGE, retrieve, nullptr, GetCurrentThreadId());
        if (!retrieval_hook)
          throw std::runtime_error("Cannot install Windows message retrieval hook");
        if (IsThreadAFiber())
          render_fiber = GetCurrentFiber();
        else
        {
          render_fiber = ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
          converted = render_fiber != nullptr;
        }
        if (!render_fiber)
          throw std::runtime_error("Cannot create Windows render fiber");
        event_fiber = CreateFiberEx(0, 0, FIBER_FLAG_FLOAT_SWITCH, dispatch, this);
        if (!event_fiber)
          throw std::runtime_error("Cannot create Windows event fiber");
        if (!SetWindowSubclass(window, procedure, reinterpret_cast<UINT_PTR>(this),
              reinterpret_cast<DWORD_PTR>(this)))
          throw std::runtime_error("Cannot install Windows event scheduler");
        RECT bounds {};
        if (!GetClientRect(window, &bounds))
          throw std::runtime_error("Cannot inspect initial Windows client geometry");
        geometry = { bounds.right, bounds.bottom };
        wake_message = RegisterWindowMessageW(L"SRanipatch.RenderWakeup");
        clock_timer =
          CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        stop_clock = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!wake_message || !clock_timer || !stop_clock)
          throw std::runtime_error("Cannot create Windows render clock");
        clock = std::jthread([this] {
          const HANDLE handles[] { stop_clock, clock_timer };
          for (;;)
          {
            const auto result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            if (result == WAIT_OBJECT_0)
              return;
            if (result != WAIT_OBJECT_0 + 1)
            {
              clock_failed = true;
              return;
            }
            if (!wake_pending.exchange(true) && !PostMessageW(window, wake_message, 0, 0))
              wake_pending = false;
          }
        });
      }

      ~WindowPump()
      {
        stopping = true;
        if (stop_clock)
          SetEvent(stop_clock);
        if (clock.joinable())
          clock.join();
        if (clock_timer)
          CloseHandle(clock_timer);
        if (stop_clock)
          CloseHandle(stop_clock);
        if (timer)
          KillTimer(window, timer);
        // Unwind any native modal dispatch before deleting its suspended stack.
        if (polling)
        {
          PostMessageW(window, WM_CANCELMODE, 0, 0);
          PostMessageW(window, WM_LBUTTONUP, 0, 0);
          SwitchToFiber(event_fiber);
        }
        if (window)
          RemoveWindowSubclass(window, procedure, reinterpret_cast<UINT_PTR>(this));
        if (retrieval_hook)
          UnhookWindowsHookEx(retrieval_hook);
        if (hook_owner == this)
          hook_owner = nullptr;
        if (event_fiber)
          DeleteFiber(event_fiber);
        if (converted)
          ConvertFiberToThread();
      }
    };

    thread_local WindowPump* WindowPump::hook_owner {};
  }  // namespace

  using Microsoft::WRL::ComPtr;

  struct WindowsSurface::Impl
  {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swapchain;
    GLFWwindow* glfw_window {};
    HWND window {};
    ClientGeometry geometry;
    std::unique_ptr<WindowPump> pump;
    GLuint drawing_framebuffer {}, drawing_texture {};
    int buffer_width {}, buffer_height {};
    ComPtr<ID3D11Texture2D> shared_texture;
    HANDLE interop_device {}, interop_object {};
    using OpenDevice = HANDLE(WINAPI*)(void*);
    using CloseDevice = BOOL(WINAPI*)(HANDLE);
    using RegisterObject = HANDLE(WINAPI*)(HANDLE, void*, GLuint, GLenum, GLenum);
    using UnregisterObject = BOOL(WINAPI*)(HANDLE, HANDLE);
    using LockObjects = BOOL(WINAPI*)(HANDLE, GLint, HANDLE*);
    CloseDevice close_device {};
    RegisterObject register_object {};
    UnregisterObject unregister_object {};
    LockObjects lock_objects {}, unlock_objects {};
    GLuint framebuffer {}, renderbuffer {};
    int width {}, height {};
    bool locked {}, ready {};

    ~Impl()
    {
      pump.reset();
      if (drawing_texture)
        glDeleteTextures(1, &drawing_texture);
      if (drawing_framebuffer)
        glDeleteFramebuffers(1, &drawing_framebuffer);
      if (locked)
        unlock_objects(interop_device, 1, &interop_object);
      if (interop_object)
        unregister_object(interop_device, interop_object);
      if (framebuffer)
        glDeleteFramebuffers(1, &framebuffer);
      if (renderbuffer)
        glDeleteRenderbuffers(1, &renderbuffer);
      if (interop_device)
        close_device(interop_device);
    }

    void initialize(GLFWwindow* value)
    {
      glfw_window = value;
      window = glfwGetWin32Window(value);
      // Redirection alpha requires Windows 11 24H2 or newer.
      const BOOL alpha = TRUE;
      check(DwmSetWindowAttribute(window, DWMWA_REDIRECTIONBITMAP_ALPHA, &alpha, sizeof(alpha)));
      if (!glfwExtensionSupported("WGL_NV_DX_interop2"))
        throw std::runtime_error("Windows presentation requires OpenGL/Direct3D GPU interop");
      const auto open_device = load<OpenDevice>("wglDXOpenDeviceNV");
      close_device = load<CloseDevice>("wglDXCloseDeviceNV");
      register_object = load<RegisterObject>("wglDXRegisterObjectNV");
      unregister_object = load<UnregisterObject>("wglDXUnregisterObjectNV");
      lock_objects = load<LockObjects>("wglDXLockObjectsNV");
      unlock_objects = load<LockObjects>("wglDXUnlockObjectsNV");
      check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
      interop_device = open_device(device.Get());
      if (!interop_device)
        throw std::runtime_error("Cannot share the Windows presentation device with OpenGL");
      ComPtr<IDXGIDevice> dxgi_device;
      ComPtr<IDXGIAdapter> adapter;
      ComPtr<IDXGIFactory> factory;
      check(device.As(&dxgi_device));
      check(dxgi_device->GetAdapter(&adapter));
      check(adapter->GetParent(IID_PPV_ARGS(&factory)));
      DXGI_SWAP_CHAIN_DESC description {};
      description.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      description.SampleDesc.Count = 1;
      description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      description.BufferCount = 1;
      description.OutputWindow = window;
      description.Windowed = TRUE;
      // Present into the HWND redirection surface, keeping native frame ownership.
      description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
      check(factory->CreateSwapChain(device.Get(), &description, &swapchain));
      check(factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER));
      RECT bounds {};
      GetClientRect(window, &bounds);
      geometry = { bounds.right, bounds.bottom };
      glGenFramebuffers(1, &drawing_framebuffer);
      glGenTextures(1, &drawing_texture);
      glGenFramebuffers(1, &framebuffer);
      glGenRenderbuffers(1, &renderbuffer);
    }

    void bind_target(int new_width, int new_height)
    {
      if (new_width != width || new_height != height)
      {
        if (interop_object)
        {
          if (!unregister_object(interop_device, interop_object))
            throw std::runtime_error("Cannot release Windows GPU interop target");
          interop_object = nullptr;
        }
        shared_texture.Reset();
        width = new_width;
        height = new_height;
        D3D11_TEXTURE2D_DESC description {};
        description.Width = static_cast<UINT>(width);
        description.Height = static_cast<UINT>(height);
        description.MipLevels = description.ArraySize = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        check(device->CreateTexture2D(&description, nullptr, &shared_texture));
        interop_object = register_object(interop_device, shared_texture.Get(), renderbuffer,
          GL_RENDERBUFFER, write_discard_access);
        if (!interop_object)
          throw std::runtime_error("Cannot register Windows GPU interop target");
        glBindTexture(GL_TEXTURE_2D, drawing_texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      }
      if (!lock_objects(interop_device, 1, &interop_object))
        throw std::runtime_error("Cannot lock Windows GPU interop target");
      locked = true;
      glBindFramebuffer(GL_FRAMEBUFFER, drawing_framebuffer);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, drawing_texture, 0);
      if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        throw std::runtime_error("Cannot create Windows GPU render target");
    }

    void commit()
    {
      if (!ready)
        return;
      if (buffer_width != width || buffer_height != height)
      {
        check(swapchain->ResizeBuffers(1, static_cast<UINT>(width), static_cast<UINT>(height),
          DXGI_FORMAT_B8G8R8A8_UNORM, 0));
        buffer_width = width;
        buffer_height = height;
      }
      ComPtr<ID3D11Texture2D> buffer;
      check(swapchain->GetBuffer(0, IID_PPV_ARGS(&buffer)));
      context->CopyResource(buffer.Get(), shared_texture.Get());
      check(swapchain->Present(1, 0));
      ready = false;
    }

    void present()
    {
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
      glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer);
      if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        throw std::runtime_error("Cannot create Windows GPU interop target");
      glBindFramebuffer(GL_READ_FRAMEBUFFER, drawing_framebuffer);
      // Flip on the GPU so presentation has a fixed top-left origin.
      glBlitFramebuffer(0, 0, width, height, 0, height, width, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      if (!unlock_objects(interop_device, 1, &interop_object))
        throw std::runtime_error("Cannot unlock Windows GPU interop target");
      locked = false;
      ready = true;
      if (!pump || !pump->preparing)
      {
        commit();
        check(DwmFlush());
      }
    }
  };

  WindowsSurface::WindowsSurface(GLFWwindow* window) : impl_(std::make_unique<Impl>())
  {
    impl_->initialize(window);
  }

  WindowsSurface::~WindowsSurface() = default;

  void WindowsSurface::stop()
  {
    impl_->pump.reset();
    impl_->ready = false;
    RECT bounds {};
    GetClientRect(impl_->window, &bounds);
    impl_->geometry = { bounds.right, bounds.bottom };
  }

  void WindowsSurface::poll()
  {
    if (!impl_->pump)
    {
      impl_->pump = std::make_unique<WindowPump>(impl_->geometry);
      impl_->pump->commit = [this] { impl_->commit(); };
      impl_->pump->initialize(impl_->glfw_window);
    }
    auto& pump = *impl_->pump;
    if (pump.clock_failed)
      throw std::runtime_error("Windows render clock failed");
    SwitchToFiber(pump.event_fiber);
    if (pump.error)
      std::rethrow_exception(pump.error);
  }

  std::pair<int, int> WindowsSurface::begin_frame()
  {
    if (IsIconic(impl_->window))
      return { 0, 0 };
    return { impl_->geometry.width, impl_->geometry.height };
  }

  void WindowsSurface::bind_target(int width, int height)
  {
    impl_->bind_target(width, height);
  }

  void WindowsSurface::present()
  {
    impl_->present();
  }
}
