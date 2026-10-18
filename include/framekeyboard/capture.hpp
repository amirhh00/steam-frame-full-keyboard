#pragma once

#include <filesystem>
#include <string>
#include <sys/types.h>
#include <vector>

namespace framekeyboard {
namespace fs = std::filesystem;

class VideoCapture {
  public:
    enum class State { Idle, Recording };

    VideoCapture();
    ~VideoCapture();
    VideoCapture(const VideoCapture&) = delete;
    VideoCapture& operator=(const VideoCapture&) = delete;

    // Checks if ffmpeg is available on the system.
    static bool ffmpeg_available();

    // Finds the SteamVR v4l2loopback camera device, falling back to /dev/video99 or first v4l2 device.
    static std::string detect_camera_device();

    // Expands paths starting with ~/ to the user's home directory.
    static fs::path resolve_save_dir(const std::string& path_str);

    // Starts video capture using ffmpeg in a separate child process.
    // Video quality: "1080p High", "1080p Standard", "720p Performance", "1080p Ultra"
    bool start(const std::string& save_dir, const std::string& quality, bool record_game_audio,
               bool record_mic, const std::string& device, std::string* error = nullptr);

    // Stops recording cleanly by sending SIGINT to ffmpeg so the MP4 container is finalized.
    bool stop(std::string* saved_path = nullptr, std::string* error = nullptr);

    // Force stops / cancels recording on application exit.
    void cancel();

    // Polls child process to detect if ffmpeg exited unexpectedly.
    void poll(double now);

    State state() const { return state_; }
    bool active() const { return state_ == State::Recording; }
    double duration(double now) const;
    const std::string& current_file() const { return current_file_; }
    const std::string& last_error() const { return last_error_; }

  private:
    void close_child(bool force_kill);

    State state_{State::Idle};
    pid_t pid_{-1};
    std::string current_file_;
    double start_time_{0.0};
    std::string last_error_;
};

} // namespace framekeyboard
