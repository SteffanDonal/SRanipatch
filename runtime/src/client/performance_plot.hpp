#ifndef SRANIPATCH_PERFORMANCE_PLOT_HPP
#define SRANIPATCH_PERFORMANCE_PLOT_HPP
#include "observation.hpp"

#include <array>
#include <deque>

namespace sranipatch
{
  class PerformancePlot
  {
  public:
    void update(const ObservationSnapshot& observation, std::uint64_t now);
    void draw(const Status& status) const;

  private:
    struct Sample
    {
      double time {};
      std::array<double, 3> values {};
    };

    struct Series
    {
      std::deque<Sample> samples;
      std::uint64_t counter {};
      bool gap { true };
    };

    Series capture_, inference_;
    std::uint64_t started_at_ {};
    double elapsed_ {};
  };
}
#endif
