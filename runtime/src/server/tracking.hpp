#pragma once
#include "sranipatch/inference.hpp"

#include <atomic>

namespace sranipatch
{
  class Tracking
  {
  public:
    Tracking(Config config, InferencePrecision precision);
    ~Tracking();
    bool update(bool active, Status& status, Frame& frame);

  private:
    struct CaptureRun;
    Config config_;
    InferencePrecision precision_;
    std::unique_ptr<Inference> inference_;
    std::atomic<std::uint64_t> captured_ {};
    std::uint64_t last_sequence_ {};
    std::unique_ptr<CaptureRun> capture_;
  };
}  // namespace sranipatch
