#pragma once

#include "capture.hpp"
#include "cjk.hpp"
#include "dictation.hpp"
#include "instance.hpp"
#include "japanese.hpp"
#include "panel.hpp"
#include "placement.hpp"
#include <csignal>
#include <optional>

namespace framekeyboard {
struct Options {
    fs::path data_dir, config_dir, ei_socket, text_socket, speech_model;
    std::string mode{"help"}, output, input{"none"}, target_language;
    double duration{};
    bool start_enabled{false};
};

// Development previews never send input. The installed typing launcher opts in.
class InputGate : public KeySink {
  public:
    explicit InputGate(KeySink& target) : target_(target) {}
    void send(int code, int value) override {
        if (enabled) {
            target_.send(code, value);
        }
    }
    bool pump() override { return target_.pump(); }
    bool take_input_reset() override { return target_.take_input_reset(); }
    bool can_resume() const override { return target_.can_resume(); }
    bool text_available() override { return target_.text_available(); }
    bool commit_text(const std::string& text) override { return enabled && target_.commit_text(text); }
    int shortcut_code(xkb_keysym_t symbol, int fallback) override {
        return target_.shortcut_code(symbol, fallback);
    }
    bool enabled{false};

  private:
    KeySink& target_;
};
class App {
  public:
    App(const Options& options, KeySink& sink);
    PanelView view() const;
    // True only for a newly accepted keyboard key, so VR can provide haptics.
    bool down(unsigned pointer, double x, double y, double now);
    // True when an unpressed pointer enters a different keyboard key.
    bool move(unsigned pointer, double x, double y);
    // True when this pointer releases a captured keyboard key, even outside it.
    bool up(unsigned pointer, double x, double y);
    bool pointer_pressed(unsigned pointer) const { return keyboard_.pointer_pressed(pointer); }
    void cancel_pointer(unsigned pointer);
    void cancel(bool discard_composition = true);
    bool tick(double now);
    void paint(double now);
    void show_settings();
    void summon();
    void set_dragging(bool dragging);
    // Hiding the VR context releases keys before closing the input gate.
    void set_interaction_active(bool active);
    void report_status(const std::string& message);
    std::vector<PlacementAction> take_placement_actions();
    void apply(Selection selection);
    void reload();
    const fs::path& config_dir() const { return options_.config_dir; }
    const fs::path& data_dir() const { return options_.data_dir; }
    bool quitting() const { return quit_; }
    bool take_recenter();
    void open_clipboard();
    void push_clipboard(const std::string& text);
    bool dirty{true};
    PanelRenderer renderer;
    const Selection& selection() const { return settings_.active; }
    bool hand_enabled() const { return settings_.active.hand_enabled; }
    double hand_distance_meters() const {
        return std::max(0.03, settings_.active.hand_distance_cm / 100.0);
    }
    int hand_pitch_degrees() const { return settings_.active.hand_pitch_deg; }
    bool voice_enabled() const { return settings_.active.voice_enabled; }
    bool clipboard_enabled() const { return settings_.active.clipboard_enabled; }
    bool touch_typing_enabled() const { return settings_.active.touch_typing_enabled; }
    bool proximity_grab_enabled() const { return settings_.active.proximity_grab_enabled; }
    bool haptics_enabled() const { return settings_.active.haptics_enabled; }
    bool horizon_lock_enabled() const { return settings_.active.horizon_lock_enabled; }
    bool capture_enabled() const { return settings_.active.capture_enabled; }
    const std::string& capture_quality() const { return settings_.active.capture_quality; }
    const std::string& capture_save_dir() const { return settings_.active.capture_save_dir; }
    bool capture_audio() const { return settings_.active.capture_audio; }
    bool capture_active() const { return capture_ && capture_->active(); }

  private:
    std::vector<Control> controls() const;
    void action(const std::string& id);
    void refresh_typing();
    bool japanese_key(const Key& key, bool execute, const std::set<int>& mods);
    void commit_japanese();
    bool japanese() const;
    bool cjk_key(const Key& key, bool execute, const std::set<int>& mods);
    void commit_cjk();
    void choose_cjk(int index);
    bool unicode_mode() const;
    bool text_key(const Key& key, const std::set<int>& mods) const;
    int native_code(const Key& key, const std::set<int>& mods);
    bool flush_text();
    void type_dictation(const std::string& text);
    void execute_voice_command(VoiceCommand command);
    std::string pending_text_;
    struct TextRepeat {
        std::string text;
        double next;
    };
    std::map<unsigned, TextRepeat> text_repeats_;
    Options options_;
    Profiles profiles_;
    Settings settings_;
    Selection pending_;
    std::unique_ptr<LanguageMap> keymap_;
    // The launch declaration is frozen; Reload cannot redefine the target.
    std::optional<Language> target_language_;
    InputGate gate_;
    KeyboardState keyboard_;
    JapaneseComposer composition_;
    std::unique_ptr<CjkComposer> cjk_;
    std::unique_ptr<Dictation> dictation_;
    bool cjk_latin_{};
    bool japanese_latin_{};
    bool text_ready_{};
    std::map<unsigned, std::string> hovered_, pressed_controls_;
    std::string status_;
    bool settings_open_{false}, quit_{false}, recenter_{false};
    int settings_tab_{0};
    std::vector<PlacementAction> placement_actions_;
    std::size_t favorite_index_{};
    bool dragging_{};
    bool interaction_active_{true};
    bool backend_ready_{true}, backend_can_resume_{};
    bool clipboard_open_{false};
    std::vector<std::string> clipboard_history_;
    void load_clipboard_history();
    void save_clipboard_history();
    double copy_poll_time_{0.0};
    bool copy_poll_pending_{false};
    std::map<unsigned, double> paste_down_time_;
    std::set<unsigned> paste_long_pressed_;
    std::unique_ptr<VideoCapture> capture_;
    std::map<unsigned, double> capture_down_time_;
    std::set<unsigned> capture_long_pressed_;
    double last_capture_tick_{0.0};
};

double monotonic_seconds();
extern volatile std::sig_atomic_t interrupted;
int run_preview(App& app, double duration);
int run_vr(App& app, VrInstance& instance, double duration);
} // namespace framekeyboard
