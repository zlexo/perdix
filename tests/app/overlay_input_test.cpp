// Needs a Vulkan loader and at least one driver: labeled "vulkan" in CTest. The
// window is a hidden one of SDL's dummy video driver, so no display is needed.

#include "overlay.hpp"

#include <vx/render/camera.hpp>
#include <vx/render/chunk_renderer.hpp>
#include <vx/render/context.hpp>
#include <vx/render/instance.hpp>
#include <vx/render/renderer.hpp>
#include <vx/world/life.hpp>

#include <SDL3/SDL.h>
#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h> // the hovered item's id, to find a checkbox by it

#include <optional>
#include <string>

namespace {

constexpr vx::render::Extent window_size = {.width = 1280, .height = 720}; // the app's

// SDL's video on its dummy driver and one hidden window, for one test.
class DummyWindow {
public:
    DummyWindow() {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
        video_ = SDL_InitSubSystem(SDL_INIT_VIDEO);
        if (video_) {
            window_ = SDL_CreateWindow("overlay test", static_cast<int>(window_size.width),
                                       static_cast<int>(window_size.height), SDL_WINDOW_HIDDEN);
        }
    }
    DummyWindow(const DummyWindow&) = delete;
    DummyWindow& operator=(const DummyWindow&) = delete;
    ~DummyWindow() {
        if (window_ != nullptr) {
            SDL_DestroyWindow(window_);
        }
        if (video_) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
    }

    [[nodiscard]] SDL_Window* get() const noexcept { return window_; }
    [[nodiscard]] SDL_WindowID id() const { return SDL_GetWindowID(window_); }

private:
    bool video_ = false;
    SDL_Window* window_ = nullptr;
};

SDL_Event motion(SDL_WindowID window, ImVec2 at) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.windowID = window;
    event.motion.x = at.x;
    event.motion.y = at.y;
    return event;
}

SDL_Event left_button(SDL_WindowID window, bool down, ImVec2 at) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.windowID = window;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.down = down;
    event.button.clicks = 1;
    event.button.x = at.x;
    event.button.y = at.y;
    return event;
}

SDL_Event key(SDL_WindowID window, SDL_Keycode code, SDL_Scancode scancode, SDL_Keymod held,
              bool down) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.windowID = window;
    event.key.key = code;
    event.key.scancode = scancode;
    event.key.mod = held;
    event.key.down = down;
    return event;
}

} // namespace

TEST_CASE("after the mouse has flown the camera, the overlay hovers and takes clicks again") {
    const DummyWindow window;
    INFO(std::string(SDL_GetError()));
    REQUIRE(window.get() != nullptr);
    vx::app::Overlay overlay(window.get(), true, true);
    vx::Result<vx::render::Renderer> created =
        vx::render::Renderer::create({.surface = std::nullopt,
                                      .framebuffer = window_size,
                                      .validation = true,
                                      .present_mode = vx::render::PresentMode::fifo,
                                      .overlay = overlay.context(),
                                      .block_textures = {}});
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    vx::render::Renderer& renderer = created.value();
    vx::app::ChunkMeshing meshing;
    bool bobbing = true;
    vx::app::RenderDistance distance;
    const vx::render::Camera camera;

    // As the app runs it: each frame the overlay is built and drawn, and every
    // event goes to it as Window::handle_events sends it, with whether the
    // mouse is captured. A click it does not take captures the mouse to fly
    // the camera, and Escape frees the mouse.
    bool captured = false;
    const auto frame = [&] {
        ImDrawData* const drawn =
            overlay.build(renderer, camera, captured, meshing, bobbing, distance);
        const vx::Result<vx::render::Frame> rendered =
            renderer.render({.sky = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F},
                             .camera = camera,
                             .cubes = {},
                             .overlay = drawn});
        REQUIRE(rendered.has_value());
    };
    const auto send = [&](const SDL_Event& event) {
        const bool took = overlay.take_event(event, captured);
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !took) {
            captured = true;
        }
        return took;
    };
    const auto escape = [&] {
        send(key(window.id(), SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, SDL_KMOD_NONE, true));
        captured = false;
        send(key(window.id(), SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, SDL_KMOD_NONE, false));
        frame();
    };
    const auto click = [&](ImVec2 at) {
        send(motion(window.id(), at));
        frame();
        const bool took = send(left_button(window.id(), true, at));
        frame();
        send(left_button(window.id(), false, at));
        frame();
        return took;
    };

    // The "Merge faces" checkbox: down a column inside the overlay's window
    // until ImGui hovers it.
    frame();
    const ImGuiWindow* const overlay_window = ImGui::FindWindowByName("Overlay");
    REQUIRE(overlay_window != nullptr);
    const ImGuiID merge_faces = ImHashStr("Merge faces", 0, overlay_window->ID);
    ImVec2 checkbox(28.0F, -1.0F);
    for (float y = 20.0F; y < static_cast<float>(window_size.height) && checkbox.y < 0.0F;
         y += 3.0F) {
        send(motion(window.id(), ImVec2(checkbox.x, y)));
        frame();
        if (GImGui->HoveredId == merge_faces) {
            checkbox.y = y;
        }
    }
    REQUIRE(checkbox.y >= 0.0F);
    // Before any flying, a click on it switches merging off.
    REQUIRE(click(checkbox));
    REQUIRE_FALSE(meshing.merge);
    REQUIRE_FALSE(captured);

    const ImVec2 frame_middle(1100.0F, 600.0F); // over the frame, well clear of the overlay
    SUBCASE("the click that starts flying ends while the mouse flies") {
        CHECK_FALSE(click(frame_middle));
        CHECK(captured);
        escape();
    }
    SUBCASE("Ctrl, held to fly faster, ends while the mouse flies") {
        // A frame apart, as a hand would be: ImGui moves its mouse a frame
        // after a key changes in the same frame.
        send(key(window.id(), SDLK_LCTRL, SDL_SCANCODE_LCTRL, SDL_KMOD_LCTRL, true));
        frame();
        CHECK_FALSE(click(frame_middle));
        CHECK(captured);
        send(key(window.id(), SDLK_LCTRL, SDL_SCANCODE_LCTRL, SDL_KMOD_NONE, false));
        frame();
        escape();
        // The release of the Escape that freed the mouse carries the
        // modifiers: held, Ctrl would turn the next click on a slider into
        // typing (on macOS ImGui calls the Ctrl key Super, so no modifier at
        // all is checked).
        CHECK(ImGui::GetIO().KeyMods == ImGuiMod_None);
    }
    SUBCASE("a click on the frame ends while the overlay is hidden") {
        send(motion(window.id(), frame_middle));
        frame();
        CHECK_FALSE(send(left_button(window.id(), true, frame_middle)));
        frame();
        overlay.toggle(); // F3, the button still down
        escape();
        send(left_button(window.id(), false, frame_middle));
        frame();
        overlay.toggle();
        frame();
    }

    // Back over the overlay with the mouse free, ImGui hovers the checkbox,
    // and a click on it switches merging on again without flying.
    send(motion(window.id(), checkbox));
    frame();
    CHECK(ImGui::GetIO().WantCaptureMouse);
    CHECK(GImGui->HoveredId == merge_faces);
    CHECK(click(checkbox));
    CHECK(meshing.merge);
    CHECK_FALSE(captured);
    CHECK(renderer.context().instance().validation_counts().errors == 0);
    CHECK(renderer.context().instance().validation_counts().warnings == 0);
}

TEST_CASE("the overlay's chunk rendering switches reach the renderer") {
    const DummyWindow window;
    INFO(std::string(SDL_GetError()));
    REQUIRE(window.get() != nullptr);
    vx::app::Overlay overlay(window.get(), true, true);
    vx::Result<vx::render::Renderer> created =
        vx::render::Renderer::create({.surface = std::nullopt,
                                      .framebuffer = window_size,
                                      .validation = true,
                                      .present_mode = vx::render::PresentMode::fifo,
                                      .overlay = overlay.context(),
                                      .block_textures = {}});
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    vx::render::Renderer& renderer = created.value();
    vx::app::ChunkMeshing meshing;
    bool bobbing = true;
    vx::app::RenderDistance distance;
    const vx::render::Camera camera;
    const auto frame = [&] {
        ImDrawData* const drawn =
            overlay.build(renderer, camera, false, meshing, bobbing, distance);
        REQUIRE(renderer
                    .render({.sky = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F},
                             .camera = camera,
                             .cubes = {},
                             .overlay = drawn})
                    .has_value());
    };
    const auto hover = [&](ImVec2 at) {
        overlay.take_event(motion(window.id(), at), false);
        frame();
    };
    const auto click = [&](ImVec2 at) {
        hover(at);
        overlay.take_event(left_button(window.id(), true, at), false);
        frame();
        overlay.take_event(left_button(window.id(), false, at), false);
        frame();
    };

    // The switches share a row: the first down the window's first column,
    // the others along its row.
    frame();
    const ImGuiWindow* const overlay_window = ImGui::FindWindowByName("Overlay");
    REQUIRE(overlay_window != nullptr);
    const auto id = [&](const char* label) { return ImHashStr(label, 0, overlay_window->ID); };
    ImVec2 frustum(28.0F, -1.0F);
    for (float y = 20.0F; y < static_cast<float>(window_size.height) && frustum.y < 0.0F;
         y += 3.0F) {
        hover(ImVec2(frustum.x, y));
        if (GImGui->HoveredId == id("Frustum culling")) {
            frustum.y = y;
        }
    }
    REQUIRE(frustum.y >= 0.0F);
    const auto along_row = [&](const char* label) {
        for (float x = frustum.x; x < static_cast<float>(window_size.width); x += 6.0F) {
            hover(ImVec2(x, frustum.y));
            if (GImGui->HoveredId == id(label)) {
                return ImVec2(x, frustum.y);
            }
        }
        return ImVec2(-1.0F, -1.0F);
    };
    const ImVec2 faces = along_row("Face culling");
    const ImVec2 merge = along_row("Merge runs");
    const ImVec2 freeze = along_row("Freeze culling");
    REQUIRE(faces.x >= 0.0F);
    REQUIRE(merge.x >= 0.0F);
    REQUIRE(freeze.x >= 0.0F);

    // Each click switches its one setting, and a second switches it back.
    const vx::render::ChunkRenderer& chunks = renderer.chunks();
    click(frustum);
    CHECK_FALSE(chunks.switches().frustum);
    CHECK(chunks.switches().faces);
    click(faces);
    CHECK_FALSE(chunks.switches().faces);
    click(merge);
    CHECK_FALSE(chunks.switches().merge);
    click(freeze);
    CHECK(chunks.frozen());
    click(frustum);
    click(faces);
    click(merge);
    click(freeze);
    CHECK(chunks.switches().frustum);
    CHECK(chunks.switches().faces);
    CHECK(chunks.switches().merge);
    CHECK_FALSE(chunks.frozen());
    CHECK(renderer.context().instance().validation_counts().errors == 0);
    CHECK(renderer.context().instance().validation_counts().warnings == 0);
}

TEST_CASE("the render distance slider sets the distance once let go, and nothing while fixed") {
    const DummyWindow window;
    INFO(std::string(SDL_GetError()));
    REQUIRE(window.get() != nullptr);
    vx::app::Overlay overlay(window.get(), true, true);
    vx::Result<vx::render::Renderer> created =
        vx::render::Renderer::create({.surface = std::nullopt,
                                      .framebuffer = window_size,
                                      .validation = true,
                                      .present_mode = vx::render::PresentMode::fifo,
                                      .overlay = overlay.context(),
                                      .block_textures = {}});
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    vx::render::Renderer& renderer = created.value();
    vx::app::ChunkMeshing meshing;
    bool bobbing = true;
    vx::app::RenderDistance distance{.chunks = 32, .fixed = false};
    const vx::render::Camera camera;
    const auto frame = [&] {
        ImDrawData* const drawn =
            overlay.build(renderer, camera, false, meshing, bobbing, distance);
        REQUIRE(renderer
                    .render({.sky = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F},
                             .camera = camera,
                             .cubes = {},
                             .overlay = drawn})
                    .has_value());
    };
    const auto send = [&](const SDL_Event& event) {
        overlay.take_event(event, false);
        frame();
    };

    // Down the window's first column until the cursor is over the slider.
    frame();
    const ImGuiWindow* const overlay_window = ImGui::FindWindowByName("Overlay");
    REQUIRE(overlay_window != nullptr);
    const ImGuiID slider_id = ImHashStr("Render distance", 0, overlay_window->ID);
    ImVec2 slider(60.0F, -1.0F);
    for (float y = 20.0F; y < static_cast<float>(window_size.height) && slider.y < 0.0F;
         y += 3.0F) {
        send(motion(window.id(), ImVec2(slider.x, y)));
        if (GImGui->HoveredId == slider_id) {
            slider.y = y;
        }
    }
    REQUIRE(slider.y >= 0.0F);

    // Pressed and dragged: the slider follows, the distance waits for the
    // release, then takes the value let go at.
    const ImVec2 dragged(slider.x + 20.0F, slider.y);
    send(left_button(window.id(), true, slider));
    send(motion(window.id(), dragged));
    CHECK(GImGui->ActiveId == slider_id);
    CHECK(distance.chunks == 32);
    send(left_button(window.id(), false, dragged));
    const std::int32_t set = distance.chunks;
    CHECK(set != 32);
    CHECK(set >= vx::app::RenderDistance::least);
    CHECK(set <= vx::app::RenderDistance::most);

    // Fixed, as in a flythrough: the same drag sets nothing.
    distance.fixed = true;
    send(left_button(window.id(), true, ImVec2(slider.x + 60.0F, slider.y)));
    send(motion(window.id(), ImVec2(slider.x + 100.0F, slider.y)));
    send(left_button(window.id(), false, ImVec2(slider.x + 100.0F, slider.y)));
    CHECK(distance.chunks == set);

    // With RFC 0011's horizon on, the slider reaches 512 chunks: dragged to
    // its far end, past what full detail allows.
    distance.fixed = false;
    distance.limit = vx::app::RenderDistance::horizon_most;
    send(left_button(window.id(), true, slider));
    send(motion(window.id(), ImVec2(static_cast<float>(window_size.width) - 1.0F, slider.y)));
    send(left_button(window.id(), false,
                     ImVec2(static_cast<float>(window_size.width) - 1.0F, slider.y)));
    CHECK(distance.chunks > vx::app::RenderDistance::most);
    CHECK(distance.chunks <= vx::app::RenderDistance::horizon_most);
}

TEST_CASE("the god panel's clicks set Perdix's mode, bring him and choose skins, and are taken") {
    const DummyWindow window;
    INFO(std::string(SDL_GetError()));
    REQUIRE(window.get() != nullptr);
    // The overlay hidden: the panel alone.
    vx::app::Overlay overlay(window.get(), true, false);
    vx::Result<vx::render::Renderer> created =
        vx::render::Renderer::create({.surface = std::nullopt,
                                      .framebuffer = window_size,
                                      .validation = true,
                                      .present_mode = vx::render::PresentMode::fifo,
                                      .overlay = overlay.context(),
                                      .block_textures = {}});
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    vx::render::Renderer& renderer = created.value();
    vx::app::ChunkMeshing meshing;
    bool bobbing = true;
    vx::app::RenderDistance distance;
    const vx::render::Camera camera;
    vx::app::GodPanel& god = overlay.god_panel();
    god.perdix = true;
    god.skins = {"perdix", "zlex", "alex (slim)"};
    god.perdix_skin = 0;
    god.your_skin = 1;
    const auto frame = [&] {
        ImDrawData* const drawn =
            overlay.build(renderer, camera, false, meshing, bobbing, distance);
        REQUIRE(renderer
                    .render({.sky = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F},
                             .camera = camera,
                             .cubes = {},
                             .overlay = drawn})
                    .has_value());
    };
    // Closed, it draws nothing and takes no click.
    frame();
    CHECK(ImGui::FindWindowByName("God panel") == nullptr);
    CHECK_FALSE(overlay.take_event(left_button(window.id(), true, ImVec2(640.0F, 360.0F)), false));
    overlay.take_event(left_button(window.id(), false, ImVec2(640.0F, 360.0F)), false);

    overlay.set_panel_open(true);
    frame();
    frame();
    const ImGuiWindow* const panel = ImGui::FindWindowByName("God panel");
    REQUIRE(panel != nullptr);
    const auto hover = [&](ImVec2 at) {
        overlay.take_event(motion(window.id(), at), false);
        frame();
    };
    const auto click = [&](ImVec2 at) {
        hover(at);
        const bool took = overlay.take_event(left_button(window.id(), true, at), false);
        frame();
        overlay.take_event(left_button(window.id(), false, at), false);
        frame();
        return took;
    };
    // An item of `window`, found down its first column, then along the row.
    const auto find = [&](const ImGuiWindow* in, const char* label) {
        const ImGuiID id = ImHashStr(label, 0, in->ID);
        const float left = in->Pos.x + 12.0F;
        for (float y = in->Pos.y; y < in->Pos.y + in->Size.y; y += 3.0F) {
            hover(ImVec2(left, y));
            if (GImGui->HoveredId == id) {
                return ImVec2(left, y);
            }
        }
        for (float y = in->Pos.y; y < in->Pos.y + in->Size.y; y += 6.0F) {
            for (float x = left; x < in->Pos.x + in->Size.x; x += 12.0F) {
                hover(ImVec2(x, y));
                if (GImGui->HoveredId == id) {
                    return ImVec2(x, y);
                }
            }
        }
        return ImVec2(-1.0F, -1.0F);
    };

    const ImVec2 follow = find(panel, "Following you");
    REQUIRE(follow.x >= 0.0F);
    CHECK(click(follow));
    CHECK(god.perdix_mode == vx::LifeMode::follow);
    const ImVec2 still = find(panel, "Standing still");
    REQUIRE(still.x >= 0.0F);
    CHECK(click(still));
    CHECK(god.perdix_mode == vx::LifeMode::still);

    const ImVec2 bring = find(panel, "Bring him here");
    REQUIRE(bring.x >= 0.0F);
    CHECK_FALSE(god.bring_perdix);
    CHECK(click(bring));
    CHECK(god.bring_perdix);

    // Your skin's list opens below it; a name in it is chosen.
    const ImVec2 yours = find(panel, "Your skin");
    REQUIRE(yours.x >= 0.0F);
    CHECK(click(yours));
    const ImGuiWindow* const list = ImGui::FindWindowByName("##Combo_00");
    REQUIRE(list != nullptr);
    REQUIRE(list->Active);
    const ImVec2 alex = find(list, "alex (slim)");
    REQUIRE(alex.x >= 0.0F);
    CHECK(click(alex));
    CHECK(god.your_skin == 2);
    CHECK(god.perdix_skin == 0);
    // And Perdix's from his own list, yours as it was.
    const ImVec2 his = find(panel, "Perdix's skin");
    REQUIRE(his.x >= 0.0F);
    CHECK(click(his));
    const ImGuiWindow* const his_list = ImGui::FindWindowByName("##Combo_00");
    REQUIRE(his_list != nullptr);
    REQUIRE(his_list->Active);
    const ImVec2 zlex = find(his_list, "zlex");
    REQUIRE(zlex.x >= 0.0F);
    CHECK(click(zlex));
    CHECK(god.perdix_skin == 1);
    CHECK(god.your_skin == 2);

    // Off the panel, a click is the world's.
    CHECK_FALSE(click(ImVec2(40.0F, 680.0F)));
    CHECK(renderer.context().instance().validation_counts().errors == 0);
    CHECK(renderer.context().instance().validation_counts().warnings == 0);
}

TEST_CASE("the overlay's Fog switch turns the fog off and on, and it reports the held distance") {
    const DummyWindow window;
    INFO(std::string(SDL_GetError()));
    REQUIRE(window.get() != nullptr);
    vx::app::Overlay overlay(window.get(), true, true);
    vx::Result<vx::render::Renderer> created =
        vx::render::Renderer::create({.surface = std::nullopt,
                                      .framebuffer = window_size,
                                      .validation = true,
                                      .present_mode = vx::render::PresentMode::fifo,
                                      .overlay = overlay.context(),
                                      .block_textures = {}});
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    vx::render::Renderer& renderer = created.value();
    vx::app::ChunkMeshing meshing;
    bool bobbing = true;
    vx::app::RenderDistance distance;
    const vx::render::Camera camera;
    const auto frame = [&] {
        ImDrawData* const drawn =
            overlay.build(renderer, camera, false, meshing, bobbing, distance);
        REQUIRE(renderer
                    .render({.sky = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F},
                             .camera = camera,
                             .cubes = {},
                             .overlay = drawn})
                    .has_value());
    };
    const auto hover = [&](ImVec2 at) {
        overlay.take_event(motion(window.id(), at), false);
        frame();
    };
    const auto click = [&](ImVec2 at) {
        hover(at);
        overlay.take_event(left_button(window.id(), true, at), false);
        frame();
        overlay.take_event(left_button(window.id(), false, at), false);
        frame();
    };

    // No distance held before streaming's first report; then the report's.
    CHECK_FALSE(overlay.held_view_distance().has_value());
    vx::app::WorldReport report;
    report.limits.view_distance = 24;
    overlay.set_world_report(report);
    REQUIRE(overlay.held_view_distance().has_value());
    CHECK(*overlay.held_view_distance() == 24);

    // The switch shares View bobbing's row.
    frame();
    const ImGuiWindow* const overlay_window = ImGui::FindWindowByName("Overlay");
    REQUIRE(overlay_window != nullptr);
    const auto id = [&](const char* label) { return ImHashStr(label, 0, overlay_window->ID); };
    ImVec2 bob(28.0F, -1.0F);
    for (float y = 20.0F; y < static_cast<float>(window_size.height) && bob.y < 0.0F; y += 3.0F) {
        hover(ImVec2(bob.x, y));
        if (GImGui->HoveredId == id("View bobbing")) {
            bob.y = y;
        }
    }
    REQUIRE(bob.y >= 0.0F);
    ImVec2 fog(-1.0F, bob.y);
    for (float x = bob.x; x < static_cast<float>(window_size.width) && fog.x < 0.0F; x += 6.0F) {
        hover(ImVec2(x, bob.y));
        if (GImGui->HoveredId == id("Fog")) {
            fog.x = x;
        }
    }
    REQUIRE(fog.x >= 0.0F);
    CHECK(overlay.fog());
    click(fog);
    CHECK_FALSE(overlay.fog());
    CHECK(bobbing);
    click(fog);
    CHECK(overlay.fog());
    CHECK(renderer.context().instance().validation_counts().errors == 0);
}
