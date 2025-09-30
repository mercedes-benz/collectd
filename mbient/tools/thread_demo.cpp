#include <array>
#include <atomic>
#include <iostream>
#include <thread>
#include <unistd.h>

std::atomic<bool> stop(false);

void work() {
  long long unsigned result = 0;
  while (!stop) {
    result++;
  }
}

int main() {
  const pid_t pid = getpid();
  std::cout << "This is process " << pid << "\n";

  std::array<std::thread, 2> threads;
  std::cout << "Launching " << threads.size() << " threads...\n";
  for (std::thread &t : threads) {
    t = std::thread(work);
  }

  const std::string command =
      "cat /proc/" + std::to_string(pid) + "/task/*/stat";

  std::cout << "Executing '" << command << "'\n";
  if (system(command.c_str())) {
    std::cerr << "Failed.\n";
    exit(1);
  }

  std::cout << "Stopping threads...\n";

  stop = true;
  for (std::thread &t : threads) {
    t.join();
  }

  std::cout << "Done.\n";

  return 0;
}
