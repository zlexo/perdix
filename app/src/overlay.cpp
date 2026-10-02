#include "overlay.hpp"

#include <vx/core/version.hpp>
#include <vx/render/context.hpp>
#include <vx/render/device_report.hpp>
#include <vx/render/swapchain.hpp>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <tracy/Tracy.hpp>

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

} // namespace

Overlay::Overlay(SDL_Window* window, bool shown) : context_(ImGui::CreateContext()), shown_(shown) {
    ImGui::SetCurrentContext(context_);
    ImGui::GetIO().IniFilename = nullptr; // no imgui.ini beside the app
    ImGui_ImplSDL3_InitForVulkan(window);
}

Overlay::~Overlay() {
    ImGui::SetCurrentContext(context_);
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(context_);
}

bool Overlay::take_event(const SDL_Event& event) {
    if (!shown_) {
        return false;
    }
    ImGui::SetCurrentContext(context_);
    ImGui_ImplSDL3_ProcessEvent(&event);
    // ImGui decides as of the last frame whether the mouse is over its window.
    return event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ImGui::GetIO().WantCaptureMouse;
}

ImDrawData* Overlay::build(const render::Renderer& renderer, bool mouse_captured) {
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
        ImGui::TextUnformatted(device_report_.data(),
                               device_report_.data() + device_report_.size());
        ImGui::TextUnformatted(swapchain_report_.data(),
                               swapchain_report_.data() + swapchain_report_.size());
    }
    ImGui::End();
    ImGui::Render();
    return ImGui::GetDrawData();
}

} // namespace vx::app
