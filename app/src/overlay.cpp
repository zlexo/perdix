#include "overlay.hpp"

#include <vx/core/version.hpp>
#include <vx/render/chunk_arena.hpp>
#include <vx/render/chunk_renderer.hpp>
#include <vx/render/chunk_uploads.hpp>
#include <vx/render/context.hpp>
#include <vx/render/device_report.hpp>
#include <vx/render/draw_lists.hpp>
#include <vx/render/instance.hpp>
#include <vx/render/swapchain.hpp>

#include <SDL3/SDL.h>
#include <glm/trigonometric.hpp>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <tracy/Tracy.hpp>

#include <algorithm>
#include <chrono>
#include <numeric>
#include <string>
#include <string_view>

namespace vx::app {
namespace {

// A report's lines, without the newline after the last.
std::string without_last_newline(std::string report) {
    if (report.ends_with('\n')) {
        report.pop_back();
    }
    return report;
}

// Whether two swapchains differ in what the report says of them.
bool same_report(const render::SwapchainChoice& a, const render::SwapchainChoice& b) {
    return a.image_count == b.image_count && a.extent.width == b.extent.width &&
           a.extent.height == b.extent.height && a.format.format == b.format.format &&
           a.present_mode == b.present_mode;
}

// RFC 0001's budget for the CPU's and the GPU's time a frame, which their
// graphs draw as a line, and the lowest top of a graph's scale. The scale
// doubles until nine samples in ten fit: a lasting rise fits, and a lone
// spike, such as the first frames', runs off the top, its height in the
// label, instead of flattening the rest for as long as it is in the graph.
constexpr float budget_ms = 0.5F;
constexpr float lowest_top_ms = 2.0F;

} // namespace

void Overlay::History::add(float milliseconds) noexcept {
    samples.at(next) = milliseconds;
    next = (next + 1) % length;
    count = std::min(count + 1, length);
}

float Overlay::History::peak() const noexcept {
    const auto taken = static_cast<std::ptrdiff_t>(count);
    return count == 0 ? 0.0F : *std::max_element(samples.begin(), samples.begin() + taken);
}

float Overlay::History::ninetieth_percentile() const noexcept {
    if (count == 0) {
        return 0.0F;
    }
    std::array<float, length> sorted = samples;
    const auto taken = static_cast<std::ptrdiff_t>(count);
    const auto at = static_cast<std::ptrdiff_t>((count - 1) * 9 / 10);
    std::nth_element(sorted.begin(), sorted.begin() + at, sorted.begin() + taken);
    return sorted.at(static_cast<std::size_t>(at));
}

float Overlay::History::average() const noexcept {
    if (count == 0) {
        return 0.0F;
    }
    const auto taken = static_cast<std::ptrdiff_t>(count);
    return std::accumulate(samples.begin(), samples.begin() + taken, 0.0F) /
           static_cast<float>(count);
}

Overlay::Overlay(SDL_Window* window, bool validation, bool shown)
    : context_(ImGui::CreateContext()), validation_(validation), shown_(shown) {
    ImGui::SetCurrentContext(context_);
    ImGui::GetIO().IniFilename = nullptr; // no imgui.ini beside the app
    ImGui_ImplSDL3_InitForVulkan(window);
}

Overlay::~Overlay() {
    ImGui::SetCurrentContext(context_);
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(context_);
}

bool Overlay::take_event(const SDL_Event& event, bool mouse_captured) {
    // A captured mouse flies the camera, so ImGui sees it only release a
    // button. The click that starts flying reaches ImGui pressed and ends
    // while captured: missing the release, ImGui would hold that click, which
    // began outside its window, for good, and so hover nothing and want no
    // click, each of which would start flying again. Keys need no such rule:
    // every key event carries the modifiers, so the Escape that frees the
    // mouse also lets go of a Ctrl held to fly faster.
    const bool released = event.type == SDL_EVENT_MOUSE_BUTTON_UP;
    if ((!shown_ || mouse_captured) && !released) {
        return false;
    }
    ImGui::SetCurrentContext(context_);
    ImGui_ImplSDL3_ProcessEvent(&event);
    // ImGui decides as of the last frame whether the mouse is over its window.
    return event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ImGui::GetIO().WantCaptureMouse;
}

void Overlay::graph(const char* name, const History& history) {
    float top = lowest_top_ms;
    for (const float most = history.ninetieth_percentile(); top < most;) {
        top *= 2.0F;
    }
    // Printf-style arguments go as doubles.
    ImGui::Text("%s %.2f ms, peak %.2f", name, static_cast<double>(history.average()),
                static_cast<double>(history.peak()));
    ImGui::SameLine();
    ImGui::TextDisabled("(graph to %.0f ms)", static_cast<double>(top));

    const auto count = static_cast<int>(history.count);
    const int oldest = history.count < History::length ? 0 : static_cast<int>(history.next);
    ImGui::PushID(name);
    ImGui::PlotLines("##graph", history.samples.data(), count, oldest, nullptr, 0.0F, top,
                     ImVec2(-1.0F, 48.0F));
    ImGui::PopID();
    // PlotLines draws inside its frame, less the frame padding.
    const ImVec2 padding = ImGui::GetStyle().FramePadding;
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const float bottom = max.y - padding.y;
    const float y = bottom - ((budget_ms / top) * (bottom - (min.y + padding.y)));
    ImGui::GetWindowDrawList()->AddLine(ImVec2(min.x + padding.x, y), ImVec2(max.x - padding.x, y),
                                        IM_COL32(255, 196, 0, 200));
}

void Overlay::record(std::chrono::nanoseconds interval, const render::FrameTimes& times) {
    using Milliseconds = std::chrono::duration<float, std::milli>;
    frame_.add(Milliseconds(interval).count());
    cpu_.add(Milliseconds(std::max(interval - times.waited, std::chrono::nanoseconds(0))).count());
    if (times.gpu.has_value()) {
        gpu_.add(Milliseconds(*times.gpu).count());
    }
}

void Overlay::update_reports(const render::Renderer& renderer) {
    const render::Context& context = renderer.context();
    if (device_report_.empty()) {
        device_report_ = without_last_newline(
            render::format_device_report(context.device_info(), context.portability_fallback()));
    }
    if (const render::Swapchain* swapchain = renderer.swapchain();
        swapchain != nullptr &&
        (swapchain_report_.empty() || !same_report(swapchain->choice(), reported_choice_))) {
        reported_choice_ = swapchain->choice();
        swapchain_report_ = without_last_newline(render::format_swapchain_report(reported_choice_));
    }
}

void Overlay::draw_frame_times() const {
    // Averages over the graphs' samples, which steady the numbers.
    // Printf-style arguments go as doubles.
    const auto frame_ms = static_cast<double>(frame_.average());
    ImGui::Text("Frame %.2f ms, %.0f fps", frame_ms, frame_ms > 0.0 ? 1000.0 / frame_ms : 0.0);
    graph("CPU", cpu_);
    if (gpu_.count == 0) {
        ImGui::TextUnformatted("GPU: no time yet, or no timestamps on this queue");
    } else {
        graph("GPU", gpu_);
    }
    ImGui::TextDisabled("Averages and graphs of the last %zu frames; the line is the "
                        "%.1f ms budget",
                        History::length, static_cast<double>(budget_ms));
}

void Overlay::draw_present_mode(render::Renderer& renderer) {
    // The mode asked for; the swapchain line above says what presentation
    // got, FIFO where the surface does not offer immediate.
    ImGui::AlignTextToFramePadding(); // level with the radio buttons' text
    ImGui::TextUnformatted("Present mode");
    int mode = renderer.present_mode() == render::PresentMode::immediate ? 1 : 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("FIFO", &mode, 0)) {
        renderer.set_present_mode(render::PresentMode::fifo);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Immediate", &mode, 1)) {
        renderer.set_present_mode(render::PresentMode::immediate);
    }
}

void Overlay::draw_block_filter(render::Renderer& renderer) {
    // RFC 0004's filtering, the owner's call by eye: sharpened by default,
    // anisotropic where the device offers it, and the classic nearest.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Textures");
    int filter = static_cast<int>(renderer.block_filter());
    ImGui::SameLine();
    if (ImGui::RadioButton("Sharpened", &filter, 0)) {
        renderer.set_block_filter(render::BlockFilter::sharpened);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("No anisotropy", &filter, 1)) {
        renderer.set_block_filter(render::BlockFilter::sharpened_isotropic);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Nearest", &filter, 2)) {
        renderer.set_block_filter(render::BlockFilter::nearest);
    }
    const float anisotropy = renderer.textures().block_anisotropy();
    if (anisotropy > 1.0F) {
        ImGui::TextDisabled("Anisotropic filtering up to %.0f samples",
                            static_cast<double>(anisotropy));
    } else {
        ImGui::TextDisabled("No anisotropic filtering on this device");
    }
}

void Overlay::draw_chunk(render::Renderer& renderer, ChunkMeshing& meshing) {
    // RFC 0003's toggles for looking at the mesh: merging, which meshes the
    // chunk again, occlusion's strength, and the wireframe, shown only where
    // the device offers fillModeNonSolid (the report above says when not).
    ImGui::Text("Chunk: %zu quads, %zu of them cutout", meshing.quads, meshing.cutout);
    ImGui::Checkbox("Merge faces", &meshing.merge);
    if (renderer.wireframe_offered()) {
        bool wireframe = renderer.wireframe();
        ImGui::SameLine();
        if (ImGui::Checkbox("Wireframe", &wireframe)) {
            renderer.set_wireframe(wireframe);
        }
    }
    float strength = renderer.occlusion_strength();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0F);
    if (ImGui::SliderFloat("Ambient occlusion", &strength, 0.0F, 1.0F, "%.2f")) {
        renderer.set_occlusion_strength(strength);
    }
}

namespace {

constexpr double mebibyte = 1024.0 * 1024.0;

double quads_mib(std::uint64_t quads) {
    return static_cast<double>(quads * render::ChunkArena::quad_bytes) / mebibyte;
}

} // namespace

void Overlay::draw_chunk_rendering(render::Renderer& renderer) {
    // RFC 0007's counters, then its switches for culling and merging.
    const render::ChunkRenderer& chunks = renderer.chunks();
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if (now - arena_sampled_ >= std::chrono::milliseconds(250)) {
        arena_ = chunks.arena().space().stats();
        arena_sampled_ = now;
    }
    const render::UploadStats& uploads = chunks.uploads().stats();
    const render::DrawLists& lists = chunks.draw_lists();
    ImGui::Text("Chunk meshes: %u, %llu quads (%.1f MiB)", uploads.meshes,
                static_cast<unsigned long long>(uploads.quads), quads_mib(uploads.quads));
    ImGui::Text("Drawn: %u chunks, %u culled; %zu opaque and %zu cutout draws", lists.stats.chunks,
                lists.stats.culled, lists.opaque.size(), lists.cutout.size());
    ImGui::Text("Quads drawn: %llu, %llu facing away",
                static_cast<unsigned long long>(lists.stats.quads),
                static_cast<unsigned long long>(lists.stats.quads_facing_away));
    ImGui::Text("Arena: %.1f of %.1f MiB, largest free %.1f MiB, fragmentation %.2f",
                quads_mib(arena_.live + arena_.retired), quads_mib(arena_.capacity),
                quads_mib(arena_.largest_free), arena_.fragmentation);
    // Refusals, not the backlog: offers stop at the first one, and what
    // still waits is the app's to count.
    ImGui::Text("Uploads: %u (%llu quads), %u dropped, %u refused", uploads.uploads,
                static_cast<unsigned long long>(uploads.quads_uploaded), uploads.dropped,
                uploads.refused);
    render::DrawSwitches switches = chunks.switches();
    bool changed = ImGui::Checkbox("Frustum culling", &switches.frustum);
    ImGui::SameLine();
    changed = ImGui::Checkbox("Face culling", &switches.faces) || changed;
    ImGui::SameLine();
    changed = ImGui::Checkbox("Merge runs", &switches.merge) || changed;
    if (changed) {
        renderer.set_draw_switches(switches);
    }
    bool frozen = chunks.frozen();
    ImGui::SameLine();
    if (ImGui::Checkbox("Freeze culling", &frozen)) {
        renderer.freeze_culling(frozen);
    }
}

void Overlay::draw_camera(const render::Camera& camera) {
    ImGui::Text("Camera at x %.2f, y %.2f, z %.2f", camera.position.x, camera.position.y,
                camera.position.z);
    ImGui::Text("Yaw %.0f, pitch %.0f degrees", static_cast<double>(glm::degrees(camera.yaw)),
                static_cast<double>(glm::degrees(camera.pitch)));
}

void Overlay::draw_movement(const MovementReport& movement) {
    if (movement.noclip) {
        ImGui::TextUnformatted("Noclip camera (F4 walks, from outside blocks)");
        return;
    }
    ImGui::Text("%s (F4 for noclip): %.3f blocks a second, %.3f up%s%s%s",
                movement.flying ? "Flying" : "Walking", movement.speed, movement.vertical_speed,
                movement.sprinting ? ", sprinting" : "", movement.crouching ? ", crouching" : "",
                movement.on_ground || movement.flying ? "" : ", in the air");
    ImGui::Text("Movement ticks: %llu", static_cast<unsigned long long>(movement.ticks));
}

void Overlay::draw_validation(const render::Context& context) const {
    // RFC 0001: in debug builds, validation errors show in red.
    if (!validation_) {
        ImGui::TextDisabled("Validation: off in this build");
        return;
    }
    const render::ValidationCounts counts = context.instance().validation_counts();
    if (counts.errors > 0) {
        const ImVec4 red(1.0F, 0.35F, 0.35F, 1.0F);
        ImGui::TextColored(red, "Validation: %u errors, %u warnings", counts.errors,
                           counts.warnings);
    } else {
        ImGui::Text("Validation: no errors, %u warnings", counts.warnings);
    }
}

ImDrawData* Overlay::build(render::Renderer& renderer, const render::Camera& camera,
                           bool mouse_captured, ChunkMeshing& meshing, bool& bobbing) {
    if (!shown_) {
        return nullptr;
    }
    ZoneScopedN("Overlay");
    ImGui::SetCurrentContext(context_);
    ImGuiIO& io = ImGui::GetIO();
    // Captured, the mouse turns the camera: the overlay neither reads it nor
    // shows the cursor, which ImGui's backend would otherwise do every frame.
    constexpr ImGuiConfigFlags flying =
        ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange;
    if (mouse_captured) {
        io.ConfigFlags |= flying;
    } else {
        io.ConfigFlags &= ~flying;
    }
    update_reports(renderer);

    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    // In the top left corner, over the frame: no title bar, no moving, sized
    // to what it shows.
    constexpr float margin = 10.0F;
    ImGui::SetNextWindowPos(ImVec2(margin, margin));
    ImGui::SetNextWindowBgAlpha(0.65F);
    constexpr ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("Overlay", nullptr, window_flags)) {
        const std::string_view version = vx::version_string();
        ImGui::Text("Perdix %.*s", static_cast<int>(version.size()), version.data());
        ImGui::Separator();
        draw_frame_times();
        ImGui::Separator();
        ImGui::TextUnformatted(device_report_.data(),
                               device_report_.data() + device_report_.size());
        ImGui::TextUnformatted(swapchain_report_.data(),
                               swapchain_report_.data() + swapchain_report_.size());
        draw_present_mode(renderer);
        ImGui::Separator();
        draw_block_filter(renderer);
        ImGui::Separator();
        draw_chunk(renderer, meshing);
        ImGui::Separator();
        draw_chunk_rendering(renderer);
        ImGui::Separator();
        draw_camera(camera);
        draw_movement(movement_);
        // The game's View Bobbing option (RFC 0010).
        ImGui::Checkbox("View bobbing", &bobbing);
        ImGui::Separator();
        draw_validation(renderer.context());
    }
    ImGui::End();
    ImGui::Render();
    return ImGui::GetDrawData();
}

} // namespace vx::app
