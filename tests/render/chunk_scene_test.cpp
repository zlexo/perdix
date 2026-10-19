// Needs a Vulkan loader and at least one driver: labeled "vulkan" in CTest.
// RFC 0007's image test: a seeded scene of a few hundred chunks streams into
// the arena through each frame's changes, within the upload budget, and draws
// the same with culling and merging on or off, and with uploads staged.
// Synchronization validation is on, and any message fails the test.

#include <vx/core/result.hpp>
#include <vx/mesh/mesh_chunk.hpp>
#include <vx/mesh/padded_chunk.hpp>
#include <vx/mesh/quad.hpp>
#include <vx/render/arena_upkeep.hpp>
#include <vx/render/block_textures.hpp>
#include <vx/render/camera.hpp>
#include <vx/render/chunk_arena.hpp>
#include <vx/render/chunk_renderer.hpp>
#include <vx/render/chunk_uploads.hpp>
#include <vx/render/draw_lists.hpp>
#include <vx/render/renderer.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/block_visual.hpp>
#include <vx/world/coordinates.hpp>

#include <doctest/doctest.h>
#include <glm/ext/vector_double3.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr vx::render::Extent image_size = {.width = 160, .height = 96};
constexpr vx::render::Color sky = {.r = 0.4F, .g = 0.6F, .b = 0.9F, .a = 1.0F};

using Rgba = std::array<std::uint8_t, 4>;

// Block texture array bytes, as build_block_textures lays them out: layer 0
// a magenta that no block here asks for, then a layer of each color, every
// texel the same but where alpha says otherwise: the cutout layer's left
// half is see-through.
std::vector<std::byte> layer_bytes(std::span<const Rgba> colors) {
    const std::size_t count = colors.size() + 1;
    std::vector<std::byte> bytes(count * vx::render::block_texture_layer_bytes);
    for (std::size_t level = 0; level < 5; ++level) {
        const std::size_t side = std::size_t{16} >> level;
        for (std::size_t layer = 0; layer < count; ++layer) {
            for (std::size_t texel = 0; texel < side * side; ++texel) {
                Rgba color = layer == 0 ? Rgba{255, 0, 255, 255} : colors[layer - 1];
                if (color[3] == 0 && texel % side >= side / 2) {
                    color[3] = 255; // the right half of a see-through layer is solid
                }
                const std::size_t at =
                    4 * ((count * vx::render::block_texture_level_start.at(level)) +
                         (layer * side * side) + texel);
                for (std::size_t channel = 0; channel < 4; ++channel) {
                    bytes[at + channel] = std::byte{color[channel]};
                }
            }
        }
    }
    return bytes;
}

constexpr std::array<Rgba, 3> colors = {Rgba{120, 120, 130, 255}, Rgba{60, 170, 60, 255},
                                        Rgba{200, 140, 40, 0}};

// Stone, grass and a cutout block, each with its own layer on every face.
std::vector<vx::BlockVisual> scene_visuals() {
    std::vector<vx::BlockVisual> visuals(4);
    for (std::uint16_t state = 1; state < 4; ++state) {
        visuals[state] = {.opacity = state == 3 ? vx::Opacity::cutout : vx::Opacity::opaque,
                          .cull_group = 0,
                          .texture = {state, state, state, state, state, state}};
    }
    return visuals;
}

std::uint32_t mix(std::uint32_t value) {
    value ^= value >> 16U;
    value *= 0x7FEB352DU;
    value ^= value >> 15U;
    value *= 0x846CA68BU;
    value ^= value >> 16U;
    return value;
}

std::uint32_t hash(int x, int y, int z, std::uint32_t seed) {
    return mix(static_cast<std::uint32_t>(x) * 0x9E3779B1U ^ mix(static_cast<std::uint32_t>(y)) ^
               mix(static_cast<std::uint32_t>(z) * 0x85EBCA77U + seed));
}

// The seeded world: terraces of ground around height 0, eight blocks a side,
// grass on top of stone with a few holes in it, and blocks floating above,
// stone and cutout, so every face direction shows from somewhere.
vx::BlockState block_at(int x, int y, int z) {
    const int ground = static_cast<int>(hash(x >> 3, 0, z >> 3, 1) % 12) - 6;
    if (y < ground - 1) {
        return hash(x, y, z, 2) % 256 == 0 ? vx::BlockState::air : static_cast<vx::BlockState>(1);
    }
    if (y == ground - 1) {
        return static_cast<vx::BlockState>(2);
    }
    if (hash(x, y, z, 3) % 300 == 0) {
        return static_cast<vx::BlockState>(hash(x, y, z, 4) % 2 == 0 ? 1 : 3);
    }
    return vx::BlockState::air;
}

// A 9 by 3 by 9 block of chunks around the origin, meshed: 243 chunks.
struct Scene {
    std::vector<std::vector<vx::Quad>> quads; // each chunk's, opaque then cutout
    std::vector<vx::render::ChunkMeshUpdate> updates;

    Scene() {
        const std::vector<vx::BlockVisual> visuals = scene_visuals();
        const auto padded = std::make_unique<vx::PaddedChunk>();
        vx::MeshScratch scratch;
        std::vector<std::pair<vx::ChunkPos, vx::ChunkMesh>> meshes;
        for (int cy = -1; cy <= 1; ++cy) {
            for (int cz = -4; cz <= 4; ++cz) {
                for (int cx = -4; cx <= 4; ++cx) {
                    for (int y = 0; y < vx::padded_size; ++y) {
                        for (int z = 0; z < vx::padded_size; ++z) {
                            for (int x = 0; x < vx::padded_size; ++x) {
                                padded->blocks[vx::padded_index(x, y, z)] = block_at(
                                    (cx * vx::chunk_size) + x - 1, (cy * vx::chunk_size) + y - 1,
                                    (cz * vx::chunk_size) + z - 1);
                            }
                        }
                    }
                    const vx::ChunkMesh mesh = vx::mesh_chunk(*padded, visuals, scratch);
                    const std::span<const vx::Quad> all = mesh.all();
                    quads.emplace_back(all.begin(), all.end());
                    meshes.emplace_back(vx::ChunkPos{.x = cx, .y = cy, .z = cz}, mesh);
                }
            }
        }
        // The meshes point into the scratch: each one's ranges move to its
        // own copy, at the same places.
        for (std::size_t chunk = 0; chunk < meshes.size(); ++chunk) {
            const vx::ChunkMesh& mesh = meshes[chunk].second;
            const vx::Quad* first = mesh.all().data();
            vx::ChunkMesh copy{};
            for (std::size_t pass = 0; pass < 2; ++pass) {
                for (std::size_t face = 0; face < 6; ++face) {
                    const std::span<const vx::Quad> range = mesh.ranges[pass][face];
                    const auto offset = static_cast<std::size_t>(range.data() - first);
                    copy.ranges[pass][face] =
                        std::span<const vx::Quad>(quads[chunk]).subspan(offset, range.size());
                }
            }
            updates.push_back({.pos = meshes[chunk].first, .generation = 1, .mesh = copy});
        }
    }
};

// What the main thread will do with the world thread's meshes: offer them
// in order every frame until the budget is spent, keep the rest for the
// next, and remove chunks as told.
class Offers final : public vx::render::ChunkChanges {
public:
    std::deque<vx::render::ChunkMeshUpdate> waiting;
    std::vector<vx::ChunkPos> removals;
    int frames_waited = 0; // frames that left meshes for the next

    vx::Result<void> apply(vx::render::ChunkUploader& uploader) override {
        for (const vx::ChunkPos pos : removals) {
            uploader.remove(pos);
        }
        removals.clear();
        while (!waiting.empty()) {
            const vx::Result<vx::render::Uploaded> uploaded = uploader.upload(waiting.front());
            if (!uploaded.has_value()) {
                return uploaded.error();
            }
            if (uploaded.value() == vx::render::Uploaded::later ||
                uploaded.value() == vx::render::Uploaded::full) {
                ++frames_waited;
                break;
            }
            waiting.pop_front();
        }
        return {};
    }
};

// In place, every pass's commands split into calls of 7; or staged, each
// pass one call.
vx::Result<vx::render::Renderer> scene_renderer(std::span<const std::byte> textures, bool staged) {
    return vx::render::Renderer::create({.surface = std::nullopt,
                                         .framebuffer = image_size,
                                         .validation = true,
                                         .present_mode = vx::render::PresentMode::fifo,
                                         .overlay = nullptr,
                                         .block_textures = textures,
                                         .chunk_arena_quads = std::uint32_t{1} << 22U,
                                         .stage_chunk_uploads = staged,
                                         .draw_call_limit = staged ? 0U : 7U});
}

// Above the ground at the scene's south edge, looking north and down over
// it: chunks behind and beside the view are culled.
vx::render::Camera scene_camera() {
    vx::render::Camera camera;
    camera.position = {0.5, 24.5, 100.5};
    camera.pitch = -0.35F;
    camera.yaw = 0.2F;
    return camera;
}

// Draws frames until every offer is in, then one more, and reads it back.
std::vector<std::uint8_t> draw_all(vx::render::Renderer& renderer, Offers& offers,
                                   const vx::render::Camera& camera) {
    const vx::render::FrameInputs inputs{
        .sky = sky, .camera = camera, .cubes = {}, .overlay = nullptr, .chunk_changes = &offers};
    for (int frame = 0; frame < 500 && !offers.waiting.empty(); ++frame) {
        const vx::Result<vx::render::Frame> drawn = renderer.render(inputs);
        INFO((drawn.has_value() ? std::string() : drawn.error().message));
        REQUIRE(drawn.has_value());
    }
    REQUIRE(offers.waiting.empty());
    REQUIRE(renderer.render(inputs).has_value());
    vx::Result<std::vector<std::uint8_t>> pixels = renderer.capture();
    REQUIRE(pixels.has_value());
    return std::move(pixels).value();
}

std::size_t differing_pixels(const std::vector<std::uint8_t>& a,
                             const std::vector<std::uint8_t>& b) {
    REQUIRE(a.size() == b.size());
    std::size_t differing = 0;
    for (std::size_t at = 0; at < a.size(); at += 4) {
        if (a[at] != b[at] || a[at + 1] != b[at + 1] || a[at + 2] != b[at + 2]) {
            ++differing;
        }
    }
    return differing;
}

// Pixels the sky shows through: the sky's color, as an sRGB target stores it.
std::size_t sky_pixels(const std::vector<std::uint8_t>& pixels) {
    std::size_t count = 0;
    for (std::size_t at = 0; at < pixels.size(); at += 4) {
        // 0.4, 0.6, 0.9 linear: 170, 203, 243 in sRGB.
        if (pixels[at] == 170 && pixels[at + 1] == 203 && pixels[at + 2] == 243) {
            ++count;
        }
    }
    return count;
}

// Pixels of the cutout layer's orange, whatever a face's shade and its
// corners' occlusion make of it: the sky, stone and grass are never orange,
// so only the cutout pass draws these.
std::size_t cutout_pixels(const std::vector<std::uint8_t>& pixels) {
    std::size_t count = 0;
    for (std::size_t at = 0; at < pixels.size(); at += 4) {
        if (pixels[at] > pixels[at + 1] + 20 && pixels[at + 1] > pixels[at + 2] + 20) {
            ++count;
        }
    }
    return count;
}

void offer_all(Offers& offers, const Scene& scene) {
    offers.waiting.assign(scene.updates.begin(), scene.updates.end());
}

} // namespace

TEST_CASE("a seeded scene draws the same with culling and merging on or off, and staged") {
    const std::vector<std::byte> textures = layer_bytes(colors);
    const Scene scene;
    const vx::render::Camera camera = scene_camera();

    vx::Result<vx::render::Renderer> created = scene_renderer(textures, false);
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    vx::render::Renderer& renderer = created.value();
    Offers offers;
    offer_all(offers, scene);
    const std::vector<std::uint8_t> culled = draw_all(renderer, offers, camera);
    const vx::render::DrawStats culling = renderer.chunks().draw_stats();
    // The scene shows ground and sky, and culling took out whole chunks and
    // the faces that cannot face the camera.
    const std::size_t pixel_count = std::size_t{image_size.width} * image_size.height;
    CHECK(sky_pixels(culled) > pixel_count / 20);
    CHECK(sky_pixels(culled) < pixel_count / 2);
    CHECK(culling.chunks > 20);
    CHECK(culling.culled > 20);
    CHECK(culling.quads_facing_away > 0);
    // The cutout pass draws the floating cutout blocks: its commands, not
    // the opaque pass's again.
    CHECK(cutout_pixels(culled) > pixel_count / 20);

    renderer.set_draw_switches({.frustum = false, .faces = false, .merge = false});
    Offers none;
    const std::vector<std::uint8_t> everything = draw_all(renderer, none, camera);
    CHECK(renderer.chunks().draw_stats().culled == 0);
    CHECK(renderer.chunks().draw_stats().quads_facing_away == 0);
    CHECK(differing_pixels(culled, everything) == 0);

    // Staged, with no budget at all: each frame takes its first mesh alone,
    // so the copies come in over many frames, through each slot's staging.
    vx::Result<vx::render::Renderer> staged_created = scene_renderer(textures, true);
    REQUIRE(staged_created.has_value());
    staged_created.value().set_chunk_budget(std::chrono::nanoseconds(0));
    Offers staged_offers;
    offer_all(staged_offers, scene);
    const std::vector<std::uint8_t> staged =
        draw_all(staged_created.value(), staged_offers, camera);
    CHECK(differing_pixels(culled, staged) == 0);
    const auto meshes = static_cast<int>(std::ranges::count_if(
        scene.updates, [](const auto& update) { return !update.mesh.all().empty(); }));
    CHECK(meshes > 100);
    CHECK(staged_offers.frames_waited == meshes - 1);
}

TEST_CASE("a small arena repacks and compacts while the scene streams in, leaves and comes back, "
          "and draws the same") {
    const std::vector<std::byte> textures = layer_bytes(colors);
    const Scene scene;
    const vx::render::Camera camera = scene_camera();
    vx::Result<vx::render::Renderer> reference_created = scene_renderer(textures, false);
    REQUIRE(reference_created.has_value());
    Offers reference_offers;
    offer_all(reference_offers, scene);
    const std::vector<std::uint8_t> reference =
        draw_all(reference_created.value(), reference_offers, camera);

    for (const bool staged : {false, true}) {
        INFO("staged: ", staged);
        // 4,096 quads to start with: the scene needs several repacks.
        vx::Result<vx::render::Renderer> created =
            vx::render::Renderer::create({.surface = std::nullopt,
                                          .framebuffer = image_size,
                                          .validation = true,
                                          .present_mode = vx::render::PresentMode::fifo,
                                          .overlay = nullptr,
                                          .block_textures = textures,
                                          .chunk_arena_quads = 4'096,
                                          .chunk_arena_limit = 0,
                                          .stage_chunk_uploads = staged,
                                          .draw_call_limit = 0});
        INFO((created.has_value() ? std::string() : created.error().message));
        REQUIRE(created.has_value());
        vx::render::Renderer& renderer = created.value();
        Offers offers;
        offer_all(offers, scene);
        CHECK(differing_pixels(reference, draw_all(renderer, offers, camera)) == 0);
        CHECK(renderer.chunks().upkeep().repacks > 0);
        CHECK(renderer.chunks().arena().space().stats().capacity > 4'096);

        // Every other chunk leaves; once every frame slot has come round,
        // their ranges are free and compaction moves the highest meshes
        // down into them.
        for (std::size_t at = 0; at < scene.updates.size(); at += 2) {
            offers.removals.push_back(scene.updates[at].pos);
        }
        const vx::render::FrameInputs inputs{.sky = sky,
                                             .camera = camera,
                                             .cubes = {},
                                             .overlay = nullptr,
                                             .chunk_changes = &offers};
        for (int frame = 0; frame < 3; ++frame) {
            REQUIRE(renderer.render(inputs).has_value());
        }
        renderer.compact_chunk_arena();
        REQUIRE(renderer.render(inputs).has_value());
        for (int frame = 0; frame < 200 && renderer.chunks().upkeep().compacting; ++frame) {
            REQUIRE(renderer.render(inputs).has_value());
        }
        CHECK_FALSE(renderer.chunks().upkeep().compacting);
        CHECK(renderer.chunks().upkeep().moved_total > 0);
        // They come back, as newer meshes of the same quads.
        for (std::size_t at = 0; at < scene.updates.size(); at += 2) {
            vx::render::ChunkMeshUpdate update = scene.updates[at];
            update.generation = 2;
            offers.waiting.push_back(update);
        }
        CHECK(differing_pixels(reference, draw_all(renderer, offers, camera)) == 0);
    }
}

TEST_CASE("the frame a repack happens in draws from the new arena") {
    const std::vector<std::byte> textures = layer_bytes(colors);
    const Scene scene;
    const vx::render::Camera camera = scene_camera();
    vx::Result<vx::render::Renderer> reference_created = scene_renderer(textures, false);
    REQUIRE(reference_created.has_value());
    Offers reference_offers;
    offer_all(reference_offers, scene);
    const std::vector<std::uint8_t> reference =
        draw_all(reference_created.value(), reference_offers, camera);

    for (const bool staged : {false, true}) {
        INFO("staged: ", staged);
        vx::Result<vx::render::Renderer> created =
            vx::render::Renderer::create({.surface = std::nullopt,
                                          .framebuffer = image_size,
                                          .validation = true,
                                          .present_mode = vx::render::PresentMode::fifo,
                                          .overlay = nullptr,
                                          .block_textures = textures,
                                          .chunk_arena_quads = 4'096,
                                          .chunk_arena_limit = 0,
                                          .stage_chunk_uploads = staged,
                                          .draw_call_limit = 0});
        REQUIRE(created.has_value());
        vx::render::Renderer& renderer = created.value();
        Offers offers;
        offer_all(offers, scene);
        static_cast<void>(draw_all(renderer, offers, camera));
        // Every other chunk again, newer: their meshes move to new ranges,
        // so a repack now moves meshes to new places, not where they were.
        for (std::size_t at = 0; at < scene.updates.size(); at += 2) {
            vx::render::ChunkMeshUpdate update = scene.updates[at];
            update.generation = 2;
            offers.waiting.push_back(update);
        }
        static_cast<void>(draw_all(renderer, offers, camera));
        // A mesh far out of view, larger than the room left: the next frame
        // refuses it, and the one after repacks, uploads it and draws.
        const vx::render::ArenaSpace& space = renderer.chunks().arena().space();
        const std::uint32_t room = space.capacity() - space.live();
        const std::vector<vx::Quad> quads(std::size_t{room} + 1);
        vx::ChunkMesh big{};
        std::size_t next = 0;
        for (std::size_t group = 0; group < 12 && next < quads.size(); ++group) {
            const std::size_t count = std::min<std::size_t>(32'768, quads.size() - next);
            big.ranges[group / 6][group % 6] =
                std::span<const vx::Quad>(quads).subspan(next, count);
            next += count;
        }
        REQUIRE(next == quads.size());
        offers.waiting.push_back(
            {.pos = {.x = 0, .y = 1'000, .z = 0}, .generation = 1, .mesh = big});
        const vx::render::FrameInputs inputs{.sky = sky,
                                             .camera = camera,
                                             .cubes = {},
                                             .overlay = nullptr,
                                             .chunk_changes = &offers};
        const std::uint32_t repacks = renderer.chunks().upkeep().repacks;
        REQUIRE(renderer.render(inputs).has_value());
        REQUIRE(offers.waiting.size() == 1);
        REQUIRE(renderer.render(inputs).has_value());
        REQUIRE(renderer.chunks().upkeep().repacks == repacks + 1);
        REQUIRE(offers.waiting.empty());
        vx::Result<std::vector<std::uint8_t>> pixels = renderer.capture();
        REQUIRE(pixels.has_value());
        CHECK(differing_pixels(reference, pixels.value()) == 0);
    }
}

TEST_CASE("an arena past what a command's vertexOffset reaches is refused, before it is made") {
    // 2^29 quads and one more: 4 GiB and 8 bytes, past Metal's largest buffer
    // too, so the refusal must come before the buffer is asked for.
    const vx::Result<vx::render::Renderer> created =
        vx::render::Renderer::create({.surface = std::nullopt,
                                      .framebuffer = image_size,
                                      .validation = true,
                                      .present_mode = vx::render::PresentMode::fifo,
                                      .overlay = nullptr,
                                      .block_textures = {},
                                      .chunk_arena_quads = (std::uint32_t{1} << 29U) + 1,
                                      .stage_chunk_uploads = false,
                                      .draw_call_limit = 0});
    REQUIRE_FALSE(created.has_value());
    CHECK(created.error().message == "a chunk arena holds at most 536870912 quads, not 536870913");
}

TEST_CASE("a removed chunk is no longer drawn: the frame is one that never had it") {
    const std::vector<std::byte> textures = layer_bytes(colors);
    const Scene scene;
    const vx::render::Camera camera = scene_camera();

    vx::Result<vx::render::Renderer> all = scene_renderer(textures, false);
    REQUIRE(all.has_value());
    Offers offers;
    offer_all(offers, scene);
    const std::vector<std::uint8_t> whole = draw_all(all.value(), offers, camera);
    // The west half goes.
    for (const vx::render::ChunkMeshUpdate& update : scene.updates) {
        if (update.pos.x < 0) {
            offers.removals.push_back(update.pos);
        }
    }
    const std::vector<std::uint8_t> east = draw_all(all.value(), offers, camera);

    vx::Result<vx::render::Renderer> fresh = scene_renderer(textures, false);
    REQUIRE(fresh.has_value());
    Offers east_only;
    for (const vx::render::ChunkMeshUpdate& update : scene.updates) {
        if (update.pos.x >= 0) {
            east_only.waiting.push_back(update);
        }
    }
    const std::vector<std::uint8_t> never = draw_all(fresh.value(), east_only, camera);
    CHECK(differing_pixels(whole, east) > 0);
    CHECK(differing_pixels(east, never) == 0);
}

TEST_CASE("frozen culling keeps what the frozen camera culled while the view moves on") {
    const std::vector<std::byte> textures = layer_bytes(colors);
    const Scene scene;
    const vx::render::Camera camera = scene_camera();
    vx::Result<vx::render::Renderer> created = scene_renderer(textures, false);
    REQUIRE(created.has_value());
    vx::render::Renderer& renderer = created.value();
    Offers offers;
    offer_all(offers, scene);
    const std::vector<std::uint8_t> before = draw_all(renderer, offers, camera);
    const vx::render::DrawStats seen = renderer.chunks().draw_stats();
    Offers none;
    const auto culling_from = [&](const vx::render::Camera& at) {
        static_cast<void>(draw_all(renderer, none, at));
        return renderer.chunks().draw_stats();
    };
    const auto same = [](const vx::render::DrawStats& a, const vx::render::DrawStats& b) {
        return a.chunks == b.chunks && a.culled == b.culled && a.quads == b.quads &&
               a.quads_facing_away == b.quads_facing_away;
    };

    // Live, culling changes where the camera moves to, and already at the
    // corner of its own block, where the moved camera sits in its block: a
    // frozen view that kept only its direction, or not its place within its
    // block, fails below.
    vx::render::Camera moved = camera;
    moved.position += glm::dvec3(37.5, -5.5, -61.5); // to 38, 19, 39
    REQUIRE_FALSE(same(culling_from(moved), seen));
    vx::render::Camera corner = camera;
    corner.position = {0.0, 24.0, 100.0};
    REQUIRE_FALSE(same(culling_from(corner), seen));

    // Frozen where it stands, nothing changes; moved and turned around, the
    // frame still culls as the camera saw before, so what it sees now is
    // missing.
    renderer.freeze_culling(true);
    CHECK(differing_pixels(before, draw_all(renderer, none, camera)) == 0);
    moved.yaw += std::numbers::pi_v<float>;
    const std::vector<std::uint8_t> frozen = draw_all(renderer, none, moved);
    CHECK(same(renderer.chunks().draw_stats(), seen));
    renderer.freeze_culling(false);
    const std::vector<std::uint8_t> thawed = draw_all(renderer, none, moved);
    const vx::render::DrawStats live = renderer.chunks().draw_stats();
    CHECK(live.chunks != seen.chunks);
    CHECK(differing_pixels(frozen, thawed) > 0);

    // Frozen again, it keeps the new view, not the first.
    renderer.freeze_culling(true);
    CHECK(same(culling_from(moved), live));
    CHECK(same(culling_from(camera), live));
}

TEST_CASE("frozen culling draws each chunk where it is from wherever the camera goes") {
    // With nothing culled either way, a frozen frame differs from a live one
    // only in its origins, measured from the frozen camera's block and moved
    // to the camera's: the frames must be the same.
    const std::vector<std::byte> textures = layer_bytes(colors);
    const Scene scene;
    const vx::render::Camera camera = scene_camera();
    vx::Result<vx::render::Renderer> created = scene_renderer(textures, false);
    REQUIRE(created.has_value());
    vx::render::Renderer& renderer = created.value();
    renderer.set_draw_switches({.frustum = false, .faces = false, .merge = true});
    Offers offers;
    offer_all(offers, scene);
    static_cast<void>(draw_all(renderer, offers, camera));
    renderer.freeze_culling(true);
    Offers none;
    static_cast<void>(draw_all(renderer, none, camera));
    vx::render::Camera moved = camera;
    moved.position += glm::dvec3(37.25, -5.5, -61.75);
    const std::vector<std::uint8_t> frozen = draw_all(renderer, none, moved);
    renderer.freeze_culling(false);
    const std::vector<std::uint8_t> live = draw_all(renderer, none, moved);
    CHECK(differing_pixels(frozen, live) == 0);
    CHECK(sky_pixels(live) < std::size_t{image_size.width} * image_size.height);
}

// RFC 0011's coarse chunks on the GPU: cells 2^level blocks wide, textures
// repeating once a block, and past level 2 a texture's average color.
namespace {

// Past the unused magenta: layer 1, a checker of 4-texel squares averaged
// down its mips, and layer 2, red at every mip but the last, 1 by 1 texel,
// which is blue.
std::vector<std::byte> lod_layer_bytes() {
    constexpr std::size_t count = 3;
    std::vector<std::byte> bytes(count * vx::render::block_texture_layer_bytes);
    const auto checker = [](std::size_t u, std::size_t v) {
        return ((u / 4) + (v / 4)) % 2 == 0 ? 60.0 : 200.0;
    };
    for (std::size_t level = 0; level < 5; ++level) {
        const std::size_t side = std::size_t{16} >> level;
        const std::size_t cover = std::size_t{1} << level; // level-0 texels a side
        for (std::size_t layer = 0; layer < count; ++layer) {
            for (std::size_t texel = 0; texel < side * side; ++texel) {
                Rgba color{255, 0, 255, 255};
                if (layer == 1) {
                    double sum = 0.0;
                    for (std::size_t dv = 0; dv < cover; ++dv) {
                        for (std::size_t du = 0; du < cover; ++du) {
                            sum += checker(((texel % side) * cover) + du,
                                           ((texel / side) * cover) + dv);
                        }
                    }
                    const auto gray =
                        static_cast<std::uint8_t>(sum / static_cast<double>(cover * cover));
                    color = {gray, gray, gray, 255};
                } else if (layer == 2) {
                    color = level == 4 ? Rgba{0, 0, 255, 255} : Rgba{255, 0, 0, 255};
                }
                const std::size_t at =
                    4 * ((count * vx::render::block_texture_level_start.at(level)) +
                         (layer * side * side) + texel);
                for (std::size_t channel = 0; channel < 4; ++channel) {
                    bytes[at + channel] = std::byte{color[channel]};
                }
            }
        }
    }
    return bytes;
}

std::vector<vx::BlockVisual> lod_visuals() {
    std::vector<vx::BlockVisual> visuals(3);
    for (std::uint16_t state = 1; state < 3; ++state) {
        visuals[state] = {.opacity = vx::Opacity::opaque,
                          .cull_group = 0,
                          .texture = {state, state, state, state, state, state}};
    }
    return visuals;
}

// Meshes that own their quads: each update's ranges point into `quads`.
struct OwnedMeshes {
    std::deque<std::vector<vx::Quad>> quads;

    vx::render::ChunkMeshUpdate own(const vx::ChunkMesh& mesh, vx::ChunkPos pos,
                                    std::uint8_t level) {
        const std::span<const vx::Quad> all = mesh.all();
        quads.emplace_back(all.begin(), all.end());
        vx::ChunkMesh copy{};
        for (std::size_t pass = 0; pass < 2; ++pass) {
            for (std::size_t face = 0; face < 6; ++face) {
                const std::span<const vx::Quad> range = mesh.ranges[pass][face];
                const auto offset = static_cast<std::size_t>(range.data() - all.data());
                copy.ranges[pass][face] =
                    std::span<const vx::Quad>(quads.back()).subspan(offset, range.size());
            }
        }
        return {.pos = pos, .generation = 1, .mesh = copy, .level = level};
    }
};

// A camera at `eye` looking at `target` (RFC 0001: yaw 0 looks down -z).
vx::render::Camera looking(glm::dvec3 eye, glm::dvec3 target) {
    const glm::dvec3 d = target - eye;
    vx::render::Camera camera;
    camera.position = eye;
    camera.yaw = static_cast<float>(std::atan2(-d.x, -d.z));
    camera.pitch = static_cast<float>(std::atan2(d.y, std::hypot(d.x, d.z)));
    return camera;
}

} // namespace

TEST_CASE("a level-1 chunk draws as the eight level-0 chunks of its blocks, textures and all") {
    // Stone terraces in cells of 2 by 2 by 2 blocks, 4 to 15 cells high.
    const auto solid = [](int cx, int cy, int cz) {
        if (cx < 0 || cy < 0 || cz < 0 || cx >= 32 || cy >= 32 || cz >= 32) {
            return false;
        }
        return cy < 4 + static_cast<int>(hash(cx >> 2, 0, cz >> 2, 9) % 12);
    };
    const std::vector<vx::BlockVisual> visuals = lod_visuals();
    vx::MeshScratch scratch;
    const auto padded = std::make_unique<vx::PaddedChunk>();
    OwnedMeshes owned;
    Offers coarse;
    for (int y = 0; y < vx::padded_size; ++y) {
        for (int z = 0; z < vx::padded_size; ++z) {
            for (int x = 0; x < vx::padded_size; ++x) {
                padded->blocks[vx::padded_index(x, y, z)] = solid(x - 1, y - 1, z - 1)
                                                                ? static_cast<vx::BlockState>(1)
                                                                : vx::BlockState::air;
            }
        }
    }
    coarse.waiting.push_back(
        owned.own(vx::mesh_chunk(*padded, visuals, scratch, {.occlusion = false}),
                  {.x = 0, .y = 0, .z = 0}, 1));
    Offers fine;
    for (int cy = 0; cy < 2; ++cy) {
        for (int cz = 0; cz < 2; ++cz) {
            for (int cx = 0; cx < 2; ++cx) {
                for (int y = 0; y < vx::padded_size; ++y) {
                    for (int z = 0; z < vx::padded_size; ++z) {
                        for (int x = 0; x < vx::padded_size; ++x) {
                            const int bx = (cx * vx::chunk_size) + x - 1;
                            const int by = (cy * vx::chunk_size) + y - 1;
                            const int bz = (cz * vx::chunk_size) + z - 1;
                            padded->blocks[vx::padded_index(x, y, z)] =
                                bx >= 0 && by >= 0 && bz >= 0 && solid(bx >> 1, by >> 1, bz >> 1)
                                    ? static_cast<vx::BlockState>(1)
                                    : vx::BlockState::air;
                        }
                    }
                }
                fine.waiting.push_back(
                    owned.own(vx::mesh_chunk(*padded, visuals, scratch, {.occlusion = false}),
                              {.x = cx, .y = cy, .z = cz}, 0));
            }
        }
    }
    const std::vector<std::byte> textures = lod_layer_bytes();
    const vx::render::Camera camera = looking({52.5, 34.5, 70.5}, {30.0, 12.0, 30.0});
    vx::Result<vx::render::Renderer> coarse_renderer = scene_renderer(textures, false);
    REQUIRE(coarse_renderer.has_value());
    const std::vector<std::uint8_t> one = draw_all(coarse_renderer.value(), coarse, camera);
    vx::Result<vx::render::Renderer> fine_renderer = scene_renderer(textures, false);
    REQUIRE(fine_renderer.has_value());
    const std::vector<std::uint8_t> eight = draw_all(fine_renderer.value(), fine, camera);
    const std::size_t pixels = one.size() / 4;
    CHECK(sky_pixels(one) < pixels / 2);
    // The same image to a color step: the triangles split differently where
    // the quads merge differently, and a pixel's color can round apart.
    int largest = 0;
    for (std::size_t at = 0; at < one.size(); at += 4) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            largest =
                std::max(largest, std::abs(int{one[at + channel]} - int{eight[at + channel]}));
        }
    }
    MESSAGE(differing_pixels(one, eight)
            << " of " << pixels << " pixels differ, by at most " << largest);
    CHECK(largest <= 1);
}

TEST_CASE("past level 2 a face draws its texture's last mip, its average, where level 2 draws the "
          "texture") {
    // A floor of the marker block, half the chunk high, seen from 10 blocks
    // above: close enough that texturing reads the red mips.
    const auto floor_at = [](std::uint8_t level) {
        const std::vector<vx::BlockVisual> visuals = lod_visuals();
        vx::MeshScratch scratch;
        const auto padded = std::make_unique<vx::PaddedChunk>();
        for (int y = 0; y < vx::padded_size; ++y) {
            for (int z = 0; z < vx::padded_size; ++z) {
                for (int x = 0; x < vx::padded_size; ++x) {
                    padded->blocks[vx::padded_index(x, y, z)] =
                        y >= 1 && y <= 16 ? static_cast<vx::BlockState>(2) : vx::BlockState::air;
                }
            }
        }
        OwnedMeshes owned;
        Offers offers;
        offers.waiting.push_back(
            owned.own(vx::mesh_chunk(*padded, visuals, scratch, {.occlusion = false}), {}, level));
        const std::vector<std::byte> textures = lod_layer_bytes();
        vx::Result<vx::render::Renderer> renderer = scene_renderer(textures, false);
        REQUIRE(renderer.has_value());
        const double middle = 16.0 * static_cast<double>(1 << level);
        const double top = 16.0 * static_cast<double>(1 << level);
        vx::render::Camera camera;
        camera.position = {middle + 0.5, top + 10.0, middle + 0.5};
        camera.pitch = -1.5F;
        const std::vector<std::uint8_t> pixels = draw_all(renderer.value(), offers, camera);
        const std::size_t center =
            4 * (((image_size.height / 2) * image_size.width) + (image_size.width / 2));
        return Rgba{pixels[center], pixels[center + 1], pixels[center + 2], pixels[center + 3]};
    };
    const Rgba flat = floor_at(3);
    CHECK(flat[2] > 150);
    CHECK(flat[0] < 60);
    const Rgba textured = floor_at(2);
    CHECK(textured[0] > 150);
    CHECK(textured[2] < 60);
}
