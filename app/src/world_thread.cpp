#include "world_thread.hpp"

#include "world_loop.hpp"

#include <vx/core/thread.hpp>

#include <chrono>
#include <thread>

namespace vx::app {

WorldThread::WorldThread(WorldLoop& loop)
    : loop_(&loop), thread_([&loop] {
          set_thread_name("World");
          set_thread_qos(ThreadQos::user_interactive);
          for (;;) {
              const std::chrono::steady_clock::time_point next =
                  loop.step(std::chrono::steady_clock::now());
              if (loop.stopping()) {
                  return;
              }
              loop.wake().wait_until(next);
          }
      }) {}

WorldThread::~WorldThread() {
    loop_->to_world().push({.kind = ToWorld::Kind::stop,
                            .camera = {},
                            .block = {},
                            .state = BlockState::air,
                            .uploaded = 0});
    thread_.join();
}

} // namespace vx::app
