#include "sranipatch/inference.hpp"
#include "sranipatch/lip.hpp"

#include <MNN/Interpreter.hpp>
#include <MNN/Tensor.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace sranipatch
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
      std::array<std::array<char, 48>, max_parameters> names_ {};
      LipPostprocessor postprocessor_;
      std::vector<std::uint8_t> filtered_;
      std::vector<std::uint8_t> filter_scratch_;

    public:
      Mnn(const Config& config, InferencePrecision precision)
      {
        interpreter_.reset(MNN::Interpreter::createFromFile(config.model.data()));
        if (!interpreter_)
          throw std::runtime_error("Cannot load .mnn model");
        MNN::ScheduleConfig schedule;
        schedule.type = MNN_FORWARD_CPU;
        schedule.numThread = static_cast<int>(config.threads);
        MNN::BackendConfig backend;
        backend.precision = precision == InferencePrecision::low ? MNN::BackendConfig::Precision_Low
                                                                 : MNN::BackendConfig::Precision_Normal;
        schedule.backendConfig = &backend;
        session_ = interpreter_->createSession(schedule);
        if (!session_)
          throw std::runtime_error("MNN could not create a CPU inference session");
        if (interpreter_->getSessionInputAll(session_).size() != 1 || interpreter_->getSessionOutputAll(session_).size() != 1)
          throw std::runtime_error("Lip model must have exactly one input and one output");
        input_ = interpreter_->getSessionInput(session_, "stereo");
        output_ = interpreter_->getSessionOutput(session_, "lip_raw");
        if (!input_ || !output_)
          throw std::runtime_error("Lip model requires tensors named stereo and lip_raw");
        if (input_->dimensions() != 4 || input_->batch() != 1 || input_->channel() != 2 || input_->width() != 100 || input_->height() != 100 || !is_float32(*input_))
          throw std::runtime_error("Lip model input must be float32 NCHW [1,2,100,100]");
        if (!is_float32(*output_) || output_->elementSize() != 30)
          throw std::runtime_error("Lip model output must contain 30 float32 values");
        input_host_.reset(new MNN::Tensor(input_, MNN::Tensor::CAFFE));
        output_host_.reset(new MNN::Tensor(output_, MNN::Tensor::CAFFE));
        if (!input_host_ || !input_host_->host<float>() || !output_host_ || !output_host_->host<float>())
          throw std::runtime_error("MNN host tensor allocation failed");
        for (std::size_t i = 0; i < lip_parameter_names.size(); ++i)
          set_text(names_[i], lip_parameter_names[i]);
        interpreter_->releaseModel();
      }

      std::uint32_t run(const Frame& frame, std::array<Parameter, max_parameters>& output,
        std::span<float> raw_output) override
      {
        if (frame.width != capture_width || frame.height != capture_height || frame.yuyv.size() != static_cast<std::size_t>(capture_width) * capture_height * 2)
          throw std::runtime_error("Lip inference requires a 400x400 native packed stereo frame");
        auto* dest = input_host_->host<float>();
        constexpr std::size_t plane = std::size_t { 100 } * 100;
        constexpr std::array<int, 4> cubic { -3, 19, 19, -3 };
        constexpr std::size_t stereo_width = std::size_t { capture_width } * 2;
        filtered_.resize(frame.yuyv.size());
        // SRanipal filters across the stereo seam before cropping either view.
        filter_scratch_.resize(frame.yuyv.size());
        pseudo_separable_median_5x5(frame.yuyv, filtered_, filter_scratch_, stereo_width, capture_height);
        for (std::size_t camera = 0; camera < 2; ++camera)
        {
          for (std::size_t y = 0; y < 100; ++y)
          {
            for (std::size_t x = 0; x < 100; ++x)
            {
              int sum {};
              for (std::size_t ky = 0; ky < cubic.size(); ++ky)
                for (std::size_t kx = 0; kx < cubic.size(); ++kx)
                  sum +=
                    cubic[ky] * cubic[kx] * filtered_[(y * 4 + ky) * stereo_width + camera * capture_width + x * 4 + kx];
              int pixel {};
              if (sum > 0)
              {
                pixel = sum >> 10;
                const auto remainder = sum & 1023;
                if (remainder > 512 || (remainder == 512 && (pixel & 1)))
                  ++pixel;
                pixel = std::min(pixel, 255);
              }
              dest[camera * plane + y * 100 + x] = static_cast<float>(pixel) / 255.0F;
            }
          }
        }
        if (!input_->copyFromHostTensor(input_host_.get()))
          throw std::runtime_error("MNN input upload failed");
        if (interpreter_->runSession(session_) != MNN::NO_ERROR)
          throw std::runtime_error("MNN inference failed");
        if (!output_->copyToHostTensor(output_host_.get()))
          throw std::runtime_error("MNN output download failed");
        std::array<float, lip_raw_count> raw {};
        std::copy_n(output_host_->host<float>(), raw.size(), raw.begin());
        for (const auto value : raw)
        {
          if (!std::isfinite(value))
            throw std::runtime_error("Model returned a non-finite parameter");
        }
        if (!raw_output.empty())
        {
          if (raw_output.size() != raw.size())
            throw std::runtime_error("Lip raw output must contain 30 values");
          std::copy(raw.begin(), raw.end(), raw_output.begin());
        }
        const auto values = postprocessor_.process(raw);
        for (std::size_t i = 0; i < values.size(); ++i)
        {
          output[i].name = names_[i];
          output[i].value = values[i];
        }
        return static_cast<std::uint32_t>(values.size());
      }
    };
  }  // namespace

  std::unique_ptr<Inference> open_lip_mnn(const Config& config, InferencePrecision precision)
  {
    return std::make_unique<Mnn>(config, precision);
  }
}  // namespace sranipatch
