#include "framekeyboard/app.hpp"
#include "framekeyboard/grip.hpp"
#include "framekeyboard/placement_store.hpp"
#include "framekeyboard/vr_haptics.hpp"
#include "framekeyboard/vr_texture.hpp"
#include "openvr.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <thread>

namespace framekeyboard {
namespace {
void check(vr::EVROverlayError error, const char* operation) {
    if (error != vr::VROverlayError_None) {
        throw std::runtime_error(std::string(operation) + ": " +
                                 vr::VROverlay()->GetOverlayErrorNameFromEnum(error));
    }
}
Transform from_vr(const vr::HmdMatrix34_t& pose) {
    Transform result{};
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 4; ++c) {
            result[r][c] = pose.m[r][c];
        }
    }
    return result;
}
std::optional<Transform> read_controller(vr::TrackedDeviceIndex_t device) {
    if (device >= vr::k_unMaxTrackedDeviceCount ||
        vr::VRSystem()->GetTrackedDeviceClass(device) != vr::TrackedDeviceClass_Controller) {
        return {};
    }
    // Dashboard overlays receive laser events even when legacy controller input
    // is unavailable. Query tracking separately; a missing button state is not
    // evidence that the controller is untracked or its trigger was released.
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
    vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses,
                                                    vr::k_unMaxTrackedDeviceCount);
    const auto& pose = poses[device];
    if (!pose.bPoseIsValid || !pose.bDeviceIsConnected) {
        return {};
    }
    return from_vr(pose.mDeviceToAbsoluteTracking);
}
struct GrabSample {
    bool active{}, held{}, pressed{};
    vr::TrackedDeviceIndex_t device{vr::k_unTrackedDeviceIndexInvalid};
    double stick_y{};
    std::array<double, 3> ray_direction{0, 0, -1};
    bool bumper_pressed{};
    bool bumper_held{};
};
class GrabInput {
  public:
    void reset() {
        for (auto& latch : latches_) {
            latch.reset();
        }
        for (auto& latch : bumper_latches_) {
            latch.reset();
        }
    }
    bool connect() {
        auto* input = vr::VRInput();
        if (!input || !vr::VRRenderModels()) {
            return false;
        }
        ready_ = input->GetInputSourceHandle("/user/hand/left", &hands_[0]) == vr::VRInputError_None &&
                 input->GetInputSourceHandle("/user/hand/right", &hands_[1]) == vr::VRInputError_None;
        return ready_;
    }
    std::array<GrabSample, 2> poll() {
        std::array<GrabSample, 2> result{};
        if (!ready_) {
            return result;
        }
        for (std::size_t i = 0; i < hands_.size(); ++i) {
            const auto role =
                i == 0 ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand;
            const auto device = vr::VRSystem()->GetTrackedDeviceIndexForControllerRole(role);
            if (device != devices_[i]) {
                latches_[i].reset();
                bumper_latches_[i].reset();
                devices_[i] = device;
                sticks_[i] = {};
            }
            char model[1024]{};
            vr::ETrackedPropertyError error{};
            vr::VRSystem()->GetStringTrackedDeviceProperty(device, vr::Prop_RenderModelName_String,
                                                           model, sizeof(model), &error);
            const std::string expected = i == 0 ? "{frame_controller}frame_controller_left"
                                                : "{frame_controller}frame_controller_right";
            if (error || model != expected || !vr::VRSystem()->IsTrackedDeviceConnected(device)) {
                latches_[i].reset();
                bumper_latches_[i].reset();
                sticks_[i] = {};
                continue;
            }
            vr::RenderModel_ControllerMode_State_t mode{};
            vr::RenderModel_ComponentState_t current{}, neutral{};
            vr::VRControllerState_t released{};
            auto* models = vr::VRRenderModels();
            // Frame's dashboard masks grip actions and legacy controller state.
            // Its render component still animates the physical squeeze. Supplying
            // an all-released state gives a stable rest pose, even when launched
            // while the user already holds the grip. This is Frame-model specific.
            const bool valid =
                models->GetComponentStateForDevicePath(model, "button_grip", hands_[i], &mode,
                                                       &current) &&
                models->GetComponentState(model, "button_grip", &released, &mode, &neutral);
            if (!valid) {
                latches_[i].reset();
                bumper_latches_[i].reset();
                continue;
            }
            const double angle =
                component_rotation_degrees(from_vr(neutral.mTrackingToComponentRenderModel),
                                           from_vr(current.mTrackingToComponentRenderModel));
            const auto grip = latches_[i].update(angle);
            result[i] = {true, grip.held, grip.pressed, device};

            vr::RenderModel_ComponentState_t current_bumper{}, neutral_bumper{};
            if (models->GetComponentStateForDevicePath(model, "bumper", hands_[i], &mode,
                                                       &current_bumper) &&
                models->GetComponentState(model, "bumper", &released, &mode, &neutral_bumper)) {
                const double b_angle =
                    component_rotation_degrees(from_vr(neutral_bumper.mTrackingToComponentRenderModel),
                                               from_vr(current_bumper.mTrackingToComponentRenderModel));
                const auto b_state = bumper_latches_[i].update(b_angle);
                result[i].bumper_pressed = b_state.pressed;
                result[i].bumper_held = b_state.held;
            }
            if (grip.held) {
                auto& stick = sticks_[i];
                if (!stick.attempted) {
                    stick.attempted = true;
                    vr::RenderModel_ComponentState_t rest{}, up{}, tip{};
                    vr::VRControllerState_t full_up{};
                    full_up.rAxis[0].y = 1;
                    stick.ready =
                        models->GetComponentState(model, "thumbstick", &released, &mode, &rest) &&
                        models->GetComponentState(model, "thumbstick", &full_up, &mode, &up) &&
                        stick.axis.calibrate(from_vr(rest.mTrackingToComponentRenderModel),
                                             from_vr(up.mTrackingToComponentRenderModel));
                    if (models->GetComponentState(model, "tip", &released, &mode, &tip)) {
                        for (std::size_t r = 0; r < 3; ++r) {
                            stick.ray_direction[r] = -tip.mTrackingToComponentLocal.m[r][2];
                        }
                    }
                }
                vr::RenderModel_ComponentState_t current_stick{};
                if (stick.ready && models->GetComponentStateForDevicePath(model, "thumbstick", hands_[i],
                                                                          &mode, &current_stick)) {
                    result[i].stick_y =
                        stick.axis.read(from_vr(current_stick.mTrackingToComponentRenderModel));
                }
                result[i].ray_direction = stick.ray_direction;
            }
        }
        return result;
    }

  private:
    struct Stick {
        ComponentAxis axis;
        std::array<double, 3> ray_direction{0, 0, -1};
        bool attempted{}, ready{};
    };
    std::array<Stick, 2> sticks_{};
    std::array<vr::VRInputValueHandle_t, 2> hands_{};
    std::array<GripLatch, 2> latches_{};
    std::array<GripLatch, 2> bumper_latches_{GripLatch{0.7, 0.35}, GripLatch{0.7, 0.35}};
    bool ready_{};
    std::array<vr::TrackedDeviceIndex_t, 2> devices_{
        {vr::k_unTrackedDeviceIndexInvalid, vr::k_unTrackedDeviceIndexInvalid}};
};
class LaserDrag {
  public:
    bool start(const GrabSample& input, const PanelPlacement& placement, double now) {
        const auto pose = read_controller(input.device);
        if (!pose) {
            return false;
        }
        device_ = input.device;
        started_ = updated_ = now;
        transform_.begin(*pose, placement.transform(), input.ray_direction);
        active_ = true;
        return true;
    }
    bool update(PanelPlacement& placement, double now, double stick_y) {
        const auto pose = read_controller(device_);
        if (!pose || now - started_ > 30) {
            stop();
            return false;
        }
        transform_.move_depth(stick_y, now - updated_);
        updated_ = now;
        placement.set_transform(transform_.update(*pose));
        return true;
    }
    bool active() const { return active_; }
    vr::TrackedDeviceIndex_t device() const { return device_; }
    void stop() { active_ = false; }

  private:
    PanelDrag transform_;
    vr::TrackedDeviceIndex_t device_{vr::k_unTrackedDeviceIndexInvalid};
    double started_{}, updated_{};
    bool active_{};
};
class VrPanel {
  public:
    ~VrPanel() {
        if (!connected_) {
            return;
        }
        if (handle_left_) {
            vr::VROverlay()->HideOverlay(handle_left_);
            vr::VROverlay()->DestroyOverlay(handle_left_);
            handle_left_ = 0;
        }
        if (handle_right_) {
            vr::VROverlay()->HideOverlay(handle_right_);
            vr::VROverlay()->DestroyOverlay(handle_right_);
            handle_right_ = 0;
        }
        if (handle_) {
            vr::VROverlay()->HideOverlay(handle_);
            vr::VROverlay()->ClearOverlayTexture(handle_);
            vr::VROverlay()->DestroyOverlay(handle_);
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
        }
        // The compositor can retain submitted images. End the VR connection before
        // destroying Vulkan resources, including on partially completed startup.
        vr::VR_Shutdown();
        texture_.destroy();
        vulkan_.destroy();
    }
    void connect() {
        vr::EVRInitError error = vr::VRInitError_None;
        // Connect only once: a background connect/disconnect followed immediately
        // by an overlay connection can leave SteamVR using the old legacy input
        // context. On this Linux-only app, refuse launch if no VR server is running.
        bool server_running = false;
        for (const auto& entry : fs::directory_iterator("/proc")) {
            std::ifstream name(entry.path() / "comm");
            std::string process_name;
            if (std::getline(name, process_name) && process_name == "vrserver") {
                server_running = true;
                break;
            }
        }
        if (!server_running) {
            throw std::runtime_error("SteamVR must already be running");
        }
        vr::VR_Init(&error, vr::VRApplication_Overlay);
        if (error != vr::VRInitError_None) {
            throw std::runtime_error(vr::VR_GetVRInitErrorAsEnglishDescription(error));
        }
        connected_ = true;
        if (!vr::VRSystem() || !vr::VRCompositor() || !vr::VROverlay()) {
            throw std::runtime_error("required OpenVR interfaces are unavailable");
        }
        auto* overlay = vr::VROverlay();
        check(overlay->CreateOverlay("org.framekeyboard.panel", "Full Keyboard", &handle_),
              "CreateOverlay");
        check(overlay->SetOverlayWidthInMeters(handle_, static_cast<float>(PanelPlacement{}.width())),
              "SetOverlayWidthInMeters");
        check(overlay->SetOverlayInputMethod(handle_, vr::VROverlayInputMethod_Mouse),
              "SetOverlayInputMethod");
        vr::HmdVector2_t scale{{panel_width, panel_height}};
        check(overlay->SetOverlayMouseScale(handle_, &scale), "SetOverlayMouseScale");
        for (const auto flag : {vr::VROverlayFlags_VisibleInDashboard, vr::VROverlayFlags_MultiCursor}) {
            check(overlay->SetOverlayFlag(handle_, flag, true), "SetOverlayFlag");
        }
        std::string message;
        if (!vulkan_.init(message) || !texture_.create(vulkan_, handle_, message)) {
            throw std::runtime_error(message);
        }
    }
    bool read_head(Transform& head) {
        vr::TrackedDevicePose_t pose{};
        vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, &pose, 1);
        if (!pose.bPoseIsValid) {
            return false;
        }
        for (std::size_t row = 0; row < 3; ++row) {
            for (std::size_t col = 0; col < 4; ++col) {
                head[row][col] = pose.mDeviceToAbsoluteTracking.m[row][col];
            }
        }
        return true;
    }
    std::optional<Transform> read_dashboard(const char* key, bool bottom_center) {
        auto* overlay = vr::VROverlay();
        vr::VROverlayHandle_t dashboard{};
        if (!overlay->IsDashboardVisible() ||
            overlay->FindOverlay(key, &dashboard) != vr::VROverlayError_None ||
            !overlay->IsOverlayVisible(dashboard)) {
            return {};
        }
        vr::HmdVector2_t scale{};
        if (overlay->GetOverlayMouseScale(dashboard, &scale) != vr::VROverlayError_None ||
            !std::isfinite(scale.v[0]) || !std::isfinite(scale.v[1]) || scale.v[0] <= 0 ||
            scale.v[1] <= 0) {
            return {};
        }
        // Invisible tabs return a valid but frozen transform, so reject them
        // above. Coordinates use mouse pixels and include overlay scale.
        vr::HmdVector2_t bottom{{scale.v[0] / 2, bottom_center ? 0 : scale.v[1] / 2}};
        vr::HmdMatrix34_t world{};
        if (overlay->GetTransformForOverlayCoordinates(dashboard, vr::TrackingUniverseStanding, bottom,
                                                       &world) != vr::VROverlayError_None) {
            return {};
        }
        return dashboard_anchor(from_vr(world));
    }
    void place(const PanelPlacement& placement) {
        const auto world = placement.transform();
        vr::HmdMatrix34_t transform{};
        for (std::size_t row = 0; row < 3; ++row) {
            for (std::size_t col = 0; col < 4; ++col) {
                transform.m[row][col] = static_cast<float>(world[row][col]);
            }
        }
        check(vr::VROverlay()->SetOverlayTransformAbsolute(handle_, vr::TrackingUniverseStanding,
                                                           &transform),
              "Place keyboard");
        check(vr::VROverlay()->SetOverlayWidthInMeters(handle_, static_cast<float>(placement.width())),
              "Resize keyboard");
    }
    std::uint64_t universe() const {
        vr::ETrackedPropertyError error{};
        const auto id = vr::VRSystem()->GetUint64TrackedDeviceProperty(
            vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_CurrentUniverseId_Uint64, &error);
        return error == vr::TrackedProp_Success ? id : 0;
    }
    void submit(PanelRenderer& renderer) {
        std::string error;
        if (!texture_.update(renderer, error)) {
            throw std::runtime_error(error);
        }
        const bool native = texture_.native_pixels();
        if (!reported_native_ || *reported_native_ != native) {
            std::cout << (native ? "Using native BGRA overlay texture.\n"
                                 : "Using RGBA compatibility overlay texture.\n")
                      << std::flush;
            reported_native_ = native;
        }
    }
    void setup_hands(const fs::path& vr_dir, const fs::path& data_dir) {
        auto* overlay = vr::VROverlay();
        if (!overlay) {
            return;
        }
        overlay->CreateOverlay("org.framekeyboard.hand_left", "Left Hand Pointer", &handle_left_);
        overlay->CreateOverlay("org.framekeyboard.hand_right", "Right Hand Pointer", &handle_right_);

        auto find_png = [&](const std::string& filename) -> fs::path {
            const std::vector<fs::path> candidates = {
                vr_dir / filename, data_dir / "vr" / filename, data_dir / filename,
                fs::path("/home/steamos/.local/share/framekeyboard/current/share/framekeyboard/vr") /
                    filename,
                fs::current_path() / "vr" / filename};
            for (const auto& p : candidates) {
                std::error_code ec;
                if (fs::is_regular_file(p, ec)) {
                    return fs::absolute(p);
                }
            }
            return {};
        };

        const auto left_png = find_png("hand_pointing_left.png");
        const auto right_png = find_png("hand_pointing_right.png");
        if (!left_png.empty()) {
            overlay->SetOverlayFromFile(handle_left_, left_png.string().c_str());
        }
        if (!right_png.empty()) {
            overlay->SetOverlayFromFile(handle_right_, right_png.string().c_str());
        }
        for (auto h : {handle_left_, handle_right_}) {
            if (h) {
                overlay->SetOverlayWidthInMeters(h, 0.14f);
                overlay->SetOverlaySortOrder(h, 2);
                overlay->SetOverlayFlag(h, vr::VROverlayFlags_VisibleInDashboard, true);
                overlay->SetOverlayFlag(h, vr::VROverlayFlags_NoDashboardTab, true);
            }
        }
    }
    void update_hand(bool is_left, bool show, const vr::HmdMatrix34_t* mat = nullptr) {
        auto h = is_left ? handle_left_ : handle_right_;
        if (!h) {
            return;
        }
        auto* overlay = vr::VROverlay();
        if (show && mat) {
            overlay->SetOverlayTransformAbsolute(h, vr::TrackingUniverseStanding, mat);
            overlay->ShowOverlay(h);
        } else {
            overlay->HideOverlay(h);
        }
    }
    void hide_hands() {
        auto* overlay = vr::VROverlay();
        if (handle_left_) {
            overlay->HideOverlay(handle_left_);
        }
        if (handle_right_) {
            overlay->HideOverlay(handle_right_);
        }
    }
    void set_world_float(bool floating) {
        auto* overlay = vr::VROverlay();
        if (!overlay || !handle_) {
            return;
        }
        overlay->SetOverlayFlag(handle_, vr::VROverlayFlags_VisibleInDashboard, !floating);
        for (auto h : {handle_left_, handle_right_}) {
            if (h) {
                overlay->SetOverlayFlag(h, vr::VROverlayFlags_VisibleInDashboard, !floating);
            }
        }
    }
    void set_overlay_interactive(bool interactive) {
        auto* overlay = vr::VROverlay();
        if (!overlay || !handle_) {
            return;
        }
        if (!interactive_reported_ || *interactive_reported_ != interactive) {
            overlay->SetOverlayFlag(handle_, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible,
                                    interactive);
            interactive_reported_ = interactive;
        }
    }
    vr::VROverlayHandle_t handle() const { return handle_; }

  private:
    bool connected_{};
    vr::VROverlayHandle_t handle_{};
    vr::VROverlayHandle_t handle_left_{}, handle_right_{};
    VulkanContext vulkan_;
    PanelTexture texture_;
    std::optional<bool> reported_native_;
    std::optional<bool> interactive_reported_;
};
} // namespace
int run_vr(App& app, VrInstance& instance, double duration) {
    app.set_interaction_active(false);
    VrPanel panel;
    panel.connect();
    bool last_world_float = app.world_float();
    panel.set_world_float(last_world_float);
    bool dynamic_interactive = false;
    bool last_interactive = false;
    double last_aim_time = 0.0;
    VrHaptics haptics;
    auto manifest = fs::canonical("/proc/self/exe").parent_path().parent_path() /
                    "share/framekeyboard/vr/actions.json";
    if (!fs::is_regular_file(manifest)) {
        manifest = fs::canonical("/proc/self/exe").parent_path() / "vr/actions.json";
    }
    haptics.connect(manifest);
    panel.setup_hands(manifest.parent_path(), app.data_dir());
    KeyHaptics feedback;
    GrabInput grip_input;
    if (!grip_input.connect()) {
        app.report_status("Native grip unavailable; use Recenter to recover the keyboard.");
    }
    std::array<bool, vr::k_unMaxTrackedDeviceCount> hovered_devices{};
    std::map<unsigned, vr::TrackedDeviceIndex_t> pointer_devices;
    struct DrumstickState {
        bool pressed{false};
        unsigned pointer{0};
    };
    std::map<vr::TrackedDeviceIndex_t, DrumstickState> drumsticks;
    auto cancel_device = [&](vr::TrackedDeviceIndex_t device) {
        for (auto it = pointer_devices.begin(); it != pointer_devices.end();) {
            if (it->second == device) {
                app.cancel_pointer(it->first);
                feedback.cancel_pointer(it->first);
                it = pointer_devices.erase(it);
            } else {
                ++it;
            }
        }
        if (auto it = drumsticks.find(device); it != drumsticks.end() && it->second.pressed) {
            app.cancel_pointer(it->second.pointer);
            feedback.cancel_pointer(it->second.pointer);
            it->second.pressed = false;
        }
        const auto role = vr::VRSystem()->GetControllerRoleForTrackedDeviceIndex(device);
        const bool hand_is_left = (role == vr::TrackedControllerRole_LeftHand) ||
                                  (role != vr::TrackedControllerRole_RightHand && (device % 2 == 1));
        panel.update_hand(hand_is_left, false);
    };
    struct ReleaseBeforeVrShutdown {
        App& app;
        ~ReleaseBeforeVrShutdown() {
            try {
                app.cancel();
            } catch (...) {
            }
        }
    } release{app};
    const double start = monotonic_seconds();
    bool done = false, visible = false;
    const auto placement_path = app.config_dir() / "placement.json";
    std::optional<SavedPlacement> saved;
    try {
        saved = load_placement(placement_path);
    } catch (const std::exception& error) {
        std::cerr << "Saved placement ignored: " << error.what() << '\n';
    }
    bool recenter_pending = !saved, transform_dirty = false, show_pending = false;
    std::uint64_t placement_universe = 0;
    bool waiting_for_tracking = false;
    PanelPlacement placement, displayed;
    if (saved) {
        // Size survives tracking-space changes, including a reset event before
        // the first valid head pose. Restore the world pose only after checking its universe.
        placement.set_width(saved->width);
    }
    HorizonAlignment horizon;
    std::optional<Transform> previous_dashboard;
    DashboardAnchor dashboard_anchor;
    if (saved && (!saved->universe || !panel.universe() || saved->universe == panel.universe())) {
        dashboard_anchor.restore(saved->dashboard, saved->dashboard_bar, saved->dashboard_full_rotation);
    }
    LaserDrag drag;
    auto stop_drag = [&](bool align_after_release = false) {
        feedback.cancel();
        panel.hide_hands();
        // Tracking loss or timeout may already have stopped LaserDrag. Always
        // release the app's input gate as well; repeated cleanup is harmless.
        drag.stop();
        app.set_dragging(false);
        if (align_after_release) {
            // Release may arrive without another movement sample. Schedule the
            // first alignment frame even when the hand was completely still.
            horizon.reset();
            transform_dirty = true;
        } else {
            if (displayed.ready()) {
                placement.restore(displayed.transform(), displayed.width());
            }
            horizon.reset();
        }
    };
    auto hide_panel = [&] {
        panel.hide_hands();
        app.set_interaction_active(false);
        stop_drag();
        hovered_devices.fill(false);
        pointer_devices.clear();
        for (auto& [dev, drum] : drumsticks) {
            if (drum.pressed) {
                app.cancel_pointer(drum.pointer);
                feedback.cancel_pointer(drum.pointer);
                drum.pressed = false;
            }
        }
        grip_input.reset();
        if (visible) {
            check(vr::VROverlay()->HideOverlay(panel.handle()), "Hide keyboard");
            visible = false;
        }
        panel.set_overlay_interactive(false);
        last_interactive = false;
        dynamic_interactive = false;
    };
    auto place_panel = [&](double now) {
        const auto aligned = app.horizon_lock_enabled()
                                 ? horizon.update(placement.transform(), now, drag.active())
                                 : placement.transform();
        if (!displayed.ready() || aligned != displayed.transform() ||
            placement.width() != displayed.width() || transform_dirty) {
            displayed.restore(aligned, placement.width());
            panel.place(displayed);
            transform_dirty = false;
        }
    };
    std::vector<PlacementAction> adjustments;
    double next_tracking_check = 0;
    while (!done && !app.quitting() && !interrupted) {
        const auto frame_started = std::chrono::steady_clock::now();
        const double now = monotonic_seconds();
        if (duration > 0 && now - start >= duration) {
            break;
        }
        if (instance.poll_recenter()) {
            stop_drag();
            std::cout << "Launch request received.\n" << std::flush;
            app.summon();
            show_pending = true;
        }
        if (app.world_float() != last_world_float) {
            last_world_float = app.world_float();
            panel.set_world_float(last_world_float);
            if (!last_world_float && !vr::VROverlay()->IsDashboardVisible()) {
                hide_panel();
            }
        }
        const bool dashboard_visible = vr::VROverlay()->IsDashboardVisible();
        if (!dashboard_visible && !app.world_float()) {
            hide_panel();
        }

        bool target_interactive = false;
        if (dashboard_visible) {
            target_interactive = true;
        } else if (app.world_float() && visible) {
            const auto& mode = app.world_float_mode();
            if (mode == "always") {
                target_interactive = true;
            } else if (mode == "passive" || mode == "touch") {
                target_interactive = false;
            } else { // "dynamic"
                bool aiming = drag.active();
                if (!aiming) {
                    for (const auto& [pointer, dev] : pointer_devices) {
                        if (app.pointer_pressed(pointer)) {
                            aiming = true;
                            break;
                        }
                    }
                }
                if (!aiming) {
                    for (const auto& [dev, drum] : drumsticks) {
                        if (drum.pressed) {
                            aiming = true;
                            break;
                        }
                    }
                }
                if (!aiming && displayed.ready()) {
                    const auto panel_t = displayed.transform();
                    const double pw = displayed.width();
                    const double ph = pw * (static_cast<double>(panel_height) / panel_width);

                    const std::array<double, 3> p_right = {panel_t[0][0], panel_t[1][0], panel_t[2][0]};
                    const std::array<double, 3> p_up = {panel_t[0][1], panel_t[1][1], panel_t[2][1]};
                    const std::array<double, 3> p_norm = {panel_t[0][2], panel_t[1][2], panel_t[2][2]};
                    const std::array<double, 3> p_cent = {panel_t[0][3], panel_t[1][3], panel_t[2][3]};

                    for (vr::TrackedDeviceIndex_t device = 0; device < vr::k_unMaxTrackedDeviceCount;
                         ++device) {
                        if (vr::VRSystem()->GetTrackedDeviceClass(device) !=
                            vr::TrackedDeviceClass_Controller) {
                            continue;
                        }
                        auto ctrl = read_controller(device);
                        if (!ctrl) {
                            continue;
                        }
                        const std::array<double, 3> c_pos = {(*ctrl)[0][3], (*ctrl)[1][3], (*ctrl)[2][3]};
                        const std::array<double, 3> d = {c_pos[0] - p_cent[0], c_pos[1] - p_cent[1],
                                                         c_pos[2] - p_cent[2]};

                        const double local_orig_x =
                            d[0] * p_right[0] + d[1] * p_right[1] + d[2] * p_right[2];
                        const double local_orig_y = d[0] * p_up[0] + d[1] * p_up[1] + d[2] * p_up[2];
                        const double local_orig_z =
                            d[0] * p_norm[0] + d[1] * p_norm[1] + d[2] * p_norm[2];

                        // Controller in proximity of keyboard:
                        if (local_orig_z >= -0.15 && local_orig_z <= 0.40 &&
                            std::abs(local_orig_x) <= pw * 0.70 && std::abs(local_orig_y) <= ph * 0.80) {
                            aiming = true;
                            break;
                        }

                        // Controller ray direction in world space (controller forward is -Z):
                        const std::array<double, 3> ray_dir_world = {-(*ctrl)[0][2], -(*ctrl)[1][2],
                                                                     -(*ctrl)[2][2]};

                        const double dir_norm = ray_dir_world[0] * p_norm[0] +
                                                ray_dir_world[1] * p_norm[1] +
                                                ray_dir_world[2] * p_norm[2];

                        if (local_orig_z > 0.0 && dir_norm < -0.05) {
                            const double t = -local_orig_z / dir_norm;
                            if (t > 0.0 && t <= 3.5) {
                                const double dir_right = ray_dir_world[0] * p_right[0] +
                                                         ray_dir_world[1] * p_right[1] +
                                                         ray_dir_world[2] * p_right[2];
                                const double dir_up = ray_dir_world[0] * p_up[0] +
                                                      ray_dir_world[1] * p_up[1] +
                                                      ray_dir_world[2] * p_up[2];
                                const double hit_x = local_orig_x + t * dir_right;
                                const double hit_y = local_orig_y + t * dir_up;
                                if (std::abs(hit_x) <= (pw * 0.55) && std::abs(hit_y) <= (ph * 0.60)) {
                                    aiming = true;
                                    break;
                                }
                            }
                        }
                    }
                }

                if (aiming) {
                    last_aim_time = now;
                    dynamic_interactive = true;
                } else if (now - last_aim_time > 0.35) {
                    dynamic_interactive = false;
                }
                target_interactive = dynamic_interactive;
            }
        } else {
            target_interactive = false;
        }

        if (last_interactive && !target_interactive) {
            hovered_devices.fill(false);
            for (const auto& [pointer, dev] : pointer_devices) {
                if (app.pointer_pressed(pointer)) {
                    app.cancel_pointer(pointer);
                    feedback.cancel_pointer(pointer);
                }
            }
            pointer_devices.clear();
        }
        last_interactive = target_interactive;
        panel.set_overlay_interactive(target_interactive);
        // Hidden panels cannot be grabbed. Do not query both render models in
        // standby; require a released grip sample again when the panel returns.
        if (!visible) {
            grip_input.reset();
        }
        const auto grips = visible ? grip_input.poll() : std::array<GrabSample, 2>{};
        vr::VREvent_t event{};
        while (vr::VRSystem()->PollNextEvent(&event, sizeof(event))) {
            if (event.eventType == vr::VREvent_Quit) {
                vr::VRSystem()->AcknowledgeQuit_Exiting();
                done = true;
            }
            if (event.eventType == vr::VREvent_SeatedZeroPoseReset ||
                event.eventType == vr::VREvent_ChaperoneUniverseHasChanged) {
                stop_drag();
                app.cancel();
                recenter_pending = true;
                saved.reset();
                previous_dashboard.reset();
                dashboard_anchor = {};
            }
            if (event.eventType == vr::VREvent_InputFocusChanged) {
                if (!app.world_float()) {
                    stop_drag();
                    app.cancel();
                }
            }
            if (event.eventType == vr::VREvent_ButtonPress && app.bumper_shift_enabled()) {
                if (event.data.controller.button == vr::k_EButton_ApplicationMenu) {
                    app.toggle_shift_lock();
                    if (app.haptics_enabled()) {
                        haptics.send(event.trackedDeviceIndex, KeyFeedback::Press);
                    }
                }
            }
        }
        while (vr::VROverlay()->PollNextOverlayEvent(panel.handle(), &event, sizeof(event))) {
            // Recheck before every pointer event, not just once per batch. A
            // closing dashboard must not deliver queued clicks to a local app.
            if (!vr::VROverlay()->IsDashboardVisible() && !app.world_float()) {
                hide_panel();
            }
            if (event.eventType == vr::VREvent_OverlayClosed) {
                done = true;
            }
            if (!visible) {
                continue;
            }
            // OpenVR reports bottom-left coordinates; Cairo uses top-left.
            const double x = event.data.mouse.x, y = panel_height - event.data.mouse.y;
            const auto pointer = event.data.mouse.cursorIndex;
            const auto device = event.trackedDeviceIndex;
            const bool controller =
                device < vr::k_unMaxTrackedDeviceCount &&
                vr::VRSystem()->GetTrackedDeviceClass(device) == vr::TrackedDeviceClass_Controller;
            if (controller && (event.eventType == vr::VREvent_MouseMove ||
                               event.eventType == vr::VREvent_MouseButtonDown ||
                               event.eventType == vr::VREvent_MouseButtonUp)) {
                pointer_devices[pointer] = device;
            }
            switch (event.eventType) {
            case vr::VREvent_MouseMove: {
                if (event.trackedDeviceIndex < hovered_devices.size()) {
                    hovered_devices[event.trackedDeviceIndex] =
                        x >= 0 && x < panel_width && y >= 0 && y < panel_height;
                }
                if (!drag.active()) {
                    if (app.move(pointer, x, y) && controller) {
                        feedback.hover(device);
                    }
                }
                break;
            }
            case vr::VREvent_MouseButtonDown:
                if (event.data.mouse.button == vr::VRMouseButton_Right && app.bumper_shift_enabled()) {
                    app.toggle_shift_lock();
                    if (controller && app.haptics_enabled()) {
                        feedback.press(pointer, device);
                    }
                }
                if (event.data.mouse.button == vr::VRMouseButton_Left && !drag.active()) {
                    // Grabbing cannot also type if grip and trigger arrive together.
                    const bool grabbing = std::any_of(grips.begin(), grips.end(), [&](const auto& grip) {
                        return grip.active && grip.held && grip.device == event.trackedDeviceIndex;
                    });
                    if (!grabbing) {
                        const bool key_pressed = app.down(pointer, x, y, now);
                        if (key_pressed && controller) {
                            feedback.press(pointer, device);
                        }
                    }
                }
                break;
            case vr::VREvent_MouseButtonUp:
                if (event.data.mouse.button == vr::VRMouseButton_Left && !drag.active()) {
                    if (app.up(pointer, x, y)) {
                        feedback.release(pointer);
                    }
                }
                break;
            case vr::VREvent_FocusLeave:
                if (event.trackedDeviceIndex < hovered_devices.size()) {
                    hovered_devices[event.trackedDeviceIndex] = false;
                } else {
                    hovered_devices.fill(false);
                }
                if (!drag.active()) {
                    // Focus events do not contain mouse cursorIndex. Use the
                    // controller-to-pointer association from actual mouse events.
                    // An unidentified laser leave must not release the other hand.
                    cancel_device(device);
                }
                break;
            case vr::VREvent_OverlayHidden:
            case vr::VREvent_Modal_Cancel:
                if (!app.world_float()) {
                    hovered_devices.fill(false);
                    stop_drag();
                    app.cancel();
                }
                break;
            case vr::VREvent_OverlayClosed:
                done = true;
                break;
            default:
                break;
            }
        }
        if (done || app.quitting()) {
            break;
        }
        // No arbitrary hold deadline: a tracked controller may legitimately hold
        // Shift or Backspace for minutes. A lost controller releases only its keys.
        std::set<vr::TrackedDeviceIndex_t> lost_devices;
        for (const auto& [pointer, device] : pointer_devices) {
            if (app.pointer_pressed(pointer) && !read_controller(device)) {
                lost_devices.insert(device);
            }
        }
        for (const auto device : lost_devices) {
            cancel_device(device);
        }
        if (drag.active()) {
            const bool held = std::any_of(grips.begin(), grips.end(), [&](const auto& grip) {
                return grip.active && grip.held && grip.device == drag.device();
            });
            if (!held) {
                stop_drag(true);
            }
        } else if (visible && placement.ready()) {
            const auto panel_t = displayed.transform();
            const double pw = displayed.width();
            const double ph = pw * (static_cast<double>(panel_height) / panel_width);
            for (const auto& grip : grips) {
                if (!grip.active || !grip.pressed) {
                    continue;
                }
                bool can_grab = (grip.device < hovered_devices.size() && hovered_devices[grip.device]);
                if (!can_grab && app.proximity_grab_enabled()) {
                    if (const auto ctrl = read_controller(grip.device)) {
                        constexpr double local_tip_x = 0.0;
                        constexpr double local_tip_y = 0.005;
                        constexpr double local_tip_z = -0.080;

                        const double tip_x = (*ctrl)[0][0] * local_tip_x + (*ctrl)[0][1] * local_tip_y +
                                             (*ctrl)[0][2] * local_tip_z + (*ctrl)[0][3];
                        const double tip_y = (*ctrl)[1][0] * local_tip_x + (*ctrl)[1][1] * local_tip_y +
                                             (*ctrl)[1][2] * local_tip_z + (*ctrl)[1][3];
                        const double tip_z = (*ctrl)[2][0] * local_tip_x + (*ctrl)[2][1] * local_tip_y +
                                             (*ctrl)[2][2] * local_tip_z + (*ctrl)[2][3];

                        const double dx = tip_x - panel_t[0][3];
                        const double dy = tip_y - panel_t[1][3];
                        const double dz = tip_z - panel_t[2][3];

                        const double clx = dx * panel_t[0][0] + dy * panel_t[1][0] + dz * panel_t[2][0];
                        const double cly = dx * panel_t[0][1] + dy * panel_t[1][1] + dz * panel_t[2][1];
                        const double clz = dx * panel_t[0][2] + dy * panel_t[1][2] + dz * panel_t[2][2];

                        const double trigger_distance = app.hand_distance_meters();
                        // Allow grabbing when the controller is closer to the keyboard (same distance as
                        // when the hand appears):
                        if (clz >= -0.15 && clz <= trigger_distance && std::abs(clx) <= pw * 0.65 &&
                            std::abs(cly) <= ph * 0.75) {
                            can_grab = true;
                        }
                    }
                }
                if (can_grab && drag.start(grip, displayed, now)) {
                    // Release any active drumstick presses:
                    for (auto& [dev, drum] : drumsticks) {
                        if (drum.pressed) {
                            app.up(drum.pointer, 0, 0);
                            feedback.release(drum.pointer);
                            drum.pressed = false;
                        }
                    }
                    // Capture the visible pose, not the uncorrected pose behind
                    // horizon assistance, so regrabbing never snaps the keyboard.
                    placement.restore(displayed.transform(), displayed.width());
                    horizon.reset();
                    feedback.cancel();
                    if (app.haptics_enabled()) {
                        haptics.send(grip.device, KeyFeedback::Press);
                    }
                    panel.hide_hands();
                    app.set_dragging(true);
                    break;
                }
            }
        }
        if (visible && app.bumper_shift_enabled()) {
            for (const auto& grip : grips) {
                if (grip.active && grip.bumper_pressed) {
                    app.toggle_shift_lock();
                    if (app.haptics_enabled()) {
                        haptics.send(grip.device, KeyFeedback::Press);
                    }
                }
            }
        }
        if (visible && displayed.ready() && !drag.active()) {
            const auto panel_t = displayed.transform();
            const double pw = displayed.width();
            const double ph = pw * (static_cast<double>(panel_height) / panel_width);
            for (vr::TrackedDeviceIndex_t device = 0; device < vr::k_unMaxTrackedDeviceCount; ++device) {
                if (vr::VRSystem()->GetTrackedDeviceClass(device) != vr::TrackedDeviceClass_Controller) {
                    continue;
                }
                const auto role = vr::VRSystem()->GetControllerRoleForTrackedDeviceIndex(device);
                const bool hand_is_left =
                    (role == vr::TrackedControllerRole_LeftHand) ||
                    (role != vr::TrackedControllerRole_RightHand && (device % 2 == 1));
                const bool grabbing = std::any_of(grips.begin(), grips.end(), [&](const auto& grip) {
                    return grip.active && grip.held && grip.device == device;
                });
                if (grabbing) {
                    panel.update_hand(hand_is_left, false);
                    continue;
                }
                auto ctrl = read_controller(device);
                auto& drum = drumsticks[device];
                drum.pointer = 100 + device;
                if (!ctrl) {
                    if (drum.pressed) {
                        app.up(drum.pointer, 0, 0);
                        feedback.release(drum.pointer);
                        drum.pressed = false;
                    }
                    panel.update_hand(hand_is_left, false);
                    continue;
                }
                // Physical tip of the controller (front nose/ring):
                constexpr double local_tip_x = 0.0;
                constexpr double local_tip_y = 0.005;
                constexpr double local_tip_z = -0.080;

                const double tip_x = (*ctrl)[0][0] * local_tip_x + (*ctrl)[0][1] * local_tip_y +
                                     (*ctrl)[0][2] * local_tip_z + (*ctrl)[0][3];
                const double tip_y = (*ctrl)[1][0] * local_tip_x + (*ctrl)[1][1] * local_tip_y +
                                     (*ctrl)[1][2] * local_tip_z + (*ctrl)[1][3];
                const double tip_z = (*ctrl)[2][0] * local_tip_x + (*ctrl)[2][1] * local_tip_y +
                                     (*ctrl)[2][2] * local_tip_z + (*ctrl)[2][3];

                const double dx = tip_x - panel_t[0][3];
                const double dy = tip_y - panel_t[1][3];
                const double dz = tip_z - panel_t[2][3];

                const double local_x = dx * panel_t[0][0] + dy * panel_t[1][0] + dz * panel_t[2][0];
                const double local_y = dx * panel_t[0][1] + dy * panel_t[1][1] + dz * panel_t[2][1];
                const double local_z = dx * panel_t[0][2] + dy * panel_t[1][2] + dz * panel_t[2][2];

                const double px = (local_x / pw + 0.5) * panel_width;
                const double py = (0.5 - local_y / ph) * panel_height;

                const double trigger_distance = app.hand_distance_meters();
                const bool near_keyboard =
                    (local_z >= -0.15 && local_z <= trigger_distance && std::abs(local_x) <= pw * 0.65 &&
                     std::abs(local_y) <= ph * 0.75);

                if (near_keyboard && app.hand_enabled()) {
                    // Hand overlay attached rigidly to controller:
                    // Rotated forward/down so the finger points into the keys,
                    // and the hand is lowered down directly onto the controller body.
                    const double pitch_deg = static_cast<double>(app.hand_pitch_degrees());
                    const double theta = pitch_deg * M_PI / 180.0;
                    const double sin_p = std::sin(theta);
                    const double cos_p = std::cos(theta);

                    // Fingertip in texture is at y=24 out of 512, offset = 0.14 * (232/512)
                    constexpr double tip_offset = 0.14 * ((256.0 - 24.0) / 512.0); // 0.0634375m

                    // In controller local space:
                    // Up (+Y) along finger pointing direction: [0, -sin_p, -cos_p]
                    // Normal (+Z) along top/back of hand facing user: [0, cos_p, -sin_p]
                    // Right (+X) along controller right: [1, 0, 0]
                    // Center aligns fingertip with controller tip:
                    const double center_x = local_tip_x;
                    const double center_y = local_tip_y + sin_p * tip_offset;
                    const double center_z = local_tip_z + cos_p * tip_offset;

                    const std::array<std::array<double, 4>, 3> mat_hand_ctrl{
                        {{{1.0, 0.0, 0.0, center_x}},
                         {{0.0, -sin_p, cos_p, center_y}},
                         {{0.0, -cos_p, -sin_p, center_z}}}};

                    vr::HmdMatrix34_t hand_mat{};
                    for (int r = 0; r < 3; ++r) {
                        for (int c = 0; c < 3; ++c) {
                            hand_mat.m[r][c] = static_cast<float>((*ctrl)[r][0] * mat_hand_ctrl[0][c] +
                                                                  (*ctrl)[r][1] * mat_hand_ctrl[1][c] +
                                                                  (*ctrl)[r][2] * mat_hand_ctrl[2][c]);
                        }
                        hand_mat.m[r][3] = static_cast<float>(
                            (*ctrl)[r][0] * mat_hand_ctrl[0][3] + (*ctrl)[r][1] * mat_hand_ctrl[1][3] +
                            (*ctrl)[r][2] * mat_hand_ctrl[2][3] + (*ctrl)[r][3]);
                    }
                    panel.update_hand(hand_is_left, true, &hand_mat);
                } else {
                    panel.update_hand(hand_is_left, false);
                }

                const bool allow_touch =
                    (dashboard_visible || !app.world_float() || app.world_float_mode() != "passive");
                const bool on_panel = (px >= 0.0 && px < panel_width && py >= 0.0 && py < panel_height);
                if (on_panel && app.touch_typing_enabled() && allow_touch) {
                    if (!drum.pressed) {
                        if (local_z <= 0.008 && local_z >= -0.06) {
                            drum.pressed = true;
                            if (app.down(drum.pointer, px, py, now)) {
                                feedback.press(drum.pointer, device);
                            }
                        } else if (local_z > 0.008 && local_z <= 0.040) {
                            if (app.move(drum.pointer, px, py)) {
                                feedback.hover(device);
                            }
                        }
                    } else {
                        if (local_z > 0.020 || local_z < -0.08) {
                            drum.pressed = false;
                            if (app.up(drum.pointer, px, py)) {
                                feedback.release(drum.pointer);
                            }
                        } else {
                            app.move(drum.pointer, px, py);
                        }
                    }
                } else if (drum.pressed) {
                    drum.pressed = false;
                    if (app.up(drum.pointer, px, py)) {
                        feedback.release(drum.pointer);
                    }
                }
            }
        } else {
            panel.hide_hands();
        }
        for (const auto& [device, kind] : feedback.take(now)) {
            if (!drag.active() && visible && app.haptics_enabled()) {
                haptics.send(device, kind);
            }
        }
        if (app.take_recenter()) {
            stop_drag();
            saved.reset();
            recenter_pending = true;
            adjustments.clear();
            show_pending = true;
            next_tracking_check = 0;
        }
        const auto actions = app.take_placement_actions();
        adjustments.insert(adjustments.end(), actions.begin(), actions.end());
        if (!actions.empty()) {
            next_tracking_check = 0;
        }
        if (drag.active()) {
            double stick_y = 0;
            for (const auto& input : grips) {
                if (input.active && input.held && input.device == drag.device()) {
                    stick_y = input.stick_y;
                }
            }
            if (drag.update(placement, now, stick_y)) {
                transform_dirty = true;
            } else {
                stop_drag();
            }
        }
        if ((dashboard_visible || (app.world_float() && recenter_pending)) &&
            (visible || now >= next_tracking_check)) {
            Transform head{};
            if (!panel.read_head(head)) {
                if (recenter_pending && !waiting_for_tracking) {
                    std::cout << "Waiting for valid headset pose before recentering.\n" << std::flush;
                }
                waiting_for_tracking = true;
                // Retain the dashboard-relative placement through brief tracking loss.
                hide_panel();
            } else {
                waiting_for_tracking = false;
                if (recenter_pending) {
                    dashboard_anchor = {}; // Recalibrate the mount only on an explicit reset.
                }
                const auto dashboard =
                    dashboard_anchor.update(panel.read_dashboard("valve.steam.gamepadui.main", true),
                                            panel.read_dashboard("valve.steam.gamepadui.bar", false));
                if (!adjustments.empty()) {
                    stop_drag();
                }
                if (saved) {
                    const auto universe = panel.universe();
                    if (saved->universe && universe && saved->universe != universe) {
                        recenter_pending = true;
                        std::cout << "Tracking space changed; recentering keyboard.\n";
                    } else {
                        placement.restore(saved->transform, saved->width);
                        placement_universe = universe;
                        previous_dashboard = saved->dashboard;
                        transform_dirty = true;
                        std::cout << "Restored saved keyboard placement.\n" << std::flush;
                    }
                    saved.reset();
                }
                if (recenter_pending) {
                    horizon.reset();
                    placement.recenter(head, dashboard ? &*dashboard : nullptr,
                                       static_cast<double>(panel_height) / panel_width);
                    previous_dashboard = dashboard;
                    placement_universe = panel.universe();
                    recenter_pending = false;
                    transform_dirty = true;
                    std::cout << (dashboard ? "Keyboard recentered below current dashboard.\n"
                                            : "Keyboard recentered below current headset heading.\n")
                              << std::flush;
                }
                if (dashboard) {
                    if (previous_dashboard && *previous_dashboard != *dashboard && placement.ready() &&
                        !drag.active()) {
                        placement.set_transform(
                            move_with_dashboard(placement.transform(), *previous_dashboard, *dashboard));
                        transform_dirty = true;
                    }
                    // While grabbed the hand owns placement; keep the anchor
                    // current so release preserves the new custom offset.
                    previous_dashboard = dashboard;
                }
                for (const auto action : adjustments) {
                    placement.adjust(action);
                    transform_dirty = true;
                }
                adjustments.clear();
                if (transform_dirty) {
                    place_panel(now);
                }
                // Read actual compositor visibility rather than trusting our cached flag.
                // Another UI can hide an overlay without destroying its owner process.
                const bool compositor_visible = vr::VROverlay()->IsOverlayVisible(panel.handle());
                if (placement.ready() && (vr::VROverlay()->IsDashboardVisible() || app.world_float()) &&
                    (show_pending || !visible || !compositor_visible)) {
                    app.paint(now);
                    panel.submit(app.renderer);
                    check(vr::VROverlay()->ShowOverlay(panel.handle()), "Show keyboard");
                    visible = true;
                    app.set_interaction_active(true);
                    show_pending = false;
                    std::cout << "Full Keyboard overlay visible.\n" << std::flush;
                }
            }
            next_tracking_check = now + .25;
        }
        if (visible && placement.ready() && (transform_dirty || horizon.animating())) {
            place_panel(now);
        }
        // Repeat and text commits share the same gate as pointer presses.
        if (!vr::VROverlay()->IsDashboardVisible() && !app.world_float()) {
            hide_panel();
        }
        const bool repaint = app.tick(now);
        if (visible && repaint) {
            app.paint(now);
            panel.submit(app.renderer);
        }
        // Follow the dashboard every visible frame, even without a laser over
        // the keys. Budget work inside the 60 Hz period instead of adding a full
        // sleep after it. Hidden panels still idle at 4 Hz; paints are event-driven.
        const auto period = std::chrono::microseconds(visible ? 16667 : 250000);
        std::this_thread::sleep_until(frame_started + period);
    }
    app.cancel();
    if (displayed.ready()) {
        try {
            save_placement(placement_path, {displayed.transform(), displayed.width(), placement_universe,
                                            previous_dashboard, dashboard_anchor.bar(), true});
        } catch (const std::exception& error) {
            std::cerr << "Could not save keyboard placement: " << error.what() << '\n';
        }
    }
    if (!placement.ready()) {
        std::cout << "No valid headset pose; panel was not shown.\n";
    }
    return 0;
}
} // namespace framekeyboard
