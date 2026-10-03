#include "framekeyboard/app.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <linux/input-event-codes.h>
#include <stdexcept>
#include <unistd.h>

namespace fk = framekeyboard;
namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
template <class Function> void rejects(Function function, const char* message) {
    bool rejected = false;
    try {
        function();
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, message);
}
struct Capture : fk::KeySink {
    std::vector<std::pair<int, int>> events;
    void send(int code, int value) override { events.emplace_back(code, value); }
};
struct TemporaryDirectory {
    fk::fs::path path;
    TemporaryDirectory() {
        std::string name = "/tmp/framekeyboard-tests-XXXXXX";
        if (!mkdtemp(name.data())) {
            throw std::runtime_error("mkdtemp failed");
        }
        path = name;
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        fk::fs::remove_all(path, ignored);
    }
};
void write(const fk::fs::path& path, const std::string& text) {
    fk::fs::create_directories(path.parent_path());
    std::ofstream(path) << text;
}
const fk::Key& key(const fk::Layout& layout, const std::string& id) {
    for (const auto& key : layout.keys) {
        if (key.id == id) {
            return key;
        }
    }
    throw std::runtime_error("missing test key " + id);
}
void state_tests(const fk::Layout& layout) {
    Capture sink;
    fk::KeyboardState state(sink);
    const auto left_super = key(layout, "MetaLeft");
    const auto right_super = key(layout, "MetaRight");
    require(left_super.sticky && !right_super.sticky, "Super keys have distinct tap behavior");
    state.down(0, left_super, 0);
    state.up(0);
    require(sink.events.empty() && state.modifiers().contains(KEY_LEFTMETA),
            "left Super tap latches without opening a menu");
    state.down(0, key(layout, "KeyA"), .1);
    state.up(0);
    require(sink.events ==
                std::vector<std::pair<int, int>>{
                    {KEY_LEFTMETA, 1}, {KEY_A, 1}, {KEY_A, 0}, {KEY_LEFTMETA, 0}},
            "latched Super sends a native chord");
    sink.events.clear();
    state.down(0, right_super, 0);
    state.tick(20);
    require(sink.events == std::vector<std::pair<int, int>>{{KEY_RIGHTMETA, 1}},
            "right Super sends immediate down without repeating");
    state.up(0);
    require(sink.events == std::vector<std::pair<int, int>>{{KEY_RIGHTMETA, 1}, {KEY_RIGHTMETA, 0}} &&
                state.modifiers().empty(),
            "right Super sends up without latching");
    for (bool cancel_all : {false, true}) {
        sink.events.clear();
        state.down(0, right_super, 0);
        state.down(1, right_super, 0);
        state.up(0);
        require(sink.events.size() == 1, "overlapping Super holds share native ownership");
        if (cancel_all) {
            state.cancel_all();
        } else {
            state.cancel_pointer(1);
        }
        require(sink.events ==
                        std::vector<std::pair<int, int>>{{KEY_RIGHTMETA, 1}, {KEY_RIGHTMETA, 0}} &&
                    state.modifiers().empty(),
                "canceling Super releases its native hold without a latch");
    }
    sink.events.clear();
    state.down(0, key(layout, "ControlLeft"), 0);
    state.up(0);
    require(sink.events.empty(), "a latched modifier must not reach the OS until used");
    state.down(0, key(layout, "KeyA"), 1);
    state.up(0);
    require(sink.events ==
                std::vector<std::pair<int, int>>{
                    {KEY_LEFTCTRL, 1}, {KEY_A, 1}, {KEY_A, 0}, {KEY_LEFTCTRL, 0}},
            "Ctrl+A order");
    require(state.modifiers().empty(), "one-shot modifier consumed");
    sink.events.clear();
    state.down(0, key(layout, "Copy"), 2);
    state.up(0);
    require(sink.events ==
                std::vector<std::pair<int, int>>{
                    {KEY_LEFTCTRL, 1}, {KEY_C, 1}, {KEY_C, 0}, {KEY_LEFTCTRL, 0}},
            "Copy order");
    sink.events.clear();
    state.down(0, key(layout, "KeyA"), 3);
    state.down(1, key(layout, "KeyA"), 3);
    state.up(0);
    require(sink.events.size() == 1, "two pointers share one down");
    state.up(1);
    require(sink.events.size() == 2 && sink.events.back().second == 0, "last pointer releases key");
    for (const auto* id : {"CapsLock", "NumLock"}) {
        const auto lock = key(layout, id);
        const int code = fk::key_code(lock.action);
        auto locked = [&] { return code == KEY_CAPSLOCK ? state.caps() : state.num(); };
        // Exercise both release orders, and a new click while the other hand
        // still holds the key. Only the final release permits another toggle.
        for (unsigned first : {0u, 1u}) {
            sink.events.clear();
            require(!locked(), "lock starts off");
            state.down(0, lock, 3);
            state.down(1, lock, 3.1);
            require(locked(), "overlapping lock presses toggle only once");
            state.up(first);
            state.down(first, lock, 3.2);
            require(locked() && sink.events.size() == 1, "remaining hold prevents a second toggle");
            state.up(first);
            state.up(1 - first);
            require(sink.events == std::vector<std::pair<int, int>>{{code, 1}, {code, 0}},
                    "overlapping lock holds share one backend press and release");
            state.down(first, lock, 3.3);
            state.up(first);
            require(!locked() && sink.events.size() == 4, "next separate click toggles lock off");
        }
    }
    sink.events.clear();
    state.down(0, key(layout, "ShiftLeft"), 4);
    state.down(1, key(layout, "KeyA"), 4);
    state.up(0);
    state.up(1);
    require(state.modifiers().empty(), "a modifier used while held must not latch on release");
    for (const auto* modifier : {"ControlLeft", "ShiftLeft", "AltLeft", "MetaLeft", "MetaRight"}) {
        sink.events.clear();
        const int code = fk::key_code(modifier);
        state.down(0, key(layout, modifier), 4);
        state.down(1, key(layout, "KeyA"), 4.1);
        state.up(0);
        require(sink.events.back() == std::pair<int, int>{code, 0},
                "releasing held modifier reaches backend before ordinary key release");
        require(!state.modifiers().contains(code), "released modifier does not affect next key");
        state.down(0, key(layout, "KeyB"), 4.2);
        state.up(0);
        state.up(1);
        require(std::count(sink.events.begin(), sink.events.end(), std::pair<int, int>{code, 1}) == 1 &&
                    std::count(sink.events.begin(), sink.events.end(), std::pair<int, int>{code, 0}) ==
                        1,
                "next key neither reacquires nor double-releases the old modifier");
    }
    sink.events.clear();
    state.down(0, key(layout, "ControlLeft"), 4);
    state.down(1, key(layout, "KeyA"), 4.1);
    state.down(2, key(layout, "ControlLeft"), 4.2);
    state.up(0);
    require(state.modifiers().contains(KEY_LEFTCTRL), "another hold retains the same modifier");
    state.up(2);
    require(!state.modifiers().contains(KEY_LEFTCTRL), "last modifier hold releases without latching");
    state.up(1);
    sink.events.clear();
    state.down(0, key(layout, "Backspace"), 5);
    state.tick(5.49);
    require(sink.events.size() == 1, "repeat delay");
    state.tick(5.51);
    require(sink.events.back() == std::pair<int, int>{KEY_BACKSPACE, 2}, "repeat event");
    state.tick(40);
    require(state.active() && sink.events.back() == std::pair<int, int>{KEY_BACKSPACE, 2},
            "long hold keeps repeating without replaying a backlog");
    state.up(0);
    state.down(0, key(layout, "ShiftLeft"), 41);
    state.down(1, key(layout, "ArrowLeft"), 41);
    state.tick(80);
    require(state.modifiers().contains(KEY_LEFTSHIFT), "modifier survives a long two-hand chord");
    state.cancel_pointer(1);
    require(state.pointer_pressed(0) && state.modifiers().contains(KEY_LEFTSHIFT),
            "leaving with the other pointer does not release the held modifier");
    state.down(1, key(layout, "ArrowRight"), 81);
    state.cancel_pointer(0);
    require(!state.modifiers().contains(KEY_LEFTSHIFT) && state.pointer_pressed(1),
            "canceling a modifier releases borrowed references without canceling the other key");
    state.up(1);
    state.down(0, key(layout, "ControlLeft"), 82);
    state.cancel_pointer(0);
    require(state.modifiers().empty(), "canceled modifier tap must not latch");
    sink.events.clear();
    state.down(0, key(layout, "Enter"), 41);
    state.cancel_all();
    require(sink.events == std::vector<std::pair<int, int>>{{KEY_ENTER, 1}, {KEY_ENTER, 0}},
            "Enter and cancellation");
}
void profile_tests(const fk::Profiles& defaults) {
    TemporaryDirectory directory;
    write(
        directory.path / "layouts/custom.json",
        R"({"schema_version":1,"id":"compact","name":"Compact","width":200,"height":100,"keys":[{"id":"Enter","label":"Enter","x":0,"y":0,"width":100,"height":50,"action":{"kind":"key","value":"Enter"}}]})");
    auto loaded = fk::load_profiles({}, directory.path);
    require(loaded.layouts.contains("compact"), "discover user layout");
    write(directory.path / "layouts/custom.json", "{broken");
    auto reloaded = fk::load_profiles({}, directory.path, &loaded);
    require(reloaded.layouts.contains("compact") && !reloaded.errors.empty(),
            "retain valid profile after malformed edit");
    fk::fs::remove(directory.path / "layouts/custom.json");
    reloaded = fk::load_profiles({}, directory.path, &reloaded);
    require(!reloaded.layouts.contains("compact"), "deleted user profile disappears");
    write(
        directory.path / "config.json",
        R"({"schema_version":1,"active":{"layout":"en-us-full","language":"en-us","theme":"graphite"},"custom":42,"favorites":[{"id":"de","name":"Deutsch","layout":"en-us-full","language":"de-de","theme":"midnight"}]})");
    fk::save_selection(directory.path, {"en-us-full", "de-de", "midnight"});
    std::vector<std::string> errors;
    auto saved = fk::load_settings(directory.path, errors);
    require(saved.active.language == "de-de" && saved.favorites.size() == 1 && errors.empty(),
            "settings preserve selection and favorites");
    std::ifstream file(directory.path / "config.json");
    const std::string content((std::istreambuf_iterator<char>(file)), {});
    require(content.find("custom") != std::string::npos, "preserve unrelated settings");
    write(directory.path / "config.json", "{broken");
    rejects([&] { fk::save_selection(directory.path, {}); }, "do not overwrite malformed settings");
    rejects([] { fk::parse_layout(R"({"schema_version":2})"); }, "reject future schema");
    rejects(
        [] {
            fk::parse_layout(
                R"({"schema_version":1,"id":"bad","name":"Bad","width":200,"height":100,"keys":[{"id":"A","label":"A","x":0,"y":0,"width":100,"height":50,"action":{"kind":"key","value":"KeyA"}},{"id":"B","label":"B","x":50,"y":0,"width":100,"height":50,"action":{"kind":"key","value":"KeyB"}}]})");
        },
        "reject overlaps");
    fk::LanguageMap english(defaults.languages.at("en-us")), german(defaults.languages.at("de-de"));
    const auto& layout = defaults.layouts.at("en-us-full");
    require(english.legend(key(layout, "KeyY"), {}, false, false) == "y", "US Y legend");
    require(english.legend(key(layout, "KeyY"), {KEY_LEFTSHIFT}, false, false) == "Y",
            "Shift uppercases letter legends");
    require(english.legend(key(layout, "KeyY"), {}, true, false) == "Y",
            "Caps Lock uppercases letter legends");
    require(english.legend(key(layout, "KeyY"), {KEY_RIGHTSHIFT}, true, false) == "y",
            "Shift with Caps Lock matches lowercase key output");
    require(german.legend(key(layout, "KeyY"), {}, false, false) == "z", "German Z legend");
    require(german.legend(key(layout, "BracketLeft"), {}, false, false) == "ü", "German umlaut");
    require(german.legend(key(layout, "Backquote"), {}, false, false) == "^", "German dead-key legend");
    rejects([&] { fk::validate_selection(defaults, {"en-us-full", "de-de", "graphite"}); },
            "reject missing language key");
    require(german.legend(key(layout, "KeyQ"), {KEY_RIGHTALT}, false, false) == "@",
            "German AltGr legend");
}
void app_tests() {
    TemporaryDirectory directory;
    fk::Options options;
    options.config_dir = directory.path;
    Capture sink;
    fk::App app(options, sink);
    app.paint(1);
    const auto original = app.renderer.rgba();
    require(original.size() == static_cast<std::size_t>(fk::panel_width * fk::panel_height * 4),
            "RGBA size");
    app.apply({"international-full", "de-de", "midnight"});
    app.paint(2);
    require(app.renderer.rgba() != original, "theme and language affect rendered image");
    app.show_settings();
    app.paint(3);
    require(app.renderer.hit_key(app.view(), 100, 100) == nullptr, "settings prevent hidden key hits");
    auto has_japanese_presets = [&] {
        const auto controls = app.view().controls;
        return std::any_of(controls.begin(), controls.end(),
                           [](const fk::Control& control) { return control.id == "preset-ja-romaji"; });
    };
    auto click_control = [&](const std::string& id) {
        for (const auto& control : app.view().controls) {
            if (control.id == id) {
                const double x = control.bounds.x + control.bounds.width / 2;
                const double y = control.bounds.y + control.bounds.height / 2;
                app.down(0, x, y, 4);
                app.up(0, x, y);
                return;
            }
        }
        throw std::runtime_error("missing settings control " + id);
    };
    require(!has_japanese_presets(), "German settings hide Japanese presets");
    click_control("layout-next");
    require(has_japanese_presets(), "pending Japanese layout shows presets before Apply");
    click_control("layout-prev");
    require(!has_japanese_presets(), "leaving Japanese layout hides presets");
    for (std::size_t i = 0; i < fk::load_profiles({}, {}).languages.size() && !has_japanese_presets();
         ++i) {
        click_control("language-next");
    }
    click_control("preset-ja-romaji"); // Romaji uses the US layout.
    require(has_japanese_presets(), "Japanese language shows presets with a US layout");
    require(sink.events.empty(), "preview settings do not emit input");
    rejects([&] { app.apply({"missing", "en-us", "graphite"}); }, "invalid selection rejected");
    require(app.selection().theme == "midnight", "failed apply retains prior selection");
}
void dictation_tests() {
    require(fk::clean_transcript("[BLANK_AUDIO]").empty(), "blank audio stripped");
    require(fk::clean_transcript("  Hello world! [music]  ") == "Hello world!", "music tags stripped");
    require(fk::clean_transcript("(laughter) That's great! *applause*") == "That's great!",
            "laughter and applause stripped");
    require(fk::clean_transcript("It’s “working” — perfectly…") == "It's \"working\" - perfectly...",
            "curly quotes and dashes normalized to ascii");
    require(fk::clean_transcript("♪♪").empty(), "musical notes stripped");
    require(fk::clean_transcript("1234567890-=qwertyuiop").empty(),
            "keyboard smash hallucination stripped");

    const auto chunks = fk::split_utf8("日本語テスト", 2);
    require(chunks.size() == 3, "split_utf8 chunk count");
    require(chunks[0] == "日本", "split_utf8 chunk 0");
    require(chunks[1] == "語テ", "split_utf8 chunk 1");
    require(chunks[2] == "スト", "split_utf8 chunk 2");

    fk::Options options;
    options.mode = "preview";
    Capture sink;
    fk::App app(options, sink);
    bool found_dictate = false;
    for (const auto& c : app.view().controls) {
        if (c.id == "dictate" && c.icon == fk::Icon::Dictate) {
            found_dictate = true;
            break;
        }
    }
    require(found_dictate, "dictate button present in controls with Dictate icon");
}
void voice_command_and_punctuation_tests() {
    // Test pure voice editing commands
    require(fk::parse_voice_result("scratch that").command == fk::VoiceCommand::ScratchThat,
            "voice command: scratch that");
    require(fk::parse_voice_result("delete that").command == fk::VoiceCommand::ScratchThat,
            "voice command: delete that");
    require(fk::parse_voice_result("undo").command == fk::VoiceCommand::ScratchThat,
            "voice command: undo");
    require(fk::parse_voice_result("backspace").command == fk::VoiceCommand::Backspace,
            "voice command: backspace");
    require(fk::parse_voice_result("delete").command == fk::VoiceCommand::Backspace,
            "voice command: delete");
    require(fk::parse_voice_result("enter").command == fk::VoiceCommand::Enter, "voice command: enter");
    require(fk::parse_voice_result("newline").command == fk::VoiceCommand::Enter,
            "voice command: newline");
    require(fk::parse_voice_result("select all").command == fk::VoiceCommand::SelectAll,
            "voice command: select all");
    require(fk::parse_voice_result("clear text").command == fk::VoiceCommand::Clear,
            "voice command: clear text");
    require(fk::parse_voice_result("clear all").command == fk::VoiceCommand::Clear,
            "voice command: clear all");

    // Test spoken punctuation and capitalization
    const auto r1 = fk::parse_voice_result("hello world period");
    require(r1.command == fk::VoiceCommand::NoCommand, "not a command");
    require(r1.text == "Hello world.", "spoken period formatting");

    const auto r2 = fk::parse_voice_result("how are you question mark i am fine period");
    require(r2.text == "How are you? I am fine.", "question mark and sentence capitalization");

    const auto r3 = fk::parse_voice_result("look out exclamation point");
    require(r3.text == "Look out!", "exclamation point formatting");

    const auto r4 = fk::parse_voice_result("items comma one comma two comma three");
    require(r4.text == "Items, one, two, three", "comma formatting and spacing");

    const auto r5 = fk::parse_voice_result("line one new line line two");
    require(r5.text == "Line one\nLine two", "new line formatting and capitalization");
}

void clipboard_tests() {
    TemporaryDirectory directory;
    fk::Options options;
    options.config_dir = directory.path;
    options.mode = "vr";
    options.input = "uinput";
    options.target_language = "en-us";
    options.start_enabled = true;
    Capture sink;
    fk::App app(options, sink);
    app.set_interaction_active(true);

    app.push_clipboard("First clipboard snippet");
    app.push_clipboard("Second clipboard snippet");
    app.open_clipboard();

    require(app.view().clipboard, "clipboard tray is open");

    bool has_clip0 = false, has_clip1 = false, has_close = false, has_clear = false;
    for (const auto& c : app.view().controls) {
        if (c.id == "clip-0") {
            has_clip0 = true;
        }
        if (c.id == "clip-1") {
            has_clip1 = true;
        }
        if (c.id == "clip-close") {
            has_close = true;
        }
        if (c.id == "clip-clear") {
            has_clear = true;
        }
    }
    require(has_clip0, "clip-0 present in controls");
    require(has_clip1, "clip-1 present in controls");
    require(has_close, "clip-close present in controls");
    require(has_clear, "clip-clear present in controls");

    // Clicking close button closes clipboard tray
    for (const auto& control : app.view().controls) {
        if (control.id == "clip-close") {
            const double x = control.bounds.x + control.bounds.width / 2;
            const double y = control.bounds.y + control.bounds.height / 2;
            app.down(0, x, y, 1.0);
            app.up(0, x, y);
            break;
        }
    }
    require(!app.view().clipboard, "clip-close closed clipboard tray");

    // Quick tap on Paste emits Ctrl+V
    std::pair<double, double> paste_pos{-1, -1};
    const auto cv = app.view();
    for (int y = 96; y < fk::panel_height; y += 4) {
        for (int x = 0; x < fk::panel_width; x += 4) {
            const auto* k = app.renderer.hit_key(cv, x, y);
            if (k && k->action == "paste") {
                paste_pos = {static_cast<double>(x), static_cast<double>(y)};
                break;
            }
        }
        if (paste_pos.first >= 0) {
            break;
        }
    }
    require(paste_pos.first >= 0, "found paste key on panel");
    const auto [px, py] = paste_pos;
    sink.events.clear();
    app.down(0, px, py, 1.0);
    app.up(0, px, py);
    bool saw_ctrl = false, saw_v = false;
    for (const auto& [code, val] : sink.events) {
        if (code == KEY_LEFTCTRL && val == 1) {
            saw_ctrl = true;
        }
        if (code == KEY_V && val == 1) {
            saw_v = true;
        }
    }
    require(saw_ctrl && saw_v, "quick tap on Paste emits Ctrl+V");
    require(!app.view().clipboard, "quick tap on Paste does not open clipboard");

    // Long press on Paste opens clipboard tray
    sink.events.clear();
    app.down(0, px, py, 2.0);
    app.tick(2.4); // > 0.35s hold
    require(app.view().clipboard, "holding Paste for >= 0.35s opens clipboard tray");
    app.up(0, px, py);
    require(sink.events.empty(), "releasing held Paste does not emit Ctrl+V");
}
} // namespace
int main() {
    try {
        const auto defaults = fk::load_profiles({}, {});
        require(defaults.errors.empty(), "bundled profiles valid");
        require(defaults.layouts.at("en-us-full").keys.size() == 106, "106 baseline keys");
        state_tests(defaults.layouts.at("en-us-full"));
        profile_tests(defaults);
        app_tests();
        dictation_tests();
        voice_command_and_punctuation_tests();
        clipboard_tests();
        std::cout << "Key sequences, cancellation, repeat, profiles, persistence, language legends, "
                     "dictation, voice commands, clipboard and rendering passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
