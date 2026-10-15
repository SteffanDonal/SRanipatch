#pragma once
#include "sof/frame.hpp"
#include "sof/protocol.hpp"

#include <memory>

namespace sof
{
  enum class InferencePrecision
  {
    normal,
    low
  };

  class Inference
  {
  public:
    virtual ~Inference() = default;
    virtual std::uint32_t run(const Frame& frame, std::array<Parameter, max_parameters>& output) = 0;
  };

  std::unique_ptr<Inference> open_lip_mnn(const Config& config,
    InferencePrecision precision = InferencePrecision::normal);
}  // namespace sof
