#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace framekeyboard {
namespace fs = std::filesystem;

class SuggestionEngine {
  public:
    SuggestionEngine() = default;
    explicit SuggestionEngine(const fs::path& data_dir, const fs::path& config_dir = {});

    void load(const fs::path& data_dir, const fs::path& config_dir = {});

    // Returns up to max_count closest suggestions for prefix.
    // Handles capitalization, common contractions, and typo tolerance.
    std::vector<std::string> suggest(const std::string& prefix, std::size_t max_count = 4) const;

    bool empty() const { return words_.empty(); }
    std::size_t size() const { return words_.size(); }

  private:
    std::vector<std::string> words_;
    std::vector<std::string> words_lower_;
    std::vector<std::string> words_no_apos_;

    void load_file(const fs::path& path);
    void load_fallback();
};

} // namespace framekeyboard
