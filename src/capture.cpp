#include "framekeyboard/capture.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <signal.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace framekeyboard {

VideoCapture::VideoCapture() = default;

VideoCapture::~VideoCapture() {
    cancel();
}

bool VideoCapture::ffmpeg_available() {
    return ::access("/usr/bin/ffmpeg", X_OK) == 0 || (std::system("which ffmpeg >/dev/null 2>&1") == 0);
}

std::string VideoCapture::detect_camera_device() {
    // 1. Look for SteamVR loopback camera in sysfs
    std::error_code ec;
    const fs::path v4l_dir = "/sys/class/video4linux";
    if (fs::exists(v4l_dir, ec)) {
        for (const auto& entry : fs::directory_iterator(v4l_dir, ec)) {
            const auto name_path = entry.path() / "name";
            if (fs::is_regular_file(name_path, ec)) {
                std::ifstream name_file(name_path);
                std::string line;
                if (std::getline(name_file, line) && line.find("SteamVR") != std::string::npos) {
                    const std::string dev_node = "/dev/" + entry.path().filename().string();
                    if (::access(dev_node.c_str(), R_OK) == 0) {
                        return dev_node;
                    }
                }
            }
        }
    }

    // 2. Fall back to standard SteamVR loopback device /dev/video99
    if (::access("/dev/video99", R_OK) == 0) {
        return "/dev/video99";
    }

    // 3. Fall back to /dev/video0 if present
    if (::access("/dev/video0", R_OK) == 0) {
        return "/dev/video0";
    }

    return "/dev/video99";
}

fs::path VideoCapture::resolve_save_dir(const std::string& path_str) {
    fs::path p;
    if (path_str.starts_with("~/") || path_str == "~") {
        const char* home = std::getenv("HOME");
        const std::string home_str = home ? home : "/home/steamos";
        if (path_str == "~") {
            p = home_str;
        } else {
            p = fs::path(home_str) / path_str.substr(2);
        }
    } else if (path_str.empty()) {
        const char* home = std::getenv("HOME");
        p = fs::path(home ? home : "/home/steamos") / "Videos" / "Recordings";
    } else {
        p = path_str;
    }

    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

bool VideoCapture::start(const std::string& save_dir, const std::string& quality, bool record_game_audio,
                         bool record_mic, const std::string& device, std::string* error) {
    if (active()) {
        if (error) {
            *error = "Recording is already in progress";
        }
        return false;
    }

    if (!ffmpeg_available()) {
        last_error_ = "ffmpeg not found on system";
        if (error) {
            *error = last_error_;
        }
        return false;
    }

    std::string video_dev = device;
    if (video_dev.empty() || video_dev == "auto") {
        video_dev = detect_camera_device();
    }

    if (::access(video_dev.c_str(), R_OK) != 0) {
        last_error_ = "Camera device not available (" + video_dev + ")";
        if (error) {
            *error = last_error_;
        }
        return false;
    }

    const fs::path dir = resolve_save_dir(save_dir);

    // Generate timestamped filename: Recording_YYYY-MM-DD_HH-MM-SS.mp4
    const auto now_c = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm_buf{};
    localtime_r(&now_c, &tm_buf);
    char name_buf[64];
    std::strftime(name_buf, sizeof(name_buf), "Recording_%Y-%m-%d_%H-%M-%S.mp4", &tm_buf);
    const fs::path out_file = dir / name_buf;

    // Open PulseAudio inputs BEFORE the V4L2 loopback device so that PulseAudio
    // connection/probing latency (~250-500ms) does not stall or overflow V4L2's
    // 2-frame ring buffer before FFmpeg's demux threads start.
    std::vector<std::string> args = {"ffmpeg", "-y"};

    if (record_game_audio && record_mic) {
        args.insert(args.end(), {"-thread_queue_size", "1024", "-f", "pulse", "-i", "default"});
        args.insert(args.end(),
                    {"-thread_queue_size", "1024", "-f", "pulse", "-i", "@DEFAULT_MONITOR@"});
    } else if (record_game_audio) {
        args.insert(args.end(),
                    {"-thread_queue_size", "1024", "-f", "pulse", "-i", "@DEFAULT_MONITOR@"});
    } else if (record_mic) {
        args.insert(args.end(), {"-thread_queue_size", "1024", "-f", "pulse", "-i", "default"});
    }

    args.insert(args.end(), {"-thread_queue_size", "512", "-f", "v4l2", "-input_format", "rgb24",
                             "-video_size", "1920x1080", "-i", video_dev});

    if (record_game_audio && record_mic) {
        args.insert(args.end(),
                    {"-filter_complex",
                     "[0:a]aresample=async=1:first_pts=0[a0];[1:a]aresample=async=1:first_pts=0[a1];"
                     "[a0][a1]amix=inputs=2:duration=first:dropout_transition=0:normalize=0[aout]",
                     "-map", "2:v:0", "-map", "[aout]"});
    } else if (record_game_audio || record_mic) {
        args.insert(args.end(),
                    {"-map", "1:v:0", "-map", "0:a:0", "-af", "aresample=async=1:first_pts=0"});
    }

    // Video encoding settings
    args.insert(args.end(), {"-c:v", "libx264"});
    if (quality == "720p Performance") {
        args.insert(args.end(), {"-vf", "scale=1280:720", "-preset", "ultrafast", "-crf", "24",
                                 "-pix_fmt", "yuv420p"});
    } else if (quality == "1080p Ultra") {
        args.insert(args.end(), {"-preset", "veryfast", "-crf", "18", "-pix_fmt", "yuv420p"});
    } else if (quality == "1080p Standard") {
        args.insert(args.end(), {"-preset", "ultrafast", "-crf", "24", "-pix_fmt", "yuv420p"});
    } else {
        // Default: 1080p High
        args.insert(args.end(), {"-preset", "ultrafast", "-crf", "20", "-pix_fmt", "yuv420p"});
    }

    if (record_game_audio || record_mic) {
        args.insert(args.end(), {"-c:a", "aac", "-b:a", "128k"});
    } else {
        args.push_back("-an");
    }

    args.push_back(out_file.string());

    // Build raw argv array
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& s : args) {
        argv.push_back(s.data());
    }
    argv.push_back(nullptr);

    pid_t child = ::fork();
    if (child < 0) {
        last_error_ = "Failed to fork ffmpeg process";
        if (error) {
            *error = last_error_;
        }
        return false;
    }

    if (child == 0) {
        // Child process
        ::setsid();

        int devnull = ::open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            ::close(devnull);
        }

        int logfd = ::open("/tmp/framekeyboard_capture.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (logfd >= 0) {
            ::dup2(logfd, STDOUT_FILENO);
            ::dup2(logfd, STDERR_FILENO);
            ::close(logfd);
        }

        ::execvp("ffmpeg", argv.data());
        ::_exit(127);
    }

    // Parent process
    pid_ = child;
    state_ = State::Recording;
    current_file_ = out_file.string();
    start_time_ = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count()) /
                  1000.0;
    last_error_.clear();

    return true;
}

bool VideoCapture::stop(std::string* saved_path, std::string* error) {
    if (!active() || pid_ <= 0) {
        if (error) {
            *error = "No active recording to stop";
        }
        return false;
    }

    // Send SIGINT to ffmpeg so it cleanly flushes and finalizes MP4 moov atom
    ::kill(pid_, SIGINT);

    // Wait up to 3 seconds for ffmpeg to exit gracefully
    bool exited = false;
    int status = 0;
    for (int i = 0; i < 60; ++i) {
        pid_t res = ::waitpid(pid_, &status, WNOHANG);
        if (res == pid_) {
            exited = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (!exited) {
        // Force terminate if graceful shutdown timed out
        ::kill(pid_, SIGKILL);
        ::waitpid(pid_, &status, 0);
    }

    pid_ = -1;
    state_ = State::Idle;

    // Check if the recorded file was successfully created
    std::error_code ec;
    if (fs::exists(current_file_, ec) && fs::file_size(current_file_, ec) > 0) {
        if (saved_path) {
            *saved_path = current_file_;
        }
        return true;
    }

    last_error_ = "Recording file was empty or missing";
    if (error) {
        *error = last_error_;
    }
    return false;
}

void VideoCapture::close_child(bool force_kill) {
    if (pid_ > 0) {
        if (force_kill) {
            ::kill(pid_, SIGKILL);
        } else {
            ::kill(pid_, SIGINT);
        }
        int status = 0;
        ::waitpid(pid_, &status, 0);
        pid_ = -1;
    }
    state_ = State::Idle;
}

void VideoCapture::cancel() {
    if (active()) {
        std::string saved;
        stop(&saved, nullptr);
    }
}

void VideoCapture::poll(double /*now*/) {
    if (active() && pid_ > 0) {
        int status = 0;
        pid_t res = ::waitpid(pid_, &status, WNOHANG);
        if (res == pid_) {
            // Child exited prematurely
            pid_ = -1;
            state_ = State::Idle;
            last_error_ = "Recording process exited unexpectedly";
        }
    }
}

double VideoCapture::duration(double now) const {
    if (!active()) {
        return 0.0;
    }
    return std::max(0.0, now - start_time_);
}

} // namespace framekeyboard
