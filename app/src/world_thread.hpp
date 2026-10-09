#pragma once

#include "world_loop.hpp"

#include <thread>

namespace vx::app {

// RFC 0005's world thread: runs a world loop, sleeping until its next tick
// or a message, named World, at user-interactive quality of service, so it
// never queues behind the workers it feeds.
class WorldThread {
public:
    explicit WorldThread(WorldLoop& loop);
    WorldThread(const WorldThread&) = delete;
    WorldThread& operator=(const WorldThread&) = delete;
    WorldThread(WorldThread&&) = delete;
    WorldThread& operator=(WorldThread&&) = delete;
    // Tells the loop to stop and joins the thread; the loop, still alive,
    // waits for its jobs when it goes.
    ~WorldThread();

private:
    WorldLoop* loop_;
    std::thread thread_;
};

} // namespace vx::app
