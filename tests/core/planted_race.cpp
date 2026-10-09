// A planted data race for ThreadSanitizer to report (RFC 0005): two threads
// add to one counter with nothing between them. Built only by the TSan
// preset, where its test passes only when the report appears.

#include <cstdio>
#include <thread>

namespace {

int counter = 0; // what both threads add to

void count() {
    for (int i = 0; i < 100'000; ++i) {
        ++counter;
    }
}

} // namespace

int main() {
    std::thread first(count);
    std::thread second(count);
    first.join();
    second.join();
    std::printf("%d\n", counter);
    return 0;
}
