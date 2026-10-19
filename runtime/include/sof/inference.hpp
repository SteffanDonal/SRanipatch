#pragma once
#include "sof/frame.hpp"
#include "sof/protocol.hpp"

#include <memory>
#include <span>

namespace sof
{
  enum class InferencePrecision
  {
    normal,
    low
  };
  enum class MedianFilter
  {
    exact,
    pseudo_separable
  };

  class Inference
  {
  public:
    virtual ~Inference() = default;
    virtual std::uint32_t run(const Frame& frame, std::array<Parameter, max_parameters>& output,
      std::span<float> raw_output = {}) = 0;
  };

  std::unique_ptr<Inference> open_lip_mnn(const Config& config,
    InferencePrecision precision = InferencePrecision::normal,
    MedianFilter filter = MedianFilter::exact);
}  // namespace sof
