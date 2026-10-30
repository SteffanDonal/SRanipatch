#pragma once
#include "sranipatch/frame.hpp"
#include "sranipatch/protocol.hpp"

#include <cstdint>
#include <memory>
#include <span>

namespace sranipatch
{
  enum class InferencePrecision : std::uint8_t
  {
    normal,
    low
  };

  class Inference
  {
  public:
    virtual ~Inference() = default;
    virtual std::uint32_t run(const Frame& frame, std::array<Parameter, max_parameters>& output,
      std::span<float> raw_output = {}) = 0;
  };

  std::unique_ptr<Inference> open_lip_mnn(const Config& config,
    InferencePrecision precision = InferencePrecision::normal);
}
