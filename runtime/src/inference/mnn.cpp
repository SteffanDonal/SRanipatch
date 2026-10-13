#include "sof/inference.hpp"

#include <MNN/Interpreter.hpp>
#include <MNN/Tensor.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sof
{
  namespace
  {
    struct InterpreterDeleter
    {
      void operator()(MNN::Interpreter* pointer) const
      {
        MNN::Interpreter::destroy(pointer);
      }
    };

    struct TensorDeleter
    {
      void operator()(MNN::Tensor* pointer) const
      {
        MNN::Tensor::destroy(pointer);
      }
    };

    using HostTensor = std::unique_ptr<MNN::Tensor, TensorDeleter>;

    bool is_float32(const MNN::Tensor& tensor)
    {
      const auto type = tensor.getType();
      return type.code == halide_type_float && type.bits == 32 && type.lanes == 1;
    }

    class Mnn final : public Inference
    {
      std::unique_ptr<MNN::Interpreter, InterpreterDeleter> interpreter_;
      MNN::Session* session_ {};
      MNN::Tensor* input_ {};
      MNN::Tensor* output_ {};
      HostTensor input_host_, output_host_;
      int width_ {}, height_ {};
      float mean_ {}, scale_ {};
      std::array<std::array<char, 48>, max_parameters> names_ {};

      struct Coordinate
      {
        std::uint32_t first {}, second {};
        float weight {};
      };

      std::vector<Coordinate> x_, y_;
      std::uint32_t source_width_ {}, source_height_ {};

      static void coordinates(std::vector<Coordinate>& dest, int size, std::uint32_t source)
      {
        dest.resize(static_cast<std::size_t>(size));
        for (int i = 0; i < size; ++i)
        {
          const float value = std::clamp(
            (static_cast<float>(i) + 0.5F) * static_cast<float>(source) / static_cast<float>(size) - 0.5F,
            0.0F, static_cast<float>(source - 1)
          );
          const auto first = static_cast<std::uint32_t>(value);
          dest[static_cast<std::size_t>(i)] = { first, std::min(first + 1, source - 1),
            value - static_cast<float>(first) };
        }
      }

    public:
      Mnn(const Config& config, const std::string& input_name, const std::string& output_name)
        : mean_(config.mean), scale_(config.scale)
      {
        interpreter_.reset(MNN::Interpreter::createFromFile(config.model.data()));
        if (!interpreter_)
          throw std::runtime_error("Cannot load .mnn model (JSON/NPZ weights need conversion)");
        MNN::ScheduleConfig schedule;
        schedule.type = MNN_FORWARD_CPU;
        schedule.numThread = static_cast<int>(config.threads);
        MNN::BackendConfig backend;
        backend.precision = MNN::BackendConfig::Precision_Normal;
        schedule.backendConfig = &backend;
        session_ = interpreter_->createSession(schedule);
        if (!session_)
          throw std::runtime_error("MNN could not create a CPU inference session");
        if (interpreter_->getSessionInputAll(session_).size() != 1)
          throw std::runtime_error(
            "Model requires multiple inputs; implement its contract in the inference adapter"
          );
        if (output_name.empty() && interpreter_->getSessionOutputAll(session_).size() != 1)
          throw std::runtime_error("Select --output for a model with multiple outputs");
        input_ = interpreter_->getSessionInput(session_, input_name.empty() ? nullptr : input_name.c_str());
        output_ =
          interpreter_->getSessionOutput(session_, output_name.empty() ? nullptr : output_name.c_str());
        if (!input_ || !output_)
          throw std::runtime_error("Named MNN input or output was not found");
        if (input_->dimensions() != 4 || input_->batch() != 1 || input_->channel() != 3 || !is_float32(*input_))
          throw std::runtime_error(
            "Generic adapter requires a fixed float32 [1,3,H,W] or [1,H,W,3] RGB input"
          );
        width_ = input_->width();
        height_ = input_->height();
        if (width_ <= 0 || height_ <= 0 || width_ > 8192 || height_ > 8192)
          throw std::runtime_error("Invalid or dynamic model dimensions; implement its resize contract");
        if (!is_float32(*output_))
          throw std::runtime_error("Generic adapter requires float32 output");
        input_host_.reset(new MNN::Tensor(input_, MNN::Tensor::CAFFE));
        output_host_.reset(new MNN::Tensor(output_, MNN::Tensor::CAFFE));
        if (!input_host_ || !input_host_->host<float>() || !output_host_ || !output_host_->host<float>())
          throw std::runtime_error("MNN host tensor allocation failed");
        if (output_host_->elementSize() <= 0 || output_host_->elementSize() > static_cast<int>(max_parameters))
          throw std::runtime_error(
            "Selected output exceeds the 128-parameter protocol; implement a model-specific projection"
          );
        for (int i = 0; i < output_host_->elementSize(); ++i)
          set_text(names_[static_cast<std::size_t>(i)], "parameter_" + std::to_string(i));
        interpreter_->releaseModel();
      }

      std::uint32_t run(const Frame& frame, std::array<Parameter, max_parameters>& output) override
      {
        if (!frame.width || !frame.height || frame.rgb.size() != static_cast<std::size_t>(frame.width) * frame.height * 3)
          throw std::runtime_error("Invalid RGB frame");
        if (source_width_ != frame.width || source_height_ != frame.height)
        {
          coordinates(x_, width_, frame.width);
          coordinates(y_, height_, frame.height);
          source_width_ = frame.width;
          source_height_ = frame.height;
        }
        auto* dest = input_host_->host<float>();
        const auto plane = static_cast<std::size_t>(width_) * height_;
        for (int y = 0; y < height_; ++y)
        {
          const auto cy = y_[static_cast<std::size_t>(y)];
          for (int x = 0; x < width_; ++x)
          {
            const auto cx = x_[static_cast<std::size_t>(x)];
            const auto index = static_cast<std::size_t>(y) * width_ + x;
            const auto p00 = (static_cast<std::size_t>(cy.first) * frame.width + cx.first) * 3;
            const auto p01 = (static_cast<std::size_t>(cy.first) * frame.width + cx.second) * 3;
            const auto p10 = (static_cast<std::size_t>(cy.second) * frame.width + cx.first) * 3;
            const auto p11 = (static_cast<std::size_t>(cy.second) * frame.width + cx.second) * 3;
            for (std::size_t c = 0; c < 3; ++c)
            {
              const float top = std::lerp(static_cast<float>(frame.rgb[p00 + c]),
                static_cast<float>(frame.rgb[p01 + c]), cx.weight);
              const float bottom = std::lerp(static_cast<float>(frame.rgb[p10 + c]),
                static_cast<float>(frame.rgb[p11 + c]), cx.weight);
              dest[c * plane + index] = (std::lerp(top, bottom, cy.weight) - mean_) * scale_;
            }
          }
        }
        if (!input_->copyFromHostTensor(input_host_.get()))
          throw std::runtime_error("MNN input upload failed");
        if (interpreter_->runSession(session_) != MNN::NO_ERROR)
          throw std::runtime_error("MNN inference failed");
        if (!output_->copyToHostTensor(output_host_.get()))
          throw std::runtime_error("MNN output download failed");
        const auto count = static_cast<std::uint32_t>(output_host_->elementSize());
        for (std::uint32_t i = 0; i < count; ++i)
        {
          const auto value = output_host_->host<float>()[i];
          if (!std::isfinite(value))
            throw std::runtime_error("Model returned a non-finite parameter");
          output[i].name = names_[i];
          output[i].value = value;
        }
        return count;
      }
    };
  }  // namespace

  std::unique_ptr<Inference> open_mnn(const Config& config, const std::string& input_name,
    const std::string& output_name)
  {
    return std::make_unique<Mnn>(config, input_name, output_name);
  }
}  // namespace sof
