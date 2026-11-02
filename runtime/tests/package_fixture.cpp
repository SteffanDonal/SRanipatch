#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char** argv)
try
{
  if (argc > 1 && std::string(argv[1]) == "status")
  {
    std::cout << "{\"steamvr\":false,\"active\":false,\"run\":false,\"managed\":false,\"revision\":0}\n";
    return 0;
  }
  if (argc == 3 && std::string(argv[1]) == "--record")
  {
    std::ofstream record(argv[2], std::ios::app);
    record << "started\n";
    record.close();
    for (;;)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return 0;
}
catch (...)
{
  return 1;
}
