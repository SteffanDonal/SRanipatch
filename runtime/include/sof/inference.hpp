#pragma once
#include "sof/frame.hpp"
#include "sof/protocol.hpp"

#include <memory>
#include <string>

namespace sof
{
  class Inference
  {
  public:
    virtual ~Inference() = default;
    virtual std::uint32_t run(const Frame& frame, std::array<Parameter, max_parameters>& output) = 0;
  };

  // Generic single-image adapter; model-specific preprocessing and parameter naming
  // belong here once the eye/lip model contracts are established.
  std::unique_ptr<Inference> open_mnn(const Config& config, const std::string& input_name,
    const std::string& output_name);
}  // namespace sof
