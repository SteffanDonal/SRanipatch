#include "head_preview.hpp"

#include "head_model.hpp"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>

namespace sof
{
  namespace
  {
    constexpr GLenum array_buffer = 0x8892, element_buffer = 0x8893, dynamic_draw = 0x88E8, static_draw = 0x88E4,
                     vertex_shader = 0x8B31, fragment_shader = 0x8B30, compile_status = 0x8B81,
                     link_status = 0x8B82, framebuffer = 0x8D40, framebuffer_binding = 0x8CA6,
                     renderbuffer = 0x8D41, renderbuffer_binding = 0x8CA7, color_attachment = 0x8CE0,
                     depth_attachment = 0x8D00, framebuffer_complete = 0x8CD5, depth_component24 = 0x81A6,
                     rgba8 = 0x8058, clamp_to_edge = 0x812F, current_program = 0x8B8D,
                     vertex_array_binding = 0x85B5, array_buffer_binding = 0x8894;

    struct Gl
    {
#define SOF_GL(result, name, ...) \
  using name##Proc = result(APIENTRY*)(__VA_ARGS__); \
  name##Proc name = load<name##Proc>("gl" #name)
      SOF_GL(GLuint, CreateShader, GLenum);
      SOF_GL(void, ShaderSource, GLuint, GLsizei, const char* const*, const GLint*);
      SOF_GL(void, CompileShader, GLuint);
      SOF_GL(void, GetShaderiv, GLuint, GLenum, GLint*);
      SOF_GL(void, GetShaderInfoLog, GLuint, GLsizei, GLsizei*, char*);
      SOF_GL(void, DeleteShader, GLuint);
      SOF_GL(GLuint, CreateProgram, void);
      SOF_GL(void, AttachShader, GLuint, GLuint);
      SOF_GL(void, LinkProgram, GLuint);
      SOF_GL(void, GetProgramiv, GLuint, GLenum, GLint*);
      SOF_GL(void, GetProgramInfoLog, GLuint, GLsizei, GLsizei*, char*);
      SOF_GL(void, DeleteProgram, GLuint);
      SOF_GL(void, UseProgram, GLuint);
      SOF_GL(GLint, GetUniformLocation, GLuint, const char*);
      SOF_GL(void, UniformMatrix4fv, GLint, GLsizei, GLboolean, const GLfloat*);
      SOF_GL(void, GenVertexArrays, GLsizei, GLuint*);
      SOF_GL(void, BindVertexArray, GLuint);
      SOF_GL(void, DeleteVertexArrays, GLsizei, const GLuint*);
      SOF_GL(void, GenBuffers, GLsizei, GLuint*);
      SOF_GL(void, BindBuffer, GLenum, GLuint);
      SOF_GL(void, BufferData, GLenum, std::ptrdiff_t, const void*, GLenum);
      SOF_GL(void, DeleteBuffers, GLsizei, const GLuint*);
      SOF_GL(void, EnableVertexAttribArray, GLuint);
      SOF_GL(void, VertexAttribPointer, GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
      SOF_GL(void, GenFramebuffers, GLsizei, GLuint*);
      SOF_GL(void, BindFramebuffer, GLenum, GLuint);
      SOF_GL(void, DeleteFramebuffers, GLsizei, const GLuint*);
      SOF_GL(void, FramebufferTexture2D, GLenum, GLenum, GLenum, GLuint, GLint);
      SOF_GL(GLenum, CheckFramebufferStatus, GLenum);
      SOF_GL(void, GenRenderbuffers, GLsizei, GLuint*);
      SOF_GL(void, BindRenderbuffer, GLenum, GLuint);
      SOF_GL(void, RenderbufferStorage, GLenum, GLenum, GLsizei, GLsizei);
      SOF_GL(void, FramebufferRenderbuffer, GLenum, GLenum, GLenum, GLuint);
      SOF_GL(void, DeleteRenderbuffers, GLsizei, const GLuint*);
#undef SOF_GL

      template <typename Function>
      static Function load(const char* name)
      {
        const auto function = glfwGetProcAddress(name);
        if (!function)
          throw std::runtime_error(std::string("Missing OpenGL function: ") + name);
        return reinterpret_cast<Function>(function);
      }
    };

    GLuint compile(const Gl& gl, GLenum type, const char* source)
    {
      const GLuint shader = gl.CreateShader(type);
      gl.ShaderSource(shader, 1, &source, nullptr);
      gl.CompileShader(shader);
      GLint success {};
      gl.GetShaderiv(shader, compile_status, &success);
      if (!success)
      {
        std::array<char, 2048> message {};
        gl.GetShaderInfoLog(shader, static_cast<GLsizei>(message.size()), nullptr, message.data());
        gl.DeleteShader(shader);
        throw std::runtime_error(std::string("Head shader: ") + message.data());
      }
      return shader;
    }

    // Rendering happens before the ImGui draw pass; preserve the surrounding GL state.
    struct SavedState
    {
      const Gl& gl;
      GLint fbo {}, rbo {}, program {}, vao {}, buffer {}, texture {}, depth_function {};
      std::array<GLint, 4> viewport {};
      std::array<GLfloat, 4> clear_color {};
      GLboolean depth_write {};
      const std::array<GLenum, 4> capabilities { GL_DEPTH_TEST, GL_BLEND, GL_CULL_FACE, GL_SCISSOR_TEST };
      std::array<GLboolean, 4> enabled {};

      explicit SavedState(const Gl& functions) : gl(functions)
      {
        glGetIntegerv(framebuffer_binding, &fbo);
        glGetIntegerv(renderbuffer_binding, &rbo);
        glGetIntegerv(current_program, &program);
        glGetIntegerv(vertex_array_binding, &vao);
        glGetIntegerv(array_buffer_binding, &buffer);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        glGetIntegerv(GL_VIEWPORT, viewport.data());
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_color.data());
        glGetIntegerv(GL_DEPTH_FUNC, &depth_function);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_write);
        for (std::size_t i = 0; i < capabilities.size(); ++i)
          enabled[i] = glIsEnabled(capabilities[i]);
      }

      ~SavedState()
      {
        gl.BindFramebuffer(framebuffer, static_cast<GLuint>(fbo));
        gl.BindRenderbuffer(renderbuffer, static_cast<GLuint>(rbo));
        gl.UseProgram(static_cast<GLuint>(program));
        gl.BindVertexArray(static_cast<GLuint>(vao));
        gl.BindBuffer(array_buffer, static_cast<GLuint>(buffer));
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture));
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glClearColor(clear_color[0], clear_color[1], clear_color[2], clear_color[3]);
        glDepthFunc(static_cast<GLenum>(depth_function));
        glDepthMask(depth_write);
        for (std::size_t i = 0; i < capabilities.size(); ++i)
          if (enabled[i])
            glEnable(capabilities[i]);
          else
            glDisable(capabilities[i]);
      }
    };
  }  // namespace

  struct HeadPreview::Impl
  {
    HeadModel model;
    Gl gl;
    GLuint program {}, vao {}, vertices {}, indices {}, fbo {}, color {}, depth {};
    GLint projection {};
    GLsizei index_count {};
    int width {}, height {};

    explicit Impl(const std::filesystem::path& path) : model(path) {}

    ~Impl()
    {
      gl.DeleteRenderbuffers(1, &depth);
      glDeleteTextures(1, &color);
      gl.DeleteFramebuffers(1, &fbo);
      gl.DeleteBuffers(1, &indices);
      gl.DeleteBuffers(1, &vertices);
      gl.DeleteVertexArrays(1, &vao);
      if (program)
        gl.DeleteProgram(program);
    }

    void initialize()
    {
      const SavedState saved(gl);
      if (model.indices().size() > static_cast<std::size_t>(std::numeric_limits<GLsizei>::max()))
        throw std::runtime_error("Too many head triangles to draw");
      index_count = static_cast<GLsizei>(model.indices().size());
      const char* vertex_source = R"(#version 330 core
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
uniform mat4 projection;
out vec3 geometry_normal;
void main() {
    gl_Position = projection * vec4(position, 1.0);
    geometry_normal = normal;
}
)";
      const char* fragment_source = R"(#version 330 core
in vec3 geometry_normal;
out vec4 color;
void main() {
    float gray = max(0.0, geometry_normal.z);
    color = vec4(vec3(gray), 1.0);
}
)";
      const GLuint vertex = compile(gl, vertex_shader, vertex_source);
      GLuint fragment {};
      try
      {
        fragment = compile(gl, fragment_shader, fragment_source);
        program = gl.CreateProgram();
        gl.AttachShader(program, vertex);
        gl.AttachShader(program, fragment);
        gl.LinkProgram(program);
      }
      catch (...)
      {
        gl.DeleteShader(vertex);
        if (fragment)
          gl.DeleteShader(fragment);
        throw;
      }
      gl.DeleteShader(vertex);
      gl.DeleteShader(fragment);
      GLint success {};
      gl.GetProgramiv(program, link_status, &success);
      if (!success)
      {
        std::array<char, 2048> message {};
        gl.GetProgramInfoLog(program, static_cast<GLsizei>(message.size()), nullptr, message.data());
        throw std::runtime_error(std::string("Head shader link: ") + message.data());
      }
      projection = gl.GetUniformLocation(program, "projection");
      gl.GenVertexArrays(1, &vao);
      gl.BindVertexArray(vao);
      gl.GenBuffers(1, &vertices);
      gl.BindBuffer(array_buffer, vertices);
      gl.EnableVertexAttribArray(0);
      gl.EnableVertexAttribArray(1);
      gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(HeadVertex), nullptr);
      gl.VertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(HeadVertex),
        // NOLINTNEXTLINE(performance-no-int-to-ptr): OpenGL encodes byte offsets as pointers.
        reinterpret_cast<const void*>(offsetof(HeadVertex, normal)));
      gl.GenBuffers(1, &indices);
      gl.BindBuffer(element_buffer, indices);
      gl.BufferData(element_buffer, static_cast<std::ptrdiff_t>(model.indices().size_bytes()),
        model.indices().data(), static_draw);
      gl.GenFramebuffers(1, &fbo);
      gl.BindFramebuffer(framebuffer, fbo);
      glGenTextures(1, &color);
      glBindTexture(GL_TEXTURE_2D, color);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, clamp_to_edge);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, clamp_to_edge);
      gl.FramebufferTexture2D(framebuffer, color_attachment, GL_TEXTURE_2D, color, 0);
      gl.GenRenderbuffers(1, &depth);
      gl.BindRenderbuffer(renderbuffer, depth);
      gl.FramebufferRenderbuffer(framebuffer, depth_attachment, renderbuffer, depth);
    }

    unsigned int render(std::span<const Parameter> parameters, int requested_width, int requested_height)
    {
      const SavedState saved(gl);
      gl.BindFramebuffer(framebuffer, fbo);
      requested_width = std::clamp(requested_width, 64, 1024);
      requested_height = std::clamp(requested_height, 32, 1024);
      if (requested_width != width || requested_height != height)
      {
        width = requested_width;
        height = requested_height;
        glBindTexture(GL_TEXTURE_2D, color);
        glTexImage2D(GL_TEXTURE_2D, 0, rgba8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        gl.BindRenderbuffer(renderbuffer, depth);
        gl.RenderbufferStorage(renderbuffer, depth_component24, width, height);
        if (gl.CheckFramebufferStatus(framebuffer) != framebuffer_complete)
          throw std::runtime_error("Cannot create head preview framebuffer");
      }
      model.deform(parameters);
      glViewport(0, 0, width, height);
      glEnable(GL_DEPTH_TEST);
      glDepthFunc(GL_LESS);
      glDepthMask(GL_TRUE);
      glDisable(GL_BLEND);
      glDisable(GL_CULL_FACE);
      glDisable(GL_SCISSOR_TEST);
      glClearColor(0.08F, 0.09F, 0.11F, 1.0F);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      gl.UseProgram(program);
      // Orthographic camera looking along +Z; Y=0 maps to the top clip plane (Y=1).
      const float aspect = static_cast<float>(width) / static_cast<float>(height);
      const std::array<float, 16> matrix { 1, 0, 0, 0, 0, aspect, 0, 0, 0, 0, -0.25F, 0, 0, 1, 0, 1 };
      gl.UniformMatrix4fv(projection, 1, GL_FALSE, matrix.data());
      gl.BindVertexArray(vao);
      gl.BindBuffer(array_buffer, vertices);
      gl.BufferData(array_buffer, static_cast<std::ptrdiff_t>(model.vertices().size_bytes()),
        model.vertices().data(), dynamic_draw);
      glDrawElements(GL_TRIANGLES, index_count, GL_UNSIGNED_INT, nullptr);
      return color;
    }
  };

  HeadPreview::HeadPreview(const std::filesystem::path& path) : impl_(std::make_unique<Impl>(path))
  {
    impl_->initialize();
  }

  HeadPreview::~HeadPreview() = default;

  unsigned int HeadPreview::render(std::span<const Parameter> parameters, int width, int height)
  {
    return impl_->render(parameters, width, height);
  }
}  // namespace sof
