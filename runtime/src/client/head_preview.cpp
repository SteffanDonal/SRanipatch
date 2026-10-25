#include "head_preview.hpp"

#include "head_model.hpp"

#include <glad/gl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>

namespace sranipatch
{
  namespace
  {
    GLuint compile(GLenum type, const char* source)
    {
      const GLuint shader = glCreateShader(type);
      glShaderSource(shader, 1, &source, nullptr);
      glCompileShader(shader);
      GLint success {};
      glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
      if (!success)
      {
        std::array<char, 2048> message {};
        glGetShaderInfoLog(shader, static_cast<GLsizei>(message.size()), nullptr, message.data());
        glDeleteShader(shader);
        throw std::runtime_error(std::string("Head shader: ") + message.data());
      }
      return shader;
    }

    // Rendering happens before the ImGui draw pass; preserve the surrounding GL state.
    struct SavedState
    {
      GLint fbo {}, rbo {}, program {}, vao {}, buffer {}, texture {}, depth_function {};
      std::array<GLint, 4> viewport {};
      std::array<GLfloat, 4> clear_color {};
      GLboolean depth_write {};
      const std::array<GLenum, 4> capabilities { GL_DEPTH_TEST, GL_BLEND, GL_CULL_FACE, GL_SCISSOR_TEST };
      std::array<GLboolean, 4> enabled {};

      SavedState()
      {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
        glGetIntegerv(GL_RENDERBUFFER_BINDING, &rbo);
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer);
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
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(fbo));
        glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(rbo));
        glUseProgram(static_cast<GLuint>(program));
        glBindVertexArray(static_cast<GLuint>(vao));
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(buffer));
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
    GLuint program {}, vao {}, vertices {}, indices {}, fbo {}, color {}, depth {};
    GLint projection {};
    GLsizei index_count {};
    int width {}, height {};

    explicit Impl(const std::filesystem::path& path) : model(path) {}

    ~Impl()
    {
      glDeleteRenderbuffers(1, &depth);
      glDeleteTextures(1, &color);
      glDeleteFramebuffers(1, &fbo);
      glDeleteBuffers(1, &indices);
      glDeleteBuffers(1, &vertices);
      glDeleteVertexArrays(1, &vao);
      if (program)
        glDeleteProgram(program);
    }

    void initialize()
    {
      const SavedState saved;
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
      const GLuint vertex = compile(GL_VERTEX_SHADER, vertex_source);
      GLuint fragment {};
      try
      {
        fragment = compile(GL_FRAGMENT_SHADER, fragment_source);
        program = glCreateProgram();
        glAttachShader(program, vertex);
        glAttachShader(program, fragment);
        glLinkProgram(program);
      }
      catch (...)
      {
        glDeleteShader(vertex);
        if (fragment)
          glDeleteShader(fragment);
        throw;
      }
      glDeleteShader(vertex);
      glDeleteShader(fragment);
      GLint success {};
      glGetProgramiv(program, GL_LINK_STATUS, &success);
      if (!success)
      {
        std::array<char, 2048> message {};
        glGetProgramInfoLog(program, static_cast<GLsizei>(message.size()), nullptr, message.data());
        throw std::runtime_error(std::string("Head shader link: ") + message.data());
      }
      projection = glGetUniformLocation(program, "projection");
      glGenVertexArrays(1, &vao);
      glBindVertexArray(vao);
      glGenBuffers(1, &vertices);
      glBindBuffer(GL_ARRAY_BUFFER, vertices);
      glEnableVertexAttribArray(0);
      glEnableVertexAttribArray(1);
      glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(HeadVertex), nullptr);
      glVertexAttribPointer(
        1, 3, GL_FLOAT, GL_FALSE, sizeof(HeadVertex),
        // NOLINTNEXTLINE(performance-no-int-to-ptr): OpenGL encodes byte offsets as pointers.
        reinterpret_cast<const void*>(offsetof(HeadVertex, normal))
      );
      glGenBuffers(1, &indices);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices);
      glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(model.indices().size_bytes()),
        model.indices().data(), GL_STATIC_DRAW);
      glGenFramebuffers(1, &fbo);
      glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      glGenTextures(1, &color);
      glBindTexture(GL_TEXTURE_2D, color);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
      glGenRenderbuffers(1, &depth);
      glBindRenderbuffer(GL_RENDERBUFFER, depth);
      glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    }

    unsigned int render(std::span<const Parameter> parameters, int requested_width, int requested_height)
    {
      const SavedState saved;
      glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      requested_width = std::clamp(requested_width, 64, 1024);
      requested_height = std::clamp(requested_height, 32, 1024);
      if (requested_width != width || requested_height != height)
      {
        width = requested_width;
        height = requested_height;
        glBindTexture(GL_TEXTURE_2D, color);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindRenderbuffer(GL_RENDERBUFFER, depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
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
      glUseProgram(program);
      // Orthographic camera looking along +Z; Y=0 maps to the top clip plane (Y=1).
      const float aspect = static_cast<float>(width) / static_cast<float>(height);
      const std::array<float, 16> matrix { 1, 0, 0, 0, 0, aspect, 0, 0, 0, 0, -0.25F, 0, 0, 1, 0, 1 };
      glUniformMatrix4fv(projection, 1, GL_FALSE, matrix.data());
      glBindVertexArray(vao);
      glBindBuffer(GL_ARRAY_BUFFER, vertices);
      glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(model.vertices().size_bytes()),
        model.vertices().data(), GL_DYNAMIC_DRAW);
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
}  // namespace sranipatch
