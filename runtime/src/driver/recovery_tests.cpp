#include "recovery.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>

namespace
{
  void require(bool condition, const char* message)
  {
    if (!condition)
      throw std::runtime_error(message);
  }
}

// Exceptions from the body are caught below; clang-tidy misidentifies MSVC exception inheritance.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main()
{
  try
  {
    using namespace std::chrono_literals;
    const auto start = sranipatch::Recovery::Clock::time_point {} + 1s;
    sranipatch::Recovery recovery;
    require(recovery.failed(start), "First failure must permit one retry");
    recovery.healthy(start + 1s);
    require(!recovery.failed(start + 60s), "Less than one healthy minute must exhaust recovery");
    recovery.reset();
    require(recovery.failed(start), "Explicit retry must reset the failure count");
    recovery.healthy(start + 1s);
    require(recovery.failed(start + 61s), "A healthy minute must restore automatic recovery");
    require(!recovery.failed(start + 62s), "Another immediate failure must stop recovery");
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
