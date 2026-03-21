#include "termind/utils.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#include <sys/ioctl.h>
#include <unistd.h>


namespace termind {
namespace utils {

// ── 字符串工具 ────────────────────────────────────────────────────────────

std::string Trim(const std::string& s) {
    return TrimLeft(TrimRight(s));
}

std::string TrimLeft(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\n\r\f\v");
    return (start == std::string::npos) ? "" : s.substr(start);
}

std::string TrimRight(const std::string& s) {
    size_t end = s.find_last_not_of(" \t\n\r\f\v");
    return (end == std::string::npos) ? "" : s.substr(0, end + 1);
}

std::vector<std::string> Split(const std::string& s, char delim) {
    std::vector<std::string> result;
    std::stringstream ss(s);
    std::string token;
    while (std::getline(ss, token, delim))
        result.push_back(token);
    return result;
}

std::string Join(const std::vector<std::string>& parts,
                  const std::string& sep) {
    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) result += sep;
        result += parts[i];
    }
    return result;
}

bool StartsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() &&
           s.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string ToLower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

std::string EscapeShellArg(const std::string& s) {
    std::string result = "'";
    for (char c : s) {
        if (c == '\'') result += "'\\''";
        else result += c;
    }
    result += "'";
    return result;
}

// UTF-8 安全截断：确保截断位不落在多字节字符中间
std::string Utf8SafeTruncate(const std::string& s, size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    size_t pos = max_bytes;
    // 向前找到合法 UTF-8 起始字节（0x00-0x7F 或 0xC0+）
    while (pos > 0 && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80)
        --pos;
    return s.substr(0, pos);
}

// 将字符串中所有非法 UTF-8 字节替换为 '?'
// 保证结果可安全传入 nlohmann/json（避免 type_error.316）
std::string SanitizeUtf8(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        auto c = static_cast<unsigned char>(s[i]);
        int seq = 0;
        if (c < 0x80) {
            seq = 1;                          // ASCII
        } else if (c < 0xC0) {
            out += '?'; ++i; continue;        // 意外的延续字节
        } else if (c < 0xE0) {
            seq = 2;
        } else if (c < 0xF0) {
            seq = 3;
        } else if (c < 0xF8) {
            seq = 4;
        } else {
            out += '?'; ++i; continue;        // 非法起始字节
        }

        // 验证后续延续字节
        if (i + static_cast<size_t>(seq) > s.size()) {
            out += '?'; ++i; continue;
        }
        bool valid = true;
        for (int j = 1; j < seq; ++j) {
            if ((static_cast<unsigned char>(s[i + j]) & 0xC0) != 0x80) {
                valid = false;
                break;
            }
        }
        if (!valid) {
            out += '?'; ++i;
        } else {
            out.append(s, i, static_cast<size_t>(seq));
            i += static_cast<size_t>(seq);
        }
    }
    return out;
}

// ── 文件工具 ──────────────────────────────────────────────────────────────

std::optional<std::string> ReadFile(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool WriteFile(const std::filesystem::path& path, const std::string& content) {
    auto parent = path.parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent)) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) return false;
    }
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    f << content;
    return f.good();
}

bool FileExists(const std::filesystem::path& path) {
    return std::filesystem::exists(path);
}

std::string GetRelativePath(const std::filesystem::path& path,
                             const std::filesystem::path& base) {
    try {
        return std::filesystem::relative(path, base).string();
    } catch (...) {
        return path.string();
    }
}

std::string FormatFileSize(std::uintmax_t bytes) {
    std::ostringstream ss;
    if (bytes < 1024) {
        ss << bytes << "B";
    } else if (bytes < 1024 * 1024) {
        ss << std::fixed << std::setprecision(1) << (bytes / 1024.0) << "KB";
    } else {
        ss << std::fixed << std::setprecision(1)
           << (bytes / (1024.0 * 1024.0)) << "MB";
    }
    return ss.str();
}

// ── 终端工具 ──────────────────────────────────────────────────────────────

int GetTerminalWidth() {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0)
        return static_cast<int>(w.ws_col);
    return 80;
}

int GetTerminalHeight() {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 0)
        return static_cast<int>(w.ws_row);
    return 24;
}

bool IsAtty() {
    return isatty(STDOUT_FILENO) != 0;
}

std::string Repeat(const std::string& s, int n) {
    std::string r;
    for (int i = 0; i < n; ++i) r += s;
    return r;
}

void PrintHorizontalRule(const std::string& title) {
    int width = GetTerminalWidth();
    std::cout << color::kDim;
    if (title.empty()) {
        std::cout << Repeat("─", width);
    } else {
        int title_len = static_cast<int>(title.size()) + 2;
        int left = (width - title_len) / 2;
        int right = width - title_len - left;
        std::cout << Repeat("─", left) << " " << title << " "
                  << Repeat("─", right);
    }
    std::cout << color::kReset << "\n";
}

void PrintInfo(const std::string& msg) {
    std::cout << color::kCyan << "ℹ " << color::kReset << msg << "\n";
}

void PrintWarning(const std::string& msg) {
    std::cout << color::kYellow << "⚠ " << color::kReset << msg << "\n";
}

void PrintError(const std::string& msg) {
    std::cerr << color::kRed << "✗ " << color::kReset << msg << "\n";
}

void PrintSuccess(const std::string& msg) {
    std::cout << color::kGreen << "✓ " << color::kReset << msg << "\n";
}

void PrintDim(const std::string& msg) {
    std::cout << color::kDim << msg << color::kReset << "\n";
}

void PrintColoredDiff(const std::string& diff_text) {
    for (const auto& line : Split(diff_text, '\n')) {
        if (StartsWith(line, "+++") || StartsWith(line, "---")) {
            std::cout << color::kBold << line << color::kReset << "\n";
        } else if (StartsWith(line, "+")) {
            std::cout << color::kGreen << line << color::kReset << "\n";
        } else if (StartsWith(line, "-")) {
            std::cout << color::kRed << line << color::kReset << "\n";
        } else if (StartsWith(line, "@")) {
            std::cout << color::kCyan << line << color::kReset << "\n";
        } else {
            std::cout << color::kDim << line << color::kReset << "\n";
        }
    }
}

// ── 差异比较 ──────────────────────────────────────────────────────────────

std::string ComputeDiff(const std::string& old_content,
                         const std::string& new_content,
                         const std::string& filename) {
    char old_tmp[] = "/tmp/termind_old_XXXXXX";
    char new_tmp[] = "/tmp/termind_new_XXXXXX";

    int old_fd = mkstemp(old_tmp);
    if (old_fd < 0) return "(diff 不可用)";

    struct TmpGuard {
        char* path;
        int   fd;
        ~TmpGuard() { if (fd >= 0) close(fd); unlink(path); }
    } old_g{old_tmp, old_fd}, new_g{new_tmp, -1};

    int new_fd = mkstemp(new_tmp);
    if (new_fd < 0) return "(diff 不可用)";
    new_g.fd = new_fd;

    auto write_all = [](int fd, const std::string& data) -> bool {
        size_t written = 0;
        while (written < data.size()) {
            ssize_t n = write(fd, data.data() + written, data.size() - written);
            if (n < 0) return false;
            written += static_cast<size_t>(n);
        }
        return true;
    };

    if (!write_all(old_fd, old_content) || !write_all(new_fd, new_content))
        return "(diff 不可用)";

    close(old_fd); old_g.fd = -1;
    close(new_fd); new_g.fd = -1;

    std::string label_a = "a/" + filename;
    std::string label_b = "b/" + filename;
    std::string cmd = "diff -u --label=" + EscapeShellArg(label_a) +
                      " --label=" + EscapeShellArg(label_b) +
                      " " + old_tmp + " " + new_tmp + " 2>/dev/null";

    FILE* pipe = popen(cmd.c_str(), "r");
    std::string result;
    if (pipe) {
        char buf[512];
        while (fgets(buf, sizeof(buf), pipe)) result += buf;
        pclose(pipe);
    }
    return result.empty() ? "(无变化)" : result;
}

// ── 用户交互 ──────────────────────────────────────────────────────────────

char AskYesNoEdit(const std::string& prompt) {
    std::cout << color::kBrightYellow << "\n? " << color::kReset
              << prompt << " "
              << color::kDim << "[Y]es / [n]o / [e]dit" << color::kReset
              << "  ";
    std::cout.flush();
    std::string line;
    if (!std::getline(std::cin, line)) return 'y';
    line = ToLower(Trim(line));
    if (line.empty()) return 'y';
    return line[0];
}

bool AskYesNo(const std::string& prompt, bool default_yes) {
    std::string choices = default_yes ? "[Y/n]" : "[y/N]";
    std::cout << color::kBrightYellow << "\n? " << color::kReset
              << prompt << " "
              << color::kDim << choices << color::kReset << "  ";
    std::cout.flush();
    std::string line;
    if (!std::getline(std::cin, line)) return default_yes;
    line = ToLower(Trim(line));
    if (line.empty()) return default_yes;
    return line[0] == 'y';
}

std::string AskString(const std::string& prompt,
                       const std::string& default_val) {
    std::cout << color::kBrightYellow << "? " << color::kReset << prompt;
    if (!default_val.empty())
        std::cout << color::kDim << " [" << default_val << "]" << color::kReset;
    std::cout << "  ";
    std::cout.flush();
    std::string line;
    std::getline(std::cin, line);
    line = Trim(line);
    return line.empty() ? default_val : line;
}

// ── 环境变量 ──────────────────────────────────────────────────────────────

std::string GetEnv(const std::string& name, const std::string& default_val) {
    const char* val = ::getenv(name.c_str());
    return val ? std::string(val) : default_val;
}

std::string ExpandHome(const std::string& path) {
    if (path.empty() || path[0] != '~') return path;
    return GetEnv("HOME", "/tmp") + path.substr(1);
}

// ── Markdown 剥离 ──────────────────────────────────────────────────────────

std::string StripMarkdownLine(const std::string& raw, bool& in_code_block) {
    // 代码围栏 ``` (可有语言标注)
    if (raw.size() >= 3 && raw.substr(0, 3) == "```") {
        in_code_block = !in_code_block;
        return "";      // 不输出围栏行
    }
    // 代码块内容原样输出（保留缩进）
    if (in_code_block) return raw;

    std::string line = raw;

    // 水平线
    if (line == "---" || line == "***" || line == "___" ||
        line == "----" || line == "====" ) {
        return std::string(40, '-');
    }

    // 标题 # / ## / ### …
    {
        size_t i = 0;
        while (i < line.size() && line[i] == '#') ++i;
        if (i > 0 && i < line.size() && line[i] == ' ')
            line = line.substr(i + 1);
    }

    // 块引用 > …
    if (!line.empty() && line[0] == '>') {
        line = line.substr(1);
        if (!line.empty() && line[0] == ' ') line = line.substr(1);
    }

    // 内联格式：逐字符扫描去除标记
    std::string out;
    out.reserve(line.size());
    size_t n = line.size();
    for (size_t i = 0; i < n; ) {
        // **bold** 或 __bold__
        if (i + 1 < n &&
            ((line[i] == '*' && line[i+1] == '*') ||
             (line[i] == '_' && line[i+1] == '_'))) {
            i += 2;
            continue;
        }
        // *italic* 或 _italic_ ——只在非字母/数字边界时才是标记
        if ((line[i] == '*' || line[i] == '_')) {
            bool left_ok  = (i == 0)    || !std::isalnum(static_cast<unsigned char>(line[i-1]));
            bool right_ok = (i+1 == n)  || !std::isalnum(static_cast<unsigned char>(line[i+1]));
            if (left_ok || right_ok) { ++i; continue; }
        }
        // `inline code`
        if (line[i] == '`') { ++i; continue; }
        // [text](url) → text
        if (line[i] == '[') {
            size_t cb = line.find(']', i);
            if (cb != std::string::npos && cb + 1 < n && line[cb+1] == '(') {
                size_t cp = line.find(')', cb + 1);
                if (cp != std::string::npos) {
                    out += line.substr(i + 1, cb - i - 1);
                    i = cp + 1;
                    continue;
                }
            }
        }
        // ![alt](url) → (删除图片)
        if (line[i] == '!' && i + 1 < n && line[i+1] == '[') {
            size_t cb = line.find(']', i);
            if (cb != std::string::npos && cb + 1 < n && line[cb+1] == '(') {
                size_t cp = line.find(')', cb + 1);
                if (cp != std::string::npos) { i = cp + 1; continue; }
            }
        }
        out += line[i++];
    }
    return out;
}

}  // namespace utils
}  // namespace termind
