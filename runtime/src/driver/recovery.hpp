#pragma once

#include <chrono>

namespace sranipatch
{
  class Recovery
  {
  public:
    using Clock = std::chrono::steady_clock;

    bool failed(Clock::time_point now)
    {
      if (healthy_since_ != Clock::time_point {} && now - healthy_since_ >= std::chrono::minutes(1))
        failures_ = 0;
      healthy_since_ = {};
      return ++failures_ < 2;
    }

    void healthy(Clock::time_point now)
    {
      if (healthy_since_ == Clock::time_point {})
        healthy_since_ = now;
    }

    void reset()
    {
      failures_ = 0;
      healthy_since_ = {};
    }

  private:
    unsigned failures_ {};
    Clock::time_point healthy_since_ {};
  };
}
