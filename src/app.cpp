#include "framekeyboard/app.hpp"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <glib.h>
#include <json-c/json.h>
#include <linux/input-event-codes.h>
#include <optional>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace framekeyboard {
volatile std::sig_atomic_t interrupted = 0;
double monotonic_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
namespace {
std::string read_system_clipboard() {
    const char* env_disp = std::getenv("DISPLAY");
    std::vector<std::string> displays_to_try;
    if (env_disp && *env_disp) {
        displays_to_try.push_back(env_disp);
    }
    if (std::find(displays_to_try.begin(), displays_to_try.end(), ":0") == displays_to_try.end()) {
        displays_to_try.push_back(":0");
    }
    if (std::find(displays_to_try.begin(), displays_to_try.end(), ":1") == displays_to_try.end()) {
        displays_to_try.push_back(":1");
    }

    for (const auto& disp_name : displays_to_try) {
        Display* display = XOpenDisplay(disp_name.c_str());
        if (!display) {
            continue;
        }
        const Atom targets[2] = {XInternAtom(display, "CLIPBOARD", False), XA_PRIMARY};
        const Atom types[2] = {XInternAtom(display, "UTF8_STRING", False), XA_STRING};
        Atom target_prop = XInternAtom(display, "FRAMEKEYBOARD_CLIP", False);
        Window window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 1, 1, 0, 0, 0);

        std::string result;
        for (Atom sel : targets) {
            for (Atom type : types) {
                XConvertSelection(display, sel, type, target_prop, window, CurrentTime);
                XFlush(display);

                for (int i = 0; i < 15; ++i) {
                    XEvent event;
                    if (XCheckTypedWindowEvent(display, window, SelectionNotify, &event)) {
                        if (event.xselection.property != None) {
                            Atom actual_type;
                            int actual_format;
                            unsigned long nitems = 0, bytes_after = 0;
                            unsigned char* prop = nullptr;
                            if (XGetWindowProperty(display, window, target_prop, 0, 65536, True,
                                                   AnyPropertyType, &actual_type, &actual_format,
                                                   &nitems, &bytes_after, &prop) == Success) {
                                if (prop && nitems > 0) {
                                    result.assign(reinterpret_cast<const char*>(prop), nitems);
                                    XFree(prop);
                                }
                            }
                        }
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                if (!result.empty()) {
                    break;
                }
            }
            if (!result.empty()) {
                break;
            }
        }
        XDestroyWindow(display, window);
        XCloseDisplay(display);

        if (!result.empty() &&
            g_utf8_validate(result.c_str(), static_cast<gssize>(result.size()), nullptr)) {
            auto start = result.find_first_not_of(" \t\r\n");
            auto end = result.find_last_not_of(" \t\r\n");
            if (start != std::string::npos && end != std::string::npos) {
                return result.substr(start, end - start + 1);
            }
            return result;
        }
    }
    return {};
}
#ifdef None
#undef None
#endif
#ifdef Success
#undef Success
#endif

bool same_keymap(const Language& a, const Language& b) {
    return std::tie(a.rules, a.model, a.keymap, a.variant, a.options) ==
           std::tie(b.rules, b.model, b.keymap, b.variant, b.options);
}
std::unique_ptr<CjkComposer> prepare_cjk(const Language& language) {
    if (language.input_method == "korean-2set" || language.input_method.starts_with("chinese-")) {
        return std::make_unique<CjkComposer>(language.input_method);
    }
    return {};
}
std::string compatible_layout(const Profiles& profiles, const Selection& selection) {
    try {
        validate_selection(profiles, selection);
        return selection.layout;
    } catch (const std::exception&) {
        // Prefer the smallest complete geometry, not alphabetical catalog order.
        const Layout* best = nullptr;
        for (const auto& [id, layout] : profiles.layouts) {
            try {
                validate_selection(profiles, {id, selection.language, selection.theme});
                if (!best || layout.keys.size() < best->keys.size()) {
                    best = &layout;
                }
            } catch (const std::exception&) {
            }
        }
        if (best) {
            return best->id;
        }
    }
    return selection.layout; // Apply reports the validation error if none fit.
}
template <class ProfilesMap> void cycle(const ProfilesMap& map, std::string& id, bool next) {
    auto it = map.find(id);
    if (it == map.end()) {
        id = map.begin()->first;
        return;
    }
    if (next) {
        if (++it == map.end()) {
            it = map.begin();
        }
    } else {
        if (it == map.begin()) {
            it = map.end();
        }
        --it;
    }
    id = it->first;
}
struct Keystroke {
    int code;
    bool shift;
};

std::optional<Keystroke> ascii_to_keystroke(char c) {
    if (c >= 'a' && c <= 'z') {
        static constexpr int letters[26] = {
            KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M,
            KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z};
        return Keystroke{letters[c - 'a'], false};
    }
    if (c >= 'A' && c <= 'Z') {
        static constexpr int letters[26] = {
            KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M,
            KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z};
        return Keystroke{letters[c - 'A'], true};
    }
    if (c >= '1' && c <= '9') {
        return Keystroke{KEY_1 + (c - '1'), false};
    }
    if (c == '0') {
        return Keystroke{KEY_0, false};
    }
    if (c == ' ') {
        return Keystroke{KEY_SPACE, false};
    }
    if (c == '\n') {
        return Keystroke{KEY_ENTER, false};
    }
    if (c == '\t') {
        return Keystroke{KEY_TAB, false};
    }
    if (c == '.') {
        return Keystroke{KEY_DOT, false};
    }
    if (c == ',') {
        return Keystroke{KEY_COMMA, false};
    }
    if (c == '-') {
        return Keystroke{KEY_MINUS, false};
    }
    if (c == '=') {
        return Keystroke{KEY_EQUAL, false};
    }
    if (c == '/') {
        return Keystroke{KEY_SLASH, false};
    }
    if (c == ';') {
        return Keystroke{KEY_SEMICOLON, false};
    }
    if (c == '\'') {
        return Keystroke{KEY_APOSTROPHE, false};
    }
    if (c == '`') {
        return Keystroke{KEY_GRAVE, false};
    }
    if (c == '[') {
        return Keystroke{KEY_LEFTBRACE, false};
    }
    if (c == ']') {
        return Keystroke{KEY_RIGHTBRACE, false};
    }
    if (c == '\\') {
        return Keystroke{KEY_BACKSLASH, false};
    }

    if (c == '!') {
        return Keystroke{KEY_1, true};
    }
    if (c == '@') {
        return Keystroke{KEY_2, true};
    }
    if (c == '#') {
        return Keystroke{KEY_3, true};
    }
    if (c == '$') {
        return Keystroke{KEY_4, true};
    }
    if (c == '%') {
        return Keystroke{KEY_5, true};
    }
    if (c == '^') {
        return Keystroke{KEY_6, true};
    }
    if (c == '&') {
        return Keystroke{KEY_7, true};
    }
    if (c == '*') {
        return Keystroke{KEY_8, true};
    }
    if (c == '(') {
        return Keystroke{KEY_9, true};
    }
    if (c == ')') {
        return Keystroke{KEY_0, true};
    }
    if (c == '_') {
        return Keystroke{KEY_MINUS, true};
    }
    if (c == '+') {
        return Keystroke{KEY_EQUAL, true};
    }
    if (c == '?') {
        return Keystroke{KEY_SLASH, true};
    }
    if (c == ':') {
        return Keystroke{KEY_SEMICOLON, true};
    }
    if (c == '"') {
        return Keystroke{KEY_APOSTROPHE, true};
    }
    if (c == '~') {
        return Keystroke{KEY_GRAVE, true};
    }
    if (c == '{') {
        return Keystroke{KEY_LEFTBRACE, true};
    }
    if (c == '}') {
        return Keystroke{KEY_RIGHTBRACE, true};
    }
    if (c == '|') {
        return Keystroke{KEY_BACKSLASH, true};
    }
    if (c == '<') {
        return Keystroke{KEY_COMMA, true};
    }
    if (c == '>') {
        return Keystroke{KEY_DOT, true};
    }

    return std::nullopt;
}
} // namespace
App::App(const Options& options, KeySink& sink)
    : options_(options), profiles_(load_profiles(options.data_dir, options.config_dir)),
      settings_(load_settings(options.config_dir, profiles_.errors)), gate_(sink), keyboard_(gate_) {
    try {
        validate_selection(profiles_, settings_.active);
        cjk_ = prepare_cjk(profiles_.languages.at(settings_.active.language));
    } catch (const std::exception& error) {
        profiles_.errors.push_back(error.what());
        settings_.active = {};
    }
    keymap_ = std::make_unique<LanguageMap>(profiles_.languages.at(settings_.active.language));
    pending_ = settings_.active;
    status_ = profiles_.errors.empty() ? "" : profiles_.errors.front();
    if (const auto target = profiles_.languages.find(options_.target_language);
        target != profiles_.languages.end()) {
        target_language_ = target->second;
    }
    auto helper = find_speech_helper();
    auto model = find_speech_model(
        options_.speech_model.empty() ? fs::path(settings_.active.speech_model) : options_.speech_model,
        options_.config_dir, options_.data_dir);
    dictation_ = std::make_unique<Dictation>(std::move(helper), std::move(model));
    capture_ = std::make_unique<VideoCapture>();
    load_clipboard_history();
    world_float_ = settings_.active.world_float_enabled;
    // A saved language mismatch must leave Settings reachable, not abort launch.
    refresh_typing();
}
App::~App() {
    if (capture_ && capture_->active()) {
        std::string saved;
        capture_->stop(&saved);
    }
}
std::vector<Control> App::controls() const {
    std::vector<Control> result = {
        {"settings",
         settings_open_ ? "Back" : "Settings",
         {18, 12, 60, 42},
         false,
         settings_open_ ? Icon::Back : Icon::Settings},
        {"recenter", "Recenter", {88, 12, 60, 42}, false, Icon::Recenter},
        {"size-smaller", "Smaller keyboard", {158, 12, 60, 42}, false, Icon::ScaleDown},
        {"size-larger", "Larger keyboard", {228, 12, 60, 42}, false, Icon::ScaleUp},
        {"world-float", "Float in world", {1452, 12, 60, 42}, world_float_, Icon::Pin},
        {"close", "Close", {1522, 12, 60, 42}, false, Icon::Close}};
    if (!settings_open_) {
        double next_x = 298.0;
        if (voice_enabled()) {
            const bool dictating = dictation_ && dictation_->active();
            result.push_back({"dictate", "Dictate", {next_x, 12, 60, 42}, dictating, Icon::Dictate});
            next_x += 70.0;
        }
        if (capture_enabled()) {
            const bool capturing = capture_ && capture_->active();
            result.push_back({"capture", "Capture", {next_x, 12, 60, 42}, capturing, Icon::Capture});
            next_x += 70.0;
        }
        if (japanese()) {
            result.push_back({"ime-toggle", japanese_latin_ ? "A / あ" : "あ / A", {610, 12, 115, 42}});
            if (!japanese_latin_) {
                result.push_back({"ime-hiragana", "ひらがな", {735, 12, 120, 42}});
                result.push_back({"ime-katakana", "カタカナ", {865, 12, 120, 42}});
                result.push_back({"ime-commit", "確定 / Commit", {995, 12, 180, 42}});
                result.push_back({"ime-cancel", "Cancel", {1185, 12, 110, 42}});
                const auto candidates = composition_.candidates();
                if (!candidates.empty()) {
                    result.push_back({"ime-prev", "↑", {20, 148, 55, 42}});
                    const int page = composition_.selected() / 5 * 5;
                    for (int i = page; i < std::min(page + 5, static_cast<int>(candidates.size()));
                         ++i) {
                        result.push_back(
                            {"ime-candidate-" + std::to_string(i),
                             std::to_string(i + 1) + " " + candidates[static_cast<std::size_t>(i)],
                             {85.0 + (i - page) * 240, 148, 230, 42},
                             i == composition_.selected()});
                    }
                    result.push_back({"ime-next", "↓", {1290, 148, 55, 42}});
                    result.push_back({"ime-segment-prev", "←", {1355, 148, 55, 42}});
                    result.push_back({"ime-segment-next", "→", {1420, 148, 55, 42}});
                    result.push_back({"ime-segment-label",
                                      std::to_string(composition_.active_segment() + 1) + "/" +
                                          std::to_string(composition_.segment_count()),
                                      {1485, 148, 90, 42}});
                } else {
                    result.push_back({"ime-convert", "変換 / Convert", {20, 148, 230, 42}});
                }
            }
        }
        if (cjk_) {
            result.push_back({"cjk-toggle",
                              cjk_latin_ ? "A / native" : (cjk_->chinese() ? "中文 / A" : "한 / A"),
                              {610, 12, 150, 42}});
            if (!cjk_latin_) {
                result.push_back({"cjk-commit", "Commit", {995, 12, 180, 42}});
                result.push_back({"cjk-cancel", "Cancel", {1185, 12, 110, 42}});
                const auto candidates = cjk_->candidates();
                if (!candidates.empty()) {
                    const int page = cjk_->selected() / 5 * 5;
                    result.push_back({"cjk-prev", "↑", {20, 148, 55, 42}});
                    for (int i = page; i < std::min(page + 5, static_cast<int>(candidates.size()));
                         ++i) {
                        result.push_back({"cjk-candidate-" + std::to_string(i),
                                          std::to_string(i - page + 1) + " " +
                                              candidates[static_cast<std::size_t>(i)],
                                          {85.0 + (i - page) * 240, 148, 230, 42},
                                          i == cjk_->selected()});
                    }
                    result.push_back({"cjk-next", "↓", {1290, 148, 55, 42}});
                }
            }
        }
        if (clipboard_open_) {
            if (clipboard_history_.empty()) {
                result.push_back({"clip-empty",
                                  "(No text copied yet · Press Copy or select text)",
                                  {260, 240, 1080, 60}});
            } else {
                const int count = std::min(5, static_cast<int>(clipboard_history_.size()));
                for (int i = 0; i < count; ++i) {
                    std::string label = clipboard_history_[i];
                    std::replace(label.begin(), label.end(), '\n', ' ');
                    if (label.size() > 65) {
                        label = label.substr(0, 62) + "...";
                    }
                    result.push_back({"clip-" + std::to_string(i),
                                      "[" + std::to_string(i + 1) + "]  " + label,
                                      {260, 175.0 + i * 55.0, 1080, 48}});
                }
                result.push_back({"clip-clear", "Clear History", {260, 470, 260, 52}});
            }
            result.push_back({"clip-close", "Close", {1080, 470, 260, 52}});
        }
        return result;
    }

    result.push_back({"settings-tab-profiles", "Profiles", {300, 12, 130, 42}, settings_tab_ == 0});
    result.push_back({"settings-tab-hands", "VR & Hands", {440, 12, 150, 42}, settings_tab_ == 1});
    result.push_back({"settings-tab-features", "Features", {600, 12, 130, 42}, settings_tab_ == 2});
    result.push_back({"settings-tab-capture", "Capture", {740, 12, 130, 42}, settings_tab_ == 3});

    if (settings_tab_ == 0) {
        const auto& language = profiles_.languages.at(pending_.language);
        const auto& layout = profiles_.layouts.at(pending_.layout);
        const bool japanese_layout =
            std::any_of(layout.keys.begin(), layout.keys.end(), [](const Key& key) {
                return key.action_kind == ActionKind::Key && key.action == "KanaMode";
            });
        // Use the pending selection so the options follow changes before Apply.
        if (japanese_layout || language.locale == "ja" || language.locale.starts_with("ja-") ||
            language.locale.starts_with("ja_") || language.keymap == "jp" ||
            language.input_method.starts_with("japanese-")) {
            result.push_back({"preset-ja-romaji", "日本語 Romaji", {880, 12, 190, 42}});
            result.push_back({"preset-ja-kana", "日本語 Kana", {1080, 12, 190, 42}});
            result.push_back({"preset-ja-jis", "JIS (system IME)", {1280, 12, 225, 42}});
        }
        auto row = [&](const std::string& kind, const std::string& name, double y) {
            result.push_back({kind + "-prev", "<", {40, y, 65, 64}});
            result.push_back({kind + "-label", name, {115, y, 1370, 64}});
            result.push_back({kind + "-next", ">", {1495, y, 65, 64}});
        };
        auto source = [&](const std::string& kind, const std::string& id) {
            const auto found = profiles_.sources.find(kind + "/" + id);
            return found == profiles_.sources.end()
                       ? " [built-in]"
                       : " [" + fs::path(found->second).filename().string() + "]";
        };
        row("layout",
            "Layout: " + profiles_.layouts.at(pending_.layout).name + source("layouts", pending_.layout),
            96);
        row("language",
            "Language: " + profiles_.languages.at(pending_.language).name +
                source("languages", pending_.language),
            182);
        row("theme",
            "Theme: " + profiles_.themes.at(pending_.theme).name + source("themes", pending_.theme),
            268);
        if (!settings_.favorites.empty()) {
            row("favorite", "Favorite: " + settings_.favorites.at(favorite_index_).name, 354);
        }
        result.push_back({"apply", "Apply and save", {350, 446, 280, 60}});
        result.push_back({"reload", "Reload profiles", {650, 446, 280, 60}});
        if (!settings_.favorites.empty()) {
            result.push_back({"favorite-use", "Use favorite", {950, 446, 280, 60}});
        }
    } else if (settings_tab_ == 1) {
        auto row = [&](const std::string& kind, const std::string& name, double y) {
            result.push_back({kind + "-prev", "<", {40, y, 65, 42}});
            result.push_back({kind + "-label", name, {115, y, 1370, 42}});
            result.push_back({kind + "-next", ">", {1495, y, 65, 42}});
        };
        row("hand-opt",
            "Virtual hand pointer: " + std::string(pending_.hand_enabled ? "Enabled" : "Disabled"), 56);

        row("hand-distance",
            "Hand appearance distance: " + std::to_string(pending_.hand_distance_cm) + " cm" +
                (pending_.hand_distance_cm == 12 ? " (recommended)" : ""),
            104);

        row("hand-pitch",
            "Hand rotation angle: " + std::to_string(pending_.hand_pitch_deg) + "°" +
                (pending_.hand_pitch_deg == 25 ? " (recommended)" : ""),
            152);

        row("touch-typing",
            "Direct touch typing: " +
                std::string(pending_.touch_typing_enabled ? "Enabled" : "Disabled"),
            200);

        row("proximity-grab",
            "Grab without laser: " +
                std::string(pending_.proximity_grab_enabled ? "Enabled" : "Disabled"),
            248);

        row("horizon-lock",
            "Horizon auto-leveling: " +
                std::string(pending_.horizon_lock_enabled ? "Enabled" : "Disabled"),
            296);

        row("bumper-shift",
            "Bumper (L1/R1) Shift toggle: " +
                std::string(pending_.bumper_shift_enabled ? "Enabled" : "Disabled"),
            344);

        row("world-float-opt",
            "Float in world: " + std::string(pending_.world_float_enabled
                                                 ? "Enabled (stays open outside dashboard)"
                                                 : "Disabled (dashboard only)"),
            392);

        std::string mode_label = "Dynamic (Aim to interact)";
        if (pending_.world_float_mode == "passive") {
            mode_label = "Passive (Dashboard laser)";
        } else if (pending_.world_float_mode == "touch") {
            mode_label = "Direct touch only";
        } else if (pending_.world_float_mode == "always") {
            mode_label = "Always laser (Game locked)";
        }
        row("world-float-mode", "In-game interaction: " + mode_label, 440);

        result.push_back({"apply", "Apply and save", {350, 496, 280, 52}});
        result.push_back({"hands-defaults", "Reset VR defaults", {650, 496, 280, 52}});
    } else if (settings_tab_ == 2) {
        auto row = [&](const std::string& kind, const std::string& name, double y) {
            result.push_back({kind + "-prev", "<", {40, y, 65, 52}});
            result.push_back({kind + "-label", name, {115, y, 1370, 52}});
            result.push_back({kind + "-next", ">", {1495, y, 65, 52}});
        };
        row("voice", "Voice typing: " + std::string(pending_.voice_enabled ? "Enabled" : "Disabled"),
            80);

        std::string model_status = " [not found]";
        auto resolved_model =
            find_speech_model(pending_.speech_model, options_.config_dir, options_.data_dir);
        std::error_code ec;
        if (fs::is_regular_file(resolved_model, ec)) {
            model_status = " [installed]";
        }
        row("speech-model", "Speech model: " + pending_.speech_model + model_status, 150);

        row("clipboard-opt",
            "Clipboard history (hold paste): " +
                std::string(pending_.clipboard_enabled ? "Enabled" : "Disabled"),
            220);

        row("haptics-opt",
            "Controller vibration haptics: " +
                std::string(pending_.haptics_enabled ? "Enabled" : "Disabled"),
            290);

        result.push_back({"apply", "Apply and save", {350, 400, 280, 56}});
        result.push_back({"features-defaults", "Reset to defaults", {650, 400, 280, 56}});
    } else {
        auto row = [&](const std::string& kind, const std::string& name, double y) {
            result.push_back({kind + "-prev", "<", {40, y, 65, 48}});
            result.push_back({kind + "-label", name, {115, y, 1370, 48}});
            result.push_back({kind + "-next", ">", {1495, y, 65, 48}});
        };
        row("capture-opt",
            "Camera recording button: " + std::string(pending_.capture_enabled ? "Enabled" : "Disabled"),
            72);

        row("capture-quality",
            "Video quality: " + pending_.capture_quality +
                (pending_.capture_quality == "1080p High" ? " (recommended)" : ""),
            132);

        row("capture-save-dir", "Save recordings to: " + pending_.capture_save_dir, 192);

        row("capture-audio",
            "Capture microphone: " +
                std::string(pending_.capture_audio ? "Enabled (captures mic)" : "Disabled (video only)"),
            252);

        const std::string dev_name = VideoCapture::detect_camera_device();
        row("capture-device", "Camera source device: " + dev_name + " (SteamVR view)", 312);

        result.push_back({"apply", "Apply and save", {350, 400, 280, 56}});
        result.push_back({"capture-defaults", "Reset to defaults", {650, 400, 280, 56}});
    }
    return result;
}
PanelView App::view() const {
    PanelView v;
    v.layout = &profiles_.layouts.at(settings_.active.layout);
    v.theme = &profiles_.themes.at(settings_.active.theme);
    v.language = &profiles_.languages.at(settings_.active.language);
    v.keymap = keymap_.get();
    v.keyboard = &keyboard_;
    v.controls = controls();
    v.status = dragging_ ? "Release grab to place keyboard" : status_;
    // Image exports show the layout without the interactive preview's input notice.
    const bool image_export = options_.mode == "render" || options_.mode == "render-settings";
    if (v.status.empty() && !gate_.enabled && !image_export) {
        v.status = options_.input == "none"
                       ? "Preview only: this launch cannot type."
                       : "Typing disabled: choose a profile matching the target keymap used at launch.";
    }
    v.settings = settings_open_;
    v.clipboard = clipboard_open_;
    v.composing = japanese() && !japanese_latin_;
    if (v.composing) {
        v.preedit = composition_.preedit(true);
        if (v.language->input_method == "japanese-kana") {
            const auto mods = keyboard_.modifiers();
            const bool shifted =
                mods.contains(key_code("ShiftLeft")) || mods.contains(key_code("ShiftRight"));
            for (const auto& key : v.layout->keys) {
                const auto& map = shifted && v.language->kana_shift.contains(key.action)
                                      ? v.language->kana_shift
                                      : v.language->kana;
                if (auto it = map.find(key.action); it != map.end()) {
                    v.key_labels[key.id] = it->second;
                }
            }
        }
    }
    if (cjk_ && !cjk_latin_) {
        v.composing = true;
        v.preedit = cjk_->preedit();
        const auto mods = keyboard_.modifiers();
        const bool shifted =
            mods.contains(key_code("ShiftLeft")) || mods.contains(key_code("ShiftRight"));
        for (const auto& key : v.layout->keys) {
            const auto& map = shifted && v.language->composition_shift.contains(key.action)
                                  ? v.language->composition_shift
                                  : v.language->composition_keys;
            if (const auto it = map.find(key.action); it != map.end()) {
                v.key_labels[key.id] = it->second;
            }
        }
    }
    if (!backend_ready_) {
        v.status = backend_can_resume_ ? "Keyboard paused by compositor. Waiting to resume."
                                       : "Keyboard connection lost. Reopen the app to reconnect.";
    } else if ((unicode_mode() || japanese() || cjk_) && options_.input != "none" && !text_ready_) {
        v.status = "Text delivery unavailable. Reopen the app after the compositor connection recovers.";
    }
    for (const auto& [pointer, id] : hovered_) {
        (void)pointer;
        v.hovered.insert(id);
    }
    return v;
}
bool App::move(unsigned pointer, double x, double y) {
    if (!interaction_active_) {
        return false;
    }
    const auto v = view();
    std::string id;
    bool keyboard_key = false;
    for (const auto& control : v.controls) {
        if (control.bounds.contains(x, y)) {
            id = control.id;
        }
    }
    if (id.empty() && !v.settings && !v.clipboard) {
        if (const auto* key = renderer.hit_key(v, x, y)) {
            id = key->id;
            keyboard_key = true;
        }
    }
    if (hovered_[pointer] != id) {
        hovered_[pointer] = id;
        dirty = true;
        return keyboard_key && !dragging_ && !keyboard_.pointer_pressed(pointer);
    }
    return false;
}

bool App::down(unsigned pointer, double x, double y, double now) {
    if (dragging_ || !interaction_active_) {
        return false;
    }
    move(pointer, x, y);
    const auto v = view();
    for (const auto& control : v.controls) {
        if (control.bounds.contains(x, y)) {
            pressed_controls_[pointer] = control.id;
            if (control.id == "capture") {
                capture_down_time_[pointer] = now;
            }
            return false;
        }
    }
    if (clipboard_open_) {
        // Tapping outside modal controls dismisses the clipboard modal
        clipboard_open_ = false;
        status_.clear();
        dirty = true;
        return false;
    }
    if (const auto* key = renderer.hit_key(v, x, y)) {
        const auto modifiers = keyboard_.modifiers();
        const bool ime_local = japanese_key(*key, false, modifiers) || cjk_key(*key, false, modifiers);
        const bool text_local = !ime_local && text_key(*key, modifiers);
        const bool cancel_accent = unicode_mode() && keymap_->composing() &&
                                   (key->action == "Escape" || key->action == "Backspace");
        const bool retry_text = !pending_text_.empty() && key->action == "Enter";
        const bool local = ime_local || text_local || cancel_accent || retry_text;
        if (!backend_ready_ || keyboard_.pointer_pressed(pointer)) {
            return false;
        }
        const bool sends_action = key->action_kind == ActionKind::Shortcut ||
                                  !is_modifier(key_code(key->action)) || !key->sticky;
        if (!pending_text_.empty() && sends_action) {
            if (!flush_text()) {
                dirty = true;
                return false;
            }
        }
        if (!ime_local && (!composition_.empty() || (cjk_ && !cjk_->empty())) && sends_action) {
            // Commit before either native events or ordinary Unicode text. Spaces,
            // digits and punctuation must not overtake the unfinished IME word.
            try {
                commit_japanese();
                commit_cjk();
            } catch (const std::exception& error) {
                status_ = error.what();
            }
            dirty = true;
            if (!composition_.empty() || (cjk_ && !cjk_->empty())) {
                return false; // Failed commit must not lose text or change the target.
            }
        }
        // Capture the symbol before a tapped Shift/AltGr is consumed by down().
        const auto symbol = text_local
                                ? keymap_->symbol(*key, modifiers, keyboard_.caps(), keyboard_.num())
                                : XKB_KEY_NoSymbol;
        const bool accepted =
            keyboard_.down(pointer, *key, now, local, local ? 0 : native_code(*key, modifiers));
        if (accepted) {
            if (key->action == "paste" && clipboard_enabled()) {
                paste_down_time_[pointer] = now;
            }
            try {
                if (ime_local) {
                    japanese_key(*key, true, modifiers);
                    cjk_key(*key, true, modifiers);
                } else if (cancel_accent) {
                    keymap_->cancel_compose();
                    status_.clear();
                } else if (text_local && key->action != "CapsLock" && key->action != "NumLock") {
                    pending_text_ = keymap_->compose(symbol);
                    const auto repeated = pending_text_;
                    if (flush_text() && !repeated.empty()) {
                        text_repeats_[pointer] = {repeated, now + .5};
                    }
                    if (keymap_->composing()) {
                        status_ = "Accent pending";
                    }
                } else if (!local) {
                    keymap_->cancel_compose();
                    status_.clear();
                }
            } catch (const std::exception& error) {
                status_ = error.what();
            }
        }
        dirty |= accepted;
        return accepted;
    }
    return false;
}

bool App::up(unsigned pointer, double x, double y) {
    if (!interaction_active_) {
        return false;
    }
    text_repeats_.erase(pointer);
    if (paste_long_pressed_.erase(pointer)) {
        paste_down_time_.erase(pointer);
        const bool key_released = keyboard_.up(pointer);
        dirty = true;
        return key_released;
    }
    if (paste_down_time_.erase(pointer)) {
        if (options_.input != "none" && gate_.enabled) {
            gate_.send(KEY_LEFTCTRL, 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            gate_.send(KEY_V, 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            gate_.send(KEY_V, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            gate_.send(KEY_LEFTCTRL, 0);
        }
    }
    if (capture_long_pressed_.erase(pointer)) {
        capture_down_time_.erase(pointer);
        pressed_controls_.erase(pointer);
        dirty = true;
        return true;
    }
    capture_down_time_.erase(pointer);
    const bool key_released = keyboard_.up(pointer);
    move(pointer, x, y);
    const auto it = pressed_controls_.find(pointer);
    if (it != pressed_controls_.end()) {
        const auto id = it->second;
        pressed_controls_.erase(it);
        if (hovered_[pointer] == id) {
            try {
                action(id);
            } catch (const std::exception& error) {
                status_ = error.what();
            }
        }
    }
    dirty = true;
    return key_released;
}

void App::cancel_pointer(unsigned pointer) {
    text_repeats_.erase(pointer);
    paste_down_time_.erase(pointer);
    paste_long_pressed_.erase(pointer);
    capture_down_time_.erase(pointer);
    capture_long_pressed_.erase(pointer);
    keyboard_.cancel_pointer(pointer);
    pressed_controls_.erase(pointer);
    hovered_.erase(pointer);
    dirty = true;
}
void App::cancel(bool discard_composition) {
    text_repeats_.clear();
    paste_down_time_.clear();
    paste_long_pressed_.clear();
    capture_down_time_.clear();
    capture_long_pressed_.clear();
    clipboard_open_ = false;
    if (dictation_) {
        dictation_->cancel();
    }
    if (discard_composition) {
        pending_text_.clear();
        if (keymap_) {
            keymap_->cancel_compose();
        }
        composition_.cancel();
        if (cjk_) {
            cjk_->cancel();
        }
    }
    keyboard_.cancel_all();
    pressed_controls_.clear();
    hovered_.clear();
    dirty = true;
}
bool App::tick(double now) {
    refresh_typing();
    dirty |= keyboard_.tick(now);
    if (dictation_ && dictation_->active()) {
        for (const auto& ev : dictation_->poll(now)) {
            switch (ev.kind) {
            case Dictation::Event::Kind::Listening:
                status_ = "Listening... speak now, tap microphone when done";
                dirty = true;
                break;
            case Dictation::Event::Kind::Hearing:
                status_ = "Hearing speech... tap microphone when done";
                dirty = true;
                break;
            case Dictation::Event::Kind::Transcribing:
                status_ = "Transcribing speech...";
                dirty = true;
                break;
            case Dictation::Event::Kind::Text: {
                VoiceResult vr = parse_voice_result(ev.payload);
                if (vr.command != VoiceCommand::NoCommand) {
                    execute_voice_command(vr.command);
                } else if (!vr.text.empty()) {
                    status_ = "Recognized: " + vr.text;
                    type_dictation(vr.text + " ");
                    push_clipboard(vr.text);
                } else {
                    status_ = "No speech detected";
                }
                dirty = true;
                break;
            }
            case Dictation::Event::Kind::Error:
                status_ = ev.payload;
                dirty = true;
                break;
            }
        }
    }
    if (capture_ && capture_->active()) {
        capture_->poll(now);
        if (!capture_->active()) {
            status_ = capture_->last_error().empty() ? "Recording stopped"
                                                     : ("Capture error: " + capture_->last_error());
            dirty = true;
        } else if (now - last_capture_tick_ >= 1.0) {
            last_capture_tick_ = now;
            const int secs = static_cast<int>(capture_->duration(now));
            const int mins = secs / 60;
            const int rem_secs = secs % 60;
            char dur_buf[32];
            std::snprintf(dur_buf, sizeof(dur_buf), "[%02d:%02d]", mins, rem_secs);
            status_ = "Recording camera " + std::string(dur_buf) + " · Tap camera icon to stop";
            dirty = true;
        }
    }
    for (auto it = capture_down_time_.begin(); it != capture_down_time_.end();) {
        if (now - it->second >= 0.45) {
            capture_long_pressed_.insert(it->first);
            it = capture_down_time_.erase(it);
            show_settings();
            settings_tab_ = 3;
            status_ = "Camera capture settings";
            dirty = true;
            continue;
        }
        ++it;
    }
    if (copy_poll_pending_ && now >= copy_poll_time_) {
        copy_poll_pending_ = false;
        std::string text = read_system_clipboard();
        if (!text.empty()) {
            push_clipboard(text);
            dirty = true;
        }
    }
    for (auto it = paste_down_time_.begin(); it != paste_down_time_.end();) {
        if (now - it->second >= 0.35) {
            paste_long_pressed_.insert(it->first);
            it = paste_down_time_.erase(it);
            open_clipboard();
            dirty = true;
            continue;
        }
        ++it;
    }
    for (auto it = text_repeats_.begin(); it != text_repeats_.end();) {
        if (now >= it->second.next) {
            if (options_.input != "none" && !gate_.commit_text(it->second.text)) {
                // Stop a repeat after an interruption; never replay a backlog.
                it = text_repeats_.erase(it);
                continue;
            }
            it->second.next = now + .04;
        }
        ++it;
    }
    return dirty || renderer.animating();
}
void App::paint(double now) {
    renderer.paint(view(), now);
    dirty = false;
}
void App::set_interaction_active(bool active) {
    if (active == interaction_active_) {
        return;
    }
    // Release through the still-open gate; disabling first would strand held
    // modifiers in the compositor. Also discard preedit and queued UI clicks.
    cancel();
    interaction_active_ = active;
    if (active) {
        refresh_typing();
    } else {
        gate_.enabled = false;
    }
}
void App::set_dragging(bool dragging) {
    if (dragging_ == dragging) {
        return;
    }
    cancel();
    dragging_ = dragging;
    status_.clear();
}
void App::report_status(const std::string& message) {
    status_ = message;
    dirty = true;
}
void App::show_settings() {
    cancel();
    settings_open_ = true;
    settings_tab_ = 0;
    pending_ = settings_.active;
    dirty = true;
}
void App::summon() {
    // Release held keys before moving; keep the input connection unchanged.
    cancel();
    settings_open_ = false;
    placement_actions_.clear();
    recenter_ = true;
    if (capture_ && capture_->active()) {
        const int secs = static_cast<int>(capture_->duration(monotonic_seconds()));
        const int mins = secs / 60;
        const int rem_secs = secs % 60;
        char dur_buf[32];
        std::snprintf(dur_buf, sizeof(dur_buf), "[%02d:%02d]", mins, rem_secs);
        status_ = "Recording camera " + std::string(dur_buf) + " · Tap camera icon to stop";
    } else {
        status_.clear();
    }
}
std::vector<PlacementAction> App::take_placement_actions() {
    std::vector<PlacementAction> result;
    result.swap(placement_actions_);
    return result;
}
void App::apply(Selection selection) {
    validate_selection(profiles_, selection);
    auto keymap = std::make_unique<LanguageMap>(profiles_.languages.at(selection.language));
    auto cjk = prepare_cjk(profiles_.languages.at(selection.language));
    cancel();
    japanese_latin_ = false;
    cjk_latin_ = false;
    cjk_ = std::move(cjk);
    const bool model_changed = (selection.speech_model != settings_.active.speech_model);
    settings_.active = std::move(selection);
    keymap_ = std::move(keymap);
    pending_ = settings_.active;
    world_float_ = settings_.active.world_float_enabled;
    if (model_changed || !dictation_) {
        if (dictation_) {
            dictation_->cancel();
        }
        auto helper = find_speech_helper();
        auto model =
            find_speech_model(options_.speech_model.empty() ? fs::path(settings_.active.speech_model)
                                                            : options_.speech_model,
                              options_.config_dir, options_.data_dir);
        dictation_ = std::make_unique<Dictation>(std::move(helper), std::move(model));
    }
    refresh_typing();
    status_.clear();
    try {
        save_selection(options_.config_dir, settings_.active);
    } catch (const std::exception& error) {
        status_ = "Applied, but not saved: " + std::string(error.what());
    }
    dirty = true;
}
void App::reload() {
    auto candidate = load_profiles(options_.data_dir, options_.config_dir, &profiles_);
    validate_selection(candidate, settings_.active);
    // Preparing a keymap can fail. Finish that work before releasing the old model.
    auto keymap = std::make_unique<LanguageMap>(candidate.languages.at(settings_.active.language));
    auto cjk = prepare_cjk(candidate.languages.at(settings_.active.language));
    cancel();
    cjk_ = std::move(cjk);
    profiles_ = std::move(candidate);
    keymap_ = std::move(keymap);
    const auto updated_settings = load_settings(options_.config_dir, profiles_.errors);
    settings_.favorites = updated_settings.favorites;
    favorite_index_ = 0;
    pending_ = settings_.active;
    refresh_typing();
    status_ = profiles_.errors.empty() ? "" : profiles_.errors.front();
}
bool App::japanese() const {
    return profiles_.languages.at(settings_.active.language).input_method.starts_with("japanese-");
}
void App::refresh_typing() {
    const bool ready = gate_.pump();
    const bool reset = gate_.take_input_reset();
    const bool can_resume = gate_.can_resume();
    if (reset || (backend_ready_ && !ready)) {
        // Clear UI holds even if input was already disabled by a hidden dashboard.
        // The backend has released its keys; never replay those holds on resume.
        cancel();
    }
    dirty |= ready != backend_ready_ || can_resume != backend_can_resume_;
    backend_ready_ = ready;
    backend_can_resume_ = can_resume;
    const auto& language = profiles_.languages.at(settings_.active.language);
    text_ready_ = (unicode_mode() || japanese() || cjk_) && gate_.text_available();
    // Unicode mode owns character mapping. Only physical-only backends and
    // external JIS still rely on a declared receiving-session keymap.
    const bool matching = target_language_ && same_keymap(*target_language_, language) &&
                          (japanese() || cjk_ || options_.target_language == settings_.active.language);
    gate_.enabled =
        interaction_active_ && options_.start_enabled && options_.mode == "vr" &&
        (options_.input == "ei" || options_.input == "uinput") &&
        (unicode_mode() ? text_ready_ : matching && (!(japanese() || cjk_) || text_ready_)) &&
        backend_ready_;
}
void App::commit_japanese() {
    const auto text = composition_.commit_text();
    if (text.empty()) {
        return;
    }
    if (options_.input == "none") {
        composition_.cancel();
        status_ = "Preview: composition committed locally.";
    } else if (gate_.commit_text(text)) {
        composition_.cancel();
        status_.clear();
    } else {
        status_ = "Text was not sent. Release other keys and check the target/input connection.";
    }
}
bool App::japanese_key(const Key& key, bool execute, const std::set<int>& mods) {
    if (!japanese() || key.action_kind != ActionKind::Key) {
        return false;
    }
    const auto& code = key.action;
    if (code == "ZenkakuHankaku" || code == "KanaMode") {
        if (execute) {
            composition_.cancel();
            japanese_latin_ = !japanese_latin_;
        }
        return true;
    }
    if (japanese_latin_) {
        return false;
    }
    for (const char* modifier :
         {"ControlLeft", "ControlRight", "AltLeft", "AltRight", "MetaLeft", "MetaRight"}) {
        if (mods.contains(key_code(modifier))) {
            return false;
        }
    }
    const bool shift = mods.contains(key_code("ShiftLeft")) || mods.contains(key_code("ShiftRight"));
    const bool composing = !composition_.empty();
    if (code == "Convert" || (code == "Space" && composing)) {
        if (execute) {
            if (shift && composition_.converting()) {
                composition_.cycle(-1);
            } else {
                composition_.convert();
            }
        }
        return true;
    }
    if (code == "NonConvert" || (composing && (code == "F6" || code == "F7"))) {
        if (execute) {
            composition_.script(code == "F7");
        }
        return true;
    }
    if (composing &&
        (code == "Enter" || code == "NumpadEnter" || code == "Backspace" || code == "Escape" ||
         code == "ArrowLeft" || code == "ArrowRight" || code == "ArrowUp" || code == "ArrowDown")) {
        if (execute) {
            if (code == "Enter" || code == "NumpadEnter") {
                commit_japanese();
            } else if (code == "Backspace") {
                composition_.backspace();
            } else if (code == "Escape") {
                if (composition_.converting()) {
                    composition_.unconvert();
                } else {
                    composition_.cancel();
                }
            } else if (code == "ArrowLeft" || code == "ArrowRight") {
                composition_.segment(code == "ArrowLeft" ? -1 : 1);
            } else {
                composition_.cycle(code == "ArrowUp" ? -1 : 1);
            }
        }
        return true;
    }
    const auto& language = profiles_.languages.at(settings_.active.language);
    if (language.input_method == "japanese-kana") {
        const auto& map =
            shift && language.kana_shift.contains(code) ? language.kana_shift : language.kana;
        if (auto it = map.find(code); it != map.end()) {
            if (execute) {
                if (composition_.converting()) {
                    commit_japanese();
                    if (!composition_.empty()) {
                        return true;
                    }
                }
                composition_.kana(it->second == "゛"   ? "\u3099"
                                  : it->second == "゜" ? "\u309a"
                                                       : it->second);
            }
            return true;
        }
        return false;
    }
    const auto legend = keymap_->legend(key, mods, keyboard_.caps(), keyboard_.num());
    if (legend.size() == 1 && g_ascii_isprint(legend[0]) && !is_modifier(key_code(code))) {
        if (execute) {
            if (composition_.converting()) {
                commit_japanese();
                if (!composition_.empty()) {
                    return true;
                }
            }
            composition_.roman(legend[0]);
        }
        return true;
    }
    return false;
}
bool App::unicode_mode() const {
    const auto& language = profiles_.languages.at(settings_.active.language);
    // External JIS explicitly delegates conversion to a system IME.
    const bool external_jis = language.input_method == "xkb" && language.keymap == "jp";
    return options_.input != "uinput" && !external_jis;
}
bool App::text_key(const Key& key, const std::set<int>& mods) const {
    if (!unicode_mode() || key.action_kind != ActionKind::Key) {
        return false;
    }
    if (key.action == "CapsLock" || key.action == "NumLock") {
        return true;
    }
    for (const auto* name : {"ControlLeft", "ControlRight", "AltLeft", "MetaLeft", "MetaRight"}) {
        if (mods.contains(key_code(name))) {
            return false;
        }
    }
    if (mods.contains(key_code("AltRight"))) {
        Key alt;
        alt.action_kind = ActionKind::Key;
        alt.action = "AltRight";
        if (keymap_->symbol(alt, {}, false, false) != XKB_KEY_ISO_Level3_Shift) {
            return false;
        }
    }
    const auto symbol = keymap_->symbol(key, mods, keyboard_.caps(), keyboard_.num());
    return symbol == XKB_KEY_KP_Begin || LanguageMap::printable(symbol);
}
bool App::flush_text() {
    if (pending_text_.empty()) {
        return true;
    }
    if (options_.input == "none" || gate_.commit_text(pending_text_)) {
        pending_text_.clear();
        status_.clear();
        return true;
    }
    status_ = "Text not sent. Release other keys, then press Enter to retry.";
    return false;
}
void App::type_dictation(const std::string& text) {
    if (options_.input == "none" || !gate_.enabled) {
        return;
    }
    for (std::size_t i = 0; i < text.size();) {
        unsigned char b = static_cast<unsigned char>(text[i]);
        if (b < 0x80) {
            char c = text[i];
            if (auto stroke = ascii_to_keystroke(c)) {
                if (stroke->shift) {
                    gate_.send(KEY_LEFTSHIFT, 1);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                gate_.send(stroke->code, 1);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                gate_.send(stroke->code, 0);
                if (stroke->shift) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    gate_.send(KEY_LEFTSHIFT, 0);
                }
            }
            ++i;
        } else {
            const char* p = text.c_str() + i;
            const char* next = g_utf8_next_char(p);
            std::size_t len = static_cast<std::size_t>(next - p);
            gate_.commit_text(text.substr(i, len));
            i += len;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
void App::execute_voice_command(VoiceCommand command) {
    if (options_.input == "none" || !gate_.enabled) {
        return;
    }
    switch (command) {
    case VoiceCommand::Backspace:
        gate_.send(KEY_BACKSPACE, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        gate_.send(KEY_BACKSPACE, 0);
        status_ = "Command: Backspace";
        break;
    case VoiceCommand::ScratchThat:
        gate_.send(KEY_LEFTCTRL, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        gate_.send(KEY_BACKSPACE, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        gate_.send(KEY_BACKSPACE, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        gate_.send(KEY_LEFTCTRL, 0);
        status_ = "Command: Scratch that";
        break;
    case VoiceCommand::Enter:
        gate_.send(KEY_ENTER, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        gate_.send(KEY_ENTER, 0);
        status_ = "Command: Enter";
        break;
    case VoiceCommand::SelectAll:
        gate_.send(KEY_LEFTCTRL, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        gate_.send(KEY_A, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        gate_.send(KEY_A, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        gate_.send(KEY_LEFTCTRL, 0);
        status_ = "Command: Select all";
        break;
    case VoiceCommand::Clear:
        gate_.send(KEY_LEFTCTRL, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        gate_.send(KEY_A, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        gate_.send(KEY_A, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        gate_.send(KEY_LEFTCTRL, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        gate_.send(KEY_BACKSPACE, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        gate_.send(KEY_BACKSPACE, 0);
        status_ = "Command: Clear";
        break;
    default:
        break;
    }
}
void App::load_clipboard_history() {
    clipboard_history_.clear();
    const auto file_path = options_.config_dir / "clipboard.json";
    std::error_code ec;
    if (!fs::is_regular_file(file_path, ec)) {
        return;
    }
    std::ifstream file(file_path);
    if (!file.is_open()) {
        return;
    }
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (text.empty()) {
        return;
    }
    json_tokener* tok = json_tokener_new();
    if (!tok) {
        return;
    }
    json_object* obj = json_tokener_parse_ex(tok, text.c_str(), static_cast<int>(text.size()));
    json_tokener_free(tok);
    if (!obj || !json_object_is_type(obj, json_type_array)) {
        if (obj) {
            json_object_put(obj);
        }
        return;
    }
    const std::size_t len = json_object_array_length(obj);
    for (std::size_t i = 0; i < len && clipboard_history_.size() < 5; ++i) {
        json_object* item = json_object_array_get_idx(obj, i);
        if (item && json_object_is_type(item, json_type_string)) {
            const char* s = json_object_get_string(item);
            if (s && *s) {
                clipboard_history_.emplace_back(s);
            }
        }
    }
    json_object_put(obj);
}
void App::save_clipboard_history() {
    try {
        fs::create_directories(options_.config_dir);
    } catch (...) {
        return;
    }
    json_object* arr = json_object_new_array();
    for (const auto& item : clipboard_history_) {
        json_object_array_add(arr, json_object_new_string(item.c_str()));
    }
    const char* str = json_object_to_json_string_ext(arr, JSON_C_TO_STRING_PRETTY);
    if (str) {
        const auto file_path = options_.config_dir / "clipboard.json";
        std::ofstream file(file_path);
        if (file.is_open()) {
            file << str << '\n';
        }
    }
    json_object_put(arr);
}
void App::open_clipboard() {
    if (!clipboard_enabled()) {
        return;
    }
    std::string text = read_system_clipboard();
    if (!text.empty()) {
        push_clipboard(text);
    }
    clipboard_open_ = true;
    dirty = true;
}
void App::push_clipboard(const std::string& text) {
    if (text.empty()) {
        return;
    }
    auto it = std::find(clipboard_history_.begin(), clipboard_history_.end(), text);
    if (it != clipboard_history_.end()) {
        clipboard_history_.erase(it);
    }
    clipboard_history_.insert(clipboard_history_.begin(), text);
    if (clipboard_history_.size() > 5) {
        clipboard_history_.resize(5);
    }
    save_clipboard_history();
}
int App::native_code(const Key& key, const std::set<int>& mods) {
    if (!unicode_mode()) {
        return 0;
    }
    if (key.action_kind == ActionKind::Shortcut) {
        if (key.action == "copy") {
            copy_poll_pending_ = true;
            copy_poll_time_ = monotonic_seconds() + 0.15;
        }
        return gate_.shortcut_code(key.action == "copy" ? XKB_KEY_c : XKB_KEY_v,
                                   key_code(key.action == "copy" ? "KeyC" : "KeyV"));
    }
    if (key.action.starts_with("Numpad")) {
        // Num Lock is local in Unicode mode. Send explicit navigation codes,
        // so the receiving system's Num Lock cannot turn Home into '7'.
        switch (keymap_->symbol(key, mods, keyboard_.caps(), keyboard_.num())) {
        case XKB_KEY_KP_Home:
            return KEY_HOME;
        case XKB_KEY_KP_End:
            return KEY_END;
        case XKB_KEY_KP_Left:
            return KEY_LEFT;
        case XKB_KEY_KP_Right:
            return KEY_RIGHT;
        case XKB_KEY_KP_Up:
            return KEY_UP;
        case XKB_KEY_KP_Down:
            return KEY_DOWN;
        case XKB_KEY_KP_Prior:
            return KEY_PAGEUP;
        case XKB_KEY_KP_Next:
            return KEY_PAGEDOWN;
        case XKB_KEY_KP_Insert:
            return KEY_INSERT;
        case XKB_KEY_KP_Delete:
            return KEY_DELETE;
        default:
            break;
        }
    }
    auto shortcut_symbol = xkb_keysym_to_lower(keymap_->symbol(key, {}, false, false));
    if (!mods.empty() && (key.action.starts_with("Key") ||
                          (shortcut_symbol >= XKB_KEY_a && shortcut_symbol <= XKB_KEY_z))) {
        auto symbol = shortcut_symbol;
        // Cyrillic and IME shortcuts conventionally retain their Latin physical
        // counterparts; Latin layouts use their displayed letter (AZERTY A, etc.).
        if (symbol < XKB_KEY_a || symbol > XKB_KEY_z) {
            symbol = static_cast<xkb_keysym_t>(g_ascii_tolower(key.action.back()));
        }
        const int fallback = key_code(
            "Key" + std::string(1, static_cast<char>(g_ascii_toupper(static_cast<char>(symbol)))));
        return gate_.shortcut_code(symbol, fallback);
    }
    return 0;
}
void App::choose_cjk(int index) {
    if (!cjk_) {
        return;
    }
    cjk_->choose(index);
    if (!cjk_->has_reading()) {
        commit_cjk();
    }
}
void App::commit_cjk() {
    if (!cjk_ || cjk_->empty()) {
        return;
    }
    const auto text = cjk_->text();
    if (!text.empty() && (options_.input == "none" || gate_.commit_text(text))) {
        cjk_->cancel();
        status_.clear();
    } else {
        status_ = "Text was not sent. Release other keys and check the target/input connection.";
    }
}
bool App::cjk_key(const Key& key, bool execute, const std::set<int>& mods) {
    if (!cjk_ || cjk_latin_ || key.action_kind != ActionKind::Key) {
        return false;
    }
    for (const auto* name :
         {"ControlLeft", "ControlRight", "AltLeft", "AltRight", "MetaLeft", "MetaRight"}) {
        if (mods.contains(key_code(name))) {
            return false;
        }
    }
    const auto& code = key.action;
    const bool composing = !cjk_->empty();
    if (cjk_->chinese() && composing && code.size() == 6 && code.starts_with("Digit") &&
        code.back() >= '1' && code.back() <= '5' && !mods.contains(key_code("ShiftLeft")) &&
        !mods.contains(key_code("ShiftRight")) && !cjk_->candidates().empty()) {
        const int index = cjk_->selected() / 5 * 5 + code.back() - '1';
        if (execute && index < static_cast<int>(cjk_->candidates().size())) {
            choose_cjk(index);
        }
        return true;
    }
    if (composing &&
        (code == "Enter" || code == "NumpadEnter" || code == "Escape" || code == "Backspace" ||
         (cjk_->chinese() && (code == "Space" || code == "ArrowUp" || code == "ArrowDown")))) {
        if (execute) {
            if (code == "Backspace") {
                cjk_->backspace();
            } else if (code == "Escape") {
                cjk_->cancel();
            } else if (code == "ArrowUp" || code == "ArrowDown") {
                cjk_->cycle(code == "ArrowUp" ? -1 : 1);
            } else if (code == "Space" && !cjk_->candidates().empty()) {
                choose_cjk(cjk_->selected());
            } else {
                commit_cjk();
            }
        }
        return true;
    }
    // Use physical US letter positions for Pinyin and two-set Hangul. Caps Lock
    // does not choose doubled Korean jamo; only Shift does.
    if (code.starts_with("Key") && code.size() == 4) {
        char ch = g_ascii_tolower(code.back());
        if (!cjk_->chinese() &&
            (mods.contains(key_code("ShiftLeft")) || mods.contains(key_code("ShiftRight")))) {
            ch = g_ascii_toupper(ch);
        }
        if (execute) {
            cjk_->type(ch);
        }
        return true;
    }
    if (cjk_->chinese() && code == "Quote" && composing && !mods.contains(key_code("ShiftLeft")) &&
        !mods.contains(key_code("ShiftRight"))) {
        if (execute) {
            cjk_->type('\'');
        }
        return true;
    }
    return false;
}
void App::action(const std::string& id) {
    const std::map<std::string, PlacementAction> adjustments = {
        {"size-smaller", PlacementAction::Smaller}, {"size-larger", PlacementAction::Larger}};
    if (const auto adjustment = adjustments.find(id); adjustment != adjustments.end()) {
        cancel();
        placement_actions_.push_back(adjustment->second);
        return;
    }
    if (id == "clip-close") {
        clipboard_open_ = false;
        status_.clear();
        dirty = true;
        return;
    }
    if (id == "clip-clear") {
        clipboard_history_.clear();
        save_clipboard_history();
        status_ = "Clipboard history cleared";
        dirty = true;
        return;
    }
    if (id.starts_with("clip-") && id.size() > 5 && std::isdigit(static_cast<unsigned char>(id[5]))) {
        std::size_t idx = std::stoul(id.substr(5));
        if (idx < clipboard_history_.size()) {
            std::string text_to_paste = clipboard_history_[idx];
            type_dictation(text_to_paste);
            clipboard_open_ = false;
            status_ = "Pasted from clipboard";
            dirty = true;
        }
        return;
    }
    if (id == "dictate") {
        if (!voice_enabled()) {
            return;
        }
        if (!dictation_) {
            status_ = "Speech helper not available";
            dirty = true;
            return;
        }
        if (dictation_->state() == Dictation::State::Listening) {
            dictation_->finish();
            status_ = "Transcribing speech...";
            dirty = true;
        } else if (dictation_->state() == Dictation::State::Transcribing) {
            // Already transcribing
        } else {
            try {
                dictation_->start();
                status_ = "Listening... tap microphone when finished";
                dirty = true;
            } catch (const std::exception& error) {
                status_ = error.what();
                dirty = true;
            }
        }
        return;
    }
    if (id == "capture") {
        if (!capture_enabled()) {
            return;
        }
        if (!capture_) {
            status_ = "Capture system not available";
            dirty = true;
            return;
        }
        if (capture_->active()) {
            std::string saved_path, err;
            if (capture_->stop(&saved_path, &err)) {
                status_ = "Recording saved: " + fs::path(saved_path).filename().string();
            } else {
                status_ = "Capture error: " + err;
            }
            dirty = true;
        } else {
            std::string err;
            if (capture_->start(settings_.active.capture_save_dir, settings_.active.capture_quality,
                                settings_.active.capture_audio, "", &err)) {
                last_capture_tick_ = monotonic_seconds();
                status_ = "Recording camera [00:00] · Tap camera icon to stop";
            } else {
                status_ = "Capture failed: " + err;
            }
            dirty = true;
        }
        return;
    }
    if (id.starts_with("preset-ja-")) {
        pending_.language = id.substr(7);
        pending_.layout = pending_.language == "ja-romaji" ? "en-us-full" : "ja-jis-full";
        return;
    }
    if (id.starts_with("cjk-") && cjk_) {
        if (id == "cjk-toggle") {
            if (!cjk_->empty()) {
                commit_cjk();
                if (!cjk_->empty()) {
                    return;
                }
            }
            cjk_latin_ = !cjk_latin_;
        } else if (id == "cjk-commit") {
            commit_cjk();
        } else if (id == "cjk-cancel") {
            cjk_->cancel();
        } else if (id == "cjk-next" || id == "cjk-prev") {
            cjk_->cycle(id == "cjk-next" ? 1 : -1);
        } else if (id.starts_with("cjk-candidate-")) {
            choose_cjk(std::stoi(id.substr(14)));
        }
        return;
    }
    if (id.starts_with("ime-")) {
        if (id == "ime-toggle") {
            composition_.cancel();
            japanese_latin_ = !japanese_latin_;
        } else if (id == "ime-convert") {
            composition_.convert();
        } else if (id == "ime-commit") {
            commit_japanese();
        } else if (id == "ime-cancel") {
            composition_.cancel();
        } else if (id == "ime-hiragana" || id == "ime-katakana") {
            composition_.script(id == "ime-katakana");
        } else if (id == "ime-next" || id == "ime-prev") {
            composition_.cycle(id == "ime-next" ? 1 : -1);
        } else if (id == "ime-segment-next" || id == "ime-segment-prev") {
            composition_.segment(id == "ime-segment-next" ? 1 : -1);
        } else if (id.starts_with("ime-candidate-")) {
            composition_.choose(std::stoi(id.substr(14)));
        }
        return;
    }
    if (id == "close") {
        cancel();
        gate_.enabled = false;
        quit_ = true;
    } else if (id == "settings") {
        if (settings_open_) {
            cancel();
            settings_open_ = false;
        } else {
            show_settings();
        }
    } else if (id == "settings-tab-profiles") {
        settings_tab_ = 0;
        dirty = true;
    } else if (id == "settings-tab-hands") {
        settings_tab_ = 1;
        dirty = true;
    } else if (id == "settings-tab-features") {
        settings_tab_ = 2;
        dirty = true;
    } else if (id == "settings-tab-capture") {
        settings_tab_ = 3;
        dirty = true;
    } else if (id == "voice-prev" || id == "voice-next") {
        pending_.voice_enabled = !pending_.voice_enabled;
        dirty = true;
    } else if (id == "speech-model-prev" || id == "speech-model-next") {
        const std::vector<std::string> models = {"ggml-tiny.en.bin", "ggml-base.en.bin",
                                                 "ggml-small.en.bin", "ggml-medium.en.bin",
                                                 "ggml-small.bin"};
        auto it = std::find(models.begin(), models.end(), pending_.speech_model);
        std::size_t idx =
            (it == models.end()) ? 2 : static_cast<std::size_t>(std::distance(models.begin(), it));
        if (id == "speech-model-next") {
            idx = (idx + 1) % models.size();
        } else {
            idx = (idx + models.size() - 1) % models.size();
        }
        pending_.speech_model = models[idx];
        dirty = true;
    } else if (id == "clipboard-opt-prev" || id == "clipboard-opt-next") {
        pending_.clipboard_enabled = !pending_.clipboard_enabled;
        dirty = true;
    } else if (id == "haptics-opt-prev" || id == "haptics-opt-next") {
        pending_.haptics_enabled = !pending_.haptics_enabled;
        dirty = true;
    } else if (id == "hand-opt-prev" || id == "hand-opt-next") {
        pending_.hand_enabled = !pending_.hand_enabled;
        dirty = true;
    } else if (id == "hand-distance-prev" || id == "hand-distance-next") {
        const std::vector<int> distances = {6, 9, 12, 15, 20, 25};
        auto it = std::find(distances.begin(), distances.end(), pending_.hand_distance_cm);
        std::size_t idx =
            (it == distances.end()) ? 2 : static_cast<std::size_t>(std::distance(distances.begin(), it));
        if (id == "hand-distance-next") {
            idx = (idx + 1) % distances.size();
        } else {
            idx = (idx + distances.size() - 1) % distances.size();
        }
        pending_.hand_distance_cm = distances[idx];
        dirty = true;
    } else if (id == "hand-pitch-prev" || id == "hand-pitch-next") {
        const std::vector<int> angles = {0, 10, 15, 20, 25, 30, 35, 40};
        auto it = std::find(angles.begin(), angles.end(), pending_.hand_pitch_deg);
        std::size_t idx =
            (it == angles.end()) ? 4 : static_cast<std::size_t>(std::distance(angles.begin(), it));
        if (id == "hand-pitch-next") {
            idx = (idx + 1) % angles.size();
        } else {
            idx = (idx + angles.size() - 1) % angles.size();
        }
        pending_.hand_pitch_deg = angles[idx];
        dirty = true;
    } else if (id == "touch-typing-prev" || id == "touch-typing-next") {
        pending_.touch_typing_enabled = !pending_.touch_typing_enabled;
        dirty = true;
    } else if (id == "proximity-grab-prev" || id == "proximity-grab-next") {
        pending_.proximity_grab_enabled = !pending_.proximity_grab_enabled;
        dirty = true;
    } else if (id == "horizon-lock-prev" || id == "horizon-lock-next") {
        pending_.horizon_lock_enabled = !pending_.horizon_lock_enabled;
        dirty = true;
    } else if (id == "hands-defaults") {
        pending_.hand_enabled = true;
        pending_.hand_distance_cm = 12;
        pending_.hand_pitch_deg = 25;
        pending_.touch_typing_enabled = true;
        pending_.proximity_grab_enabled = true;
        pending_.horizon_lock_enabled = true;
        pending_.bumper_shift_enabled = true;
        pending_.world_float_enabled = false;
        pending_.world_float_mode = "dynamic";
        status_ = "VR & hand options reset to defaults";
        dirty = true;
    } else if (id == "bumper-shift-prev" || id == "bumper-shift-next") {
        pending_.bumper_shift_enabled = !pending_.bumper_shift_enabled;
        dirty = true;
    } else if (id == "world-float") {
        toggle_world_float();
    } else if (id == "world-float-opt-prev" || id == "world-float-opt-next") {
        pending_.world_float_enabled = !pending_.world_float_enabled;
        dirty = true;
    } else if (id == "world-float-mode-prev" || id == "world-float-mode-next") {
        const std::vector<std::string> modes = {"dynamic", "passive", "touch", "always"};
        auto it = std::find(modes.begin(), modes.end(), pending_.world_float_mode);
        std::size_t idx =
            (it == modes.end()) ? 0 : static_cast<std::size_t>(std::distance(modes.begin(), it));
        if (id.ends_with("next")) {
            idx = (idx + 1) % modes.size();
        } else {
            idx = (idx + modes.size() - 1) % modes.size();
        }
        pending_.world_float_mode = modes[idx];
        dirty = true;
    } else if (id == "capture-opt-prev" || id == "capture-opt-next") {
        pending_.capture_enabled = !pending_.capture_enabled;
        dirty = true;
    } else if (id == "capture-quality-prev" || id == "capture-quality-next") {
        const std::vector<std::string> qualities = {"1080p High", "1080p Standard", "720p Performance",
                                                    "1080p Ultra"};
        auto it = std::find(qualities.begin(), qualities.end(), pending_.capture_quality);
        std::size_t idx =
            (it == qualities.end()) ? 0 : static_cast<std::size_t>(std::distance(qualities.begin(), it));
        if (id == "capture-quality-next") {
            idx = (idx + 1) % qualities.size();
        } else {
            idx = (idx + qualities.size() - 1) % qualities.size();
        }
        pending_.capture_quality = qualities[idx];
        dirty = true;
    } else if (id == "capture-save-dir-prev" || id == "capture-save-dir-next") {
        const std::vector<std::string> dirs = {"~/Videos/Recordings", "~/Videos", "~/Desktop",
                                               "~/Downloads"};
        auto it = std::find(dirs.begin(), dirs.end(), pending_.capture_save_dir);
        std::size_t idx =
            (it == dirs.end()) ? 0 : static_cast<std::size_t>(std::distance(dirs.begin(), it));
        if (id == "capture-save-dir-next") {
            idx = (idx + 1) % dirs.size();
        } else {
            idx = (idx + dirs.size() - 1) % dirs.size();
        }
        pending_.capture_save_dir = dirs[idx];
        dirty = true;
    } else if (id == "capture-audio-prev" || id == "capture-audio-next") {
        pending_.capture_audio = !pending_.capture_audio;
        dirty = true;
    } else if (id == "capture-defaults") {
        pending_.capture_enabled = true;
        pending_.capture_quality = "1080p High";
        pending_.capture_save_dir = "~/Videos/Recordings";
        pending_.capture_audio = true;
        status_ = "Capture options reset to defaults";
        dirty = true;
    } else if (id == "features-defaults") {
        pending_.voice_enabled = true;
        pending_.speech_model = "ggml-small.en.bin";
        pending_.clipboard_enabled = true;
        pending_.hand_enabled = true;
        pending_.hand_distance_cm = 12;
        pending_.hand_pitch_deg = 25;
        pending_.touch_typing_enabled = true;
        pending_.proximity_grab_enabled = true;
        pending_.haptics_enabled = true;
        pending_.horizon_lock_enabled = true;
        pending_.capture_enabled = true;
        pending_.capture_quality = "1080p High";
        pending_.capture_save_dir = "~/Videos/Recordings";
        pending_.capture_audio = true;
        pending_.bumper_shift_enabled = true;
        status_ = "Feature options reset to defaults";
        dirty = true;
    } else if (id == "recenter") {
        cancel();
        placement_actions_.clear();
        recenter_ = true;
    } else if (id == "apply") {
        apply(pending_);
        settings_open_ = false;
    } else if (id == "reload") {
        reload();
    } else if (id == "layout-next" || id == "layout-prev") {
        cycle(profiles_.layouts, pending_.layout, id.ends_with("next"));
    } else if (id == "language-next" || id == "language-prev") {
        cycle(profiles_.languages, pending_.language, id.ends_with("next"));
        pending_.layout = compatible_layout(profiles_, pending_);
    } else if (id == "theme-next" || id == "theme-prev") {
        cycle(profiles_.themes, pending_.theme, id.ends_with("next"));
    } else if ((id == "favorite-next" || id == "favorite-prev") && !settings_.favorites.empty()) {
        const auto count = settings_.favorites.size();
        favorite_index_ = (favorite_index_ + (id.ends_with("next") ? 1 : count - 1)) % count;
    } else if (id == "favorite-use") {
        const auto& candidate = settings_.favorites.at(favorite_index_).selection;
        validate_selection(profiles_, candidate);
        pending_ = candidate;
    }
}
bool App::take_recenter() {
    const bool result = recenter_;
    recenter_ = false;
    return result;
}
void App::toggle_shift_lock() {
    const bool locked = keyboard_.toggle_shift_lock();
    status_ = locked ? "Shift Lock: ON" : "Shift Lock: OFF";
    dirty = true;
}
void App::toggle_world_float() {
    world_float_ = !world_float_;
    std::string mode_label = "Dynamic";
    if (settings_.active.world_float_mode == "passive") {
        mode_label = "Passive";
    } else if (settings_.active.world_float_mode == "touch") {
        mode_label = "Direct touch";
    } else if (settings_.active.world_float_mode == "always") {
        mode_label = "Always laser";
    }
    status_ = world_float_ ? ("Floating in world: ON (" + mode_label + ")")
                           : "Floating in world: OFF (dashboard only)";
    dirty = true;
}
} // namespace framekeyboard
