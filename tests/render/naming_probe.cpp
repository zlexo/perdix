// Records one command that breaks a rule about a named buffer, so that the
// validation layer reports it, for CTest to read: the message must name the
// buffer as the engine did (RFC 0001), not only give its handle. A program of
// its own, since every other Vulkan test fails on any validation message.

#include "one_shot.hpp"

#include <vx/render/buffer.hpp>
#include <vx/render/context.hpp>

#include <volk.h>

#include <iostream>
#include <optional>

int main() {
    vx::Result<vx::render::Context> context =
        vx::render::Context::create({.surface = std::nullopt, .validation = true});
    if (!context.has_value()) {
        std::cerr << "error: " << context.error().message << '\n';
        return 1;
    }
    const vx::render::Device& device = context.value().device();
    const vx::Result<vx::render::Buffer> buffer =
        vx::render::Buffer::create(device, 64, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   vx::render::BufferMemory::device_local, "named probe buffer");
    if (!buffer.has_value()) {
        std::cerr << "error: " << buffer.error().message << '\n';
        return 1;
    }
    // A fill must start at a multiple of 4 bytes
    // (VUID-vkCmdFillBuffer-dstOffset-00025). Recorded, never submitted.
    const vx::test::OneShot shot(device);
    vkCmdFillBuffer(shot.commands(), buffer.value().handle(), 2, 16, 0);
    return 0;
}
