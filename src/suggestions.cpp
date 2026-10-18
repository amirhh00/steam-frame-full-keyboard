#include "framekeyboard/suggestions.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <unistd.h>

namespace framekeyboard {
namespace {
std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

std::string remove_apostrophes(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c != '\'') {
            out.push_back(c);
        }
    }
    return out;
}

fs::path executable_dir() {
    char buffer[1024]{};
    const auto len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len > 0) {
        return fs::path(std::string(buffer, static_cast<std::size_t>(len))).parent_path();
    }
    return {};
}
} // namespace

SuggestionEngine::SuggestionEngine(const fs::path& data_dir, const fs::path& config_dir) {
    load(data_dir, config_dir);
}

void SuggestionEngine::load(const fs::path& data_dir, const fs::path& config_dir) {
    words_.clear();
    words_lower_.clear();
    words_no_apos_.clear();

    const auto exe = executable_dir();
    const std::vector<fs::path> candidates = {config_dir / "words/en.txt",
                                              data_dir / "words/en.txt",
                                              exe / "words/en.txt",
                                              exe.parent_path() / "share/framekeyboard/words/en.txt",
                                              "/usr/share/framekeyboard/words/en.txt",
                                              "/usr/local/share/framekeyboard/words/en.txt"};

    for (const auto& path : candidates) {
        std::error_code ec;
        if (!path.empty() && fs::is_regular_file(path, ec)) {
            load_file(path);
            if (!words_.empty()) {
                return;
            }
        }
    }

    load_fallback();
}

void SuggestionEngine::load_file(const fs::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return;
    }

    std::set<std::string> seen;
    std::string line;
    while (std::getline(file, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        std::size_t start = 0;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) {
            ++start;
        }
        if (start > 0) {
            line = line.substr(start);
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }

        std::string lower = to_lower(line);
        std::string no_apos = remove_apostrophes(lower);
        if (seen.insert(no_apos).second) {
            words_.push_back(line);
            words_lower_.push_back(std::move(lower));
            words_no_apos_.push_back(std::move(no_apos));
        }
    }
}

void SuggestionEngine::load_fallback() {
    static const char* fallback_words[] = {"the",
                                           "of",
                                           "and",
                                           "to",
                                           "a",
                                           "in",
                                           "for",
                                           "is",
                                           "on",
                                           "that",
                                           "by",
                                           "this",
                                           "with",
                                           "i",
                                           "you",
                                           "it",
                                           "not",
                                           "or",
                                           "be",
                                           "are",
                                           "from",
                                           "at",
                                           "as",
                                           "your",
                                           "all",
                                           "have",
                                           "new",
                                           "more",
                                           "an",
                                           "was",
                                           "we",
                                           "will",
                                           "home",
                                           "can",
                                           "us",
                                           "about",
                                           "if",
                                           "page",
                                           "my",
                                           "has",
                                           "search",
                                           "free",
                                           "but",
                                           "our",
                                           "one",
                                           "other",
                                           "do",
                                           "no",
                                           "information",
                                           "time",
                                           "they",
                                           "site",
                                           "he",
                                           "up",
                                           "may",
                                           "what",
                                           "which",
                                           "their",
                                           "news",
                                           "out",
                                           "use",
                                           "any",
                                           "there",
                                           "see",
                                           "only",
                                           "so",
                                           "his",
                                           "when",
                                           "contact",
                                           "here",
                                           "business",
                                           "who",
                                           "web",
                                           "also",
                                           "now",
                                           "help",
                                           "get",
                                           "pm",
                                           "view",
                                           "online",
                                           "first",
                                           "am",
                                           "been",
                                           "would",
                                           "how",
                                           "were",
                                           "me",
                                           "services",
                                           "some",
                                           "these",
                                           "click",
                                           "its",
                                           "like",
                                           "service",
                                           "than",
                                           "find",
                                           "price",
                                           "date",
                                           "back",
                                           "top",
                                           "people",
                                           "had",
                                           "list",
                                           "name",
                                           "just",
                                           "over",
                                           "state",
                                           "year",
                                           "day",
                                           "into",
                                           "email",
                                           "two",
                                           "health",
                                           "world",
                                           "re",
                                           "next",
                                           "used",
                                           "go",
                                           "b",
                                           "work",
                                           "last",
                                           "most",
                                           "products",
                                           "music",
                                           "buy",
                                           "data",
                                           "make",
                                           "them",
                                           "should",
                                           "product",
                                           "system",
                                           "post",
                                           "her",
                                           "city",
                                           "policy",
                                           "number",
                                           "such",
                                           "please",
                                           "available",
                                           "copyright",
                                           "support",
                                           "message",
                                           "after",
                                           "best",
                                           "software",
                                           "then",
                                           "good",
                                           "video",
                                           "well",
                                           "where",
                                           "info",
                                           "rights",
                                           "public",
                                           "books",
                                           "high",
                                           "school",
                                           "through",
                                           "m",
                                           "each",
                                           "links",
                                           "she",
                                           "review",
                                           "years",
                                           "order",
                                           "very",
                                           "privacy",
                                           "book",
                                           "items",
                                           "company",
                                           "read",
                                           "group",
                                           "sex",
                                           "need",
                                           "many",
                                           "user",
                                           "said",
                                           "de",
                                           "does",
                                           "set",
                                           "under",
                                           "general",
                                           "research",
                                           "university",
                                           "january",
                                           "mail",
                                           "full",
                                           "map",
                                           "reviews",
                                           "program",
                                           "life",
                                           "know",
                                           "games",
                                           "way",
                                           "days",
                                           "management",
                                           "part",
                                           "could",
                                           "great",
                                           "united",
                                           "hotel",
                                           "real",
                                           "item",
                                           "international",
                                           "center",
                                           "ebay",
                                           "must",
                                           "store",
                                           "travel",
                                           "comments",
                                           "made",
                                           "development",
                                           "report",
                                           "off",
                                           "member",
                                           "details",
                                           "line",
                                           "terms",
                                           "before",
                                           "hotels",
                                           "did",
                                           "send",
                                           "right",
                                           "type",
                                           "because",
                                           "local",
                                           "those",
                                           "using",
                                           "results",
                                           "office",
                                           "education",
                                           "national",
                                           "car",
                                           "design",
                                           "take",
                                           "posted",
                                           "internet",
                                           "address",
                                           "community",
                                           "within",
                                           "states",
                                           "area",
                                           "want",
                                           "phone",
                                           "dvd",
                                           "shipping",
                                           "reserved",
                                           "subject",
                                           "between",
                                           "forum",
                                           "family",
                                           "l",
                                           "long",
                                           "based",
                                           "w",
                                           "code",
                                           "show",
                                           "o",
                                           "even",
                                           "black",
                                           "check",
                                           "special",
                                           "prices",
                                           "website",
                                           "index",
                                           "being",
                                           "women",
                                           "much",
                                           "sign",
                                           "file",
                                           "link",
                                           "open",
                                           "today",
                                           "technology",
                                           "south",
                                           "case",
                                           "project",
                                           "same",
                                           "pages",
                                           "uk",
                                           "version",
                                           "section",
                                           "own",
                                           "found",
                                           "sports",
                                           "house",
                                           "related",
                                           "security",
                                           "both",
                                           "county",
                                           "american",
                                           "photo",
                                           "game",
                                           "members",
                                           "power",
                                           "while",
                                           "care",
                                           "network",
                                           "down",
                                           "computer",
                                           "systems",
                                           "three",
                                           "total",
                                           "place",
                                           "end",
                                           "following",
                                           "download",
                                           "h",
                                           "him",
                                           "without",
                                           "per",
                                           "access",
                                           "think",
                                           "north",
                                           "resources",
                                           "current",
                                           "posts",
                                           "big",
                                           "media",
                                           "law",
                                           "control",
                                           "water",
                                           "history",
                                           "pictures",
                                           "size",
                                           "art",
                                           "personal",
                                           "since",
                                           "including",
                                           "guide",
                                           "shop",
                                           "directory",
                                           "board",
                                           "location",
                                           "change",
                                           "white",
                                           "text",
                                           "small",
                                           "rating",
                                           "rate",
                                           "government",
                                           "children",
                                           "during",
                                           "usa",
                                           "return",
                                           "students",
                                           "v",
                                           "shopping",
                                           "account",
                                           "times",
                                           "sites",
                                           "level",
                                           "digital",
                                           "profile",
                                           "previous",
                                           "form",
                                           "events",
                                           "love",
                                           "old",
                                           "john",
                                           "main",
                                           "call",
                                           "hours",
                                           "image",
                                           "department",
                                           "title",
                                           "description",
                                           "non",
                                           "k",
                                           "y",
                                           "insurance",
                                           "another",
                                           "why",
                                           "shall",
                                           "property",
                                           "class",
                                           "cd",
                                           "still",
                                           "money",
                                           "quality",
                                           "every",
                                           "listing",
                                           "content",
                                           "country",
                                           "private",
                                           "little",
                                           "visit",
                                           "save",
                                           "(c)",
                                           "tools",
                                           "low",
                                           "reply",
                                           "customer",
                                           "december",
                                           "compare",
                                           "movies",
                                           "include",
                                           "college",
                                           "value",
                                           "article",
                                           "york",
                                           "man",
                                           "card",
                                           "jobs",
                                           "provide",
                                           "j",
                                           "food",
                                           "source",
                                           "author",
                                           "different",
                                           "press",
                                           "learn",
                                           "sale",
                                           "around",
                                           "print",
                                           "course",
                                           "job",
                                           "canada",
                                           "process",
                                           "teen",
                                           "room",
                                           "stock",
                                           "training",
                                           "too",
                                           "credit",
                                           "point",
                                           "join",
                                           "science",
                                           "men",
                                           "categories",
                                           "advanced",
                                           "west",
                                           "sales",
                                           "look",
                                           "english",
                                           "left",
                                           "team",
                                           "estate",
                                           "box",
                                           "conditions",
                                           "select",
                                           "windows",
                                           "photos",
                                           "gay",
                                           "thread",
                                           "week",
                                           "category",
                                           "note",
                                           "live",
                                           "large",
                                           "gallery",
                                           "table",
                                           "register",
                                           "however",
                                           "june",
                                           "october",
                                           "november",
                                           "market",
                                           "library",
                                           "really",
                                           "action",
                                           "start",
                                           "series",
                                           "model",
                                           "features",
                                           "air",
                                           "industry",
                                           "plan",
                                           "human",
                                           "provided",
                                           "tv",
                                           "yes",
                                           "clean",
                                           "steam",
                                           "vr",
                                           "keyboard",
                                           "controller"};

    std::set<std::string> seen;
    for (const char* w : fallback_words) {
        std::string lower = to_lower(w);
        std::string no_apos = remove_apostrophes(lower);
        if (seen.insert(no_apos).second) {
            words_.push_back(w);
            words_lower_.push_back(std::move(lower));
            words_no_apos_.push_back(std::move(no_apos));
        }
    }
}

std::vector<std::string> SuggestionEngine::suggest(const std::string& prefix,
                                                   std::size_t max_count) const {
    if (prefix.empty() || max_count == 0 || words_.empty()) {
        return {};
    }

    const bool all_upper =
        prefix.size() > 1 && std::all_of(prefix.begin(), prefix.end(), [](unsigned char c) {
            return !std::isalpha(c) || std::isupper(c);
        });
    const bool first_upper = std::isupper(static_cast<unsigned char>(prefix[0]));

    const std::string lower_prefix = to_lower(prefix);
    const std::string clean_prefix = remove_apostrophes(lower_prefix);

    static const std::map<std::string, std::string> contractions = {
        {"im", "I'm"},           {"dont", "don't"},       {"cant", "can't"},
        {"wont", "won't"},       {"didnt", "didn't"},     {"doesnt", "doesn't"},
        {"isnt", "isn't"},       {"arent", "aren't"},     {"wasnt", "wasn't"},
        {"werent", "weren't"},   {"youre", "you're"},     {"theyre", "they're"},
        {"weve", "we've"},       {"youve", "you've"},     {"theyve", "they've"},
        {"youll", "you'll"},     {"theyll", "they'll"},   {"thats", "that's"},
        {"whats", "what's"},     {"theres", "there's"},   {"heres", "here's"},
        {"hows", "how's"},       {"wheres", "where's"},   {"lets", "let's"},
        {"couldnt", "couldn't"}, {"wouldnt", "wouldn't"}, {"shouldnt", "shouldn't"},
        {"ive", "I've"},         {"ill", "I'll"},         {"id", "I'd"}};

    std::vector<std::string> results;
    std::vector<std::string> results_clean;

    auto add_candidate = [&](const std::string& word) {
        std::string clean = remove_apostrophes(to_lower(word));
        for (const auto& r : results_clean) {
            if (r == clean) {
                return false;
            }
        }
        results.push_back(word);
        results_clean.push_back(clean);
        return true;
    };

    if (auto it = contractions.find(clean_prefix); it != contractions.end()) {
        add_candidate(it->second);
    }

    for (std::size_t i = 0; i < words_.size() && results.size() < max_count; ++i) {
        if (words_lower_[i].starts_with(lower_prefix) || words_no_apos_[i].starts_with(clean_prefix)) {
            add_candidate(words_[i]);
        }
    }

    if (results.size() < max_count && clean_prefix.size() >= 3) {
        const std::size_t limit = std::min<std::size_t>(words_.size(), 3000);
        for (std::size_t i = 0; i < limit && results.size() < max_count; ++i) {
            const auto& candidate = words_no_apos_[i];
            const auto c_len = candidate.size();
            const auto p_len = clean_prefix.size();
            if (std::abs(static_cast<int>(c_len) - static_cast<int>(p_len)) > 1) {
                continue;
            }
            int diff = 0;
            if (c_len == p_len) {
                for (std::size_t j = 0; j < p_len; ++j) {
                    if (candidate[j] != clean_prefix[j]) {
                        if (++diff > 1) {
                            break;
                        }
                    }
                }
            } else if (c_len == p_len + 1) {
                std::size_t pi = 0, ci = 0;
                while (pi < p_len && ci < c_len) {
                    if (clean_prefix[pi] == candidate[ci]) {
                        ++pi;
                    }
                    ++ci;
                }
                diff = (pi == p_len) ? 1 : 2;
            } else if (c_len + 1 == p_len) {
                std::size_t pi = 0, ci = 0;
                while (pi < p_len && ci < c_len) {
                    if (clean_prefix[pi] == candidate[ci]) {
                        ++ci;
                    }
                    ++pi;
                }
                diff = (ci == c_len) ? 1 : 2;
            }
            if (diff == 1) {
                add_candidate(words_[i]);
            }
        }
    }

    std::vector<std::string> formatted;
    formatted.reserve(results.size());
    for (auto word : results) {
        if (all_upper) {
            for (auto& c : word) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
        } else if (first_upper && !word.empty()) {
            word[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(word[0])));
        }
        formatted.push_back(std::move(word));
    }
    return formatted;
}

} // namespace framekeyboard
