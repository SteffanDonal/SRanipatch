#include "observation.hpp"

#include <exception>
#include <utility>

namespace sranipatch
{
  namespace
  {
    constexpr std::uint64_t freshness_ns = 250'000'000;

    bool recent(std::uint64_t timestamp, std::uint64_t now, std::uint64_t timeout)
    {
      return timestamp && now >= timestamp && now - timestamp < timeout;
    }
  }  // namespace

  Observation::Observation(std::string name, std::function<std::uint64_t()> clock)
    : name_(std::move(name)), clock_(std::move(clock)) {}

  const ObservationSnapshot& Observation::update()
  {
    auto now = clock_();
    snapshot_.session_changed = false;
    snapshot_.frame_changed = false;
    if (!ipc_ && now >= retry_at_)
    {
      retry_at_ = now + 1'000'000'000;
      try
      {
        ipc_ = std::make_unique<SharedMemory>(name_, SharedMemory::Role::client);
      }
      catch (const std::exception& error)
      {
        if (snapshot_.message.empty())
          snapshot_.message = error.what();
      }
    }
    if (ipc_)
    {
      try
      {
        Status status;
        if (ipc_->read(status))
        {
          now = clock_();
          if (status.session_id != snapshot_.status.session_id)
          {
            snapshot_.session_changed = true;
            snapshot_.status = {};
            snapshot_.frame = {};
            camera_at_ = tracking_at_ = 0;
          }
          if (status.parameter_count && status.frames_inferred && status.frames_inferred != snapshot_.status.frames_inferred)
            tracking_at_ = now;
          snapshot_.status = status;
        }
        if (ipc_->read_frame(snapshot_.frame))
        {
          snapshot_.frame_changed = true;
          camera_at_ = clock_();
        }
        now = clock_();
        const auto& current = snapshot_.status;
        if (current.state == ServerState::waiting_for_steam_link)
        {
          snapshot_.frame = {};
          snapshot_.frame_changed = false;
          camera_at_ = tracking_at_ = 0;
        }
        snapshot_.available = recent(current.heartbeat_ns, now, 2'000'000'000) && current.state != ServerState::stopped && current.state != ServerState::fault;
        if (snapshot_.available)
          snapshot_.message.clear();
        else
        {
          if (current.error[0])
            snapshot_.message = current.error.data();
          else if (snapshot_.message.empty())
            snapshot_.message = "Server is offline or its heartbeat is stale";
          ipc_.reset();
          retry_at_ = now + 1'000'000'000;
        }
      }
      catch (const std::exception& error)
      {
        snapshot_.message = error.what();
        snapshot_.available = false;
        ipc_.reset();
        retry_at_ = clock_() + 1'000'000'000;
      }
    }
    snapshot_.camera_fresh = snapshot_.available && recent(camera_at_, now, freshness_ns);
    snapshot_.tracking_fresh = snapshot_.available && recent(tracking_at_, now, freshness_ns);
    return snapshot_;
  }
}  // namespace sranipatch
