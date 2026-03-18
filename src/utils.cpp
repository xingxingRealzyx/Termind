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

namespace {
// Braille 旋转动画帧（10 帧）
constexpr const char* kSpinnerFrames[] = {
    "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"
};
constexpr int kNumFrames = 10;
constexpr int kFrameMs   = 80;
}  // namespace

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
    while (std::getline(ss, token, delim)) {
        result.push_back(token);
    }
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
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) {
        return static_cast<int>(w.ws_col);
    }
    return 80;
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
    // 写入临时文件
    char old_tmp[] = "/tmp/termind_old_XXXXXX";
    char new_tmp[] = "/tmp/termind_new_XXXXXX";

    int old_fd = mkstemp(old_tmp);
    int new_fd = mkstemp(new_tmp);
    if (old_fd < 0 || new_fd < 0) return "(diff 不可用)";

    if (write(old_fd, old_content.data(),
              static_cast<ssize_t>(old_content.size())) < 0) {}
    if (write(new_fd, new_content.data(),
              static_cast<ssize_t>(new_content.size())) < 0) {}
    close(old_fd);
    close(new_fd);

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

    unlink(old_tmp);
    unlink(new_tmp);

    return result.empty() ? "(无变化)" : result;
}

// ── 用户交互 ──────────────────────────────────────────────────────────────

char AskYesNoEdit(const std::string& prompt) {
    std::cout << color::kBrightYellow << "\n? " << color::kReset
              << prompt << " "
              << color::kDim << "[y]es / [n]o / [e]dit" << color::kReset
              << "  ";
    std::cout.flush();

    std::string line;
    if (!std::getline(std::cin, line)) return 'n';
    line = ToLower(Trim(line));
    if (line.empty()) return 'n';
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

// ── Spinner ───────────────────────────────────────────────────────────────

void Spinner::Start(const std::string& message) {
    // 幂等：已在运行则先停止
    if (running_.exchange(true)) return;
    {
        std::lock_guard<std::mutex> lock(message_mutex_);
        message_ = message;
    }
    thread_ = std::thread([this] { ThreadFunc(); });
}

void Spinner::Stop() {
    // 设为 false；如果之前已经是 false，直接返回（幂等）
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    // 清除整行（\r 回到行首，\033[2K 擦除整行）
    std::cout << "\r\033[2K";
    std::cout.flush();
}

void Spinner::SetMessage(const std::string& message) {
    std::lock_guard<std::mutex> lock(message_mutex_);
    message_ = message;
}

void Spinner::ThreadFunc() {
    int frame = 0;
    while (running_.load(std::memory_order_relaxed)) {
        {
            std::lock_guard<std::mutex> lock(message_mutex_);
            std::cout << "\r"
                      << color::kDim
                      << kSpinnerFrames[frame % kNumFrames]
                      << " " << message_
                      << color::kReset
                      << "   ";  // 尾部空格覆盖更长的旧文字
            std::cout.flush();
        }
        ++frame;
        std::this_thread::sleep_for(std::chrono::milliseconds(kFrameMs));
    }
}

// ── ThinkingPane ──────────────────────────────────────────────────────────

namespace {
constexpr const char* kPaneFrames[] = {
    "⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"};
constexpr int kPaneNumFrames = 10;
}  // namespace

void ThinkingPane::Start(const std::string& heading) {
    if (running_.exchange(true)) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        heading_        = heading;
        content_buf_.clear();
        rendered_lines_ = 0;
        frame_          = 0;
    }
    thread_ = std::thread([this] { Loop(); });
}

void ThinkingPane::SetHeading(const std::string& heading) {
    std::lock_guard<std::mutex> lock(mutex_);
    heading_ = heading;
}

void ThinkingPane::Feed(const std::string& chunk) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < chunk.size(); ++i) {
        char c = chunk[i];
        if (c == '\r') continue;
        // 展开 JSON 字符串转义（工具参数里常见）
        if (c == '\\' && i + 1 < chunk.size()) {
            char nx = chunk[i + 1];
            if (nx == 'n')  { content_buf_ += '\n'; ++i; continue; }
            if (nx == 't')  { content_buf_ += '\t'; ++i; continue; }
            if (nx == '\\') { content_buf_ += '\\'; ++i; continue; }
            if (nx == '"')  { content_buf_ += '"';  ++i; continue; }
        }
        content_buf_ += c;
    }
}

void ThinkingPane::Stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    // 后台线程已退出，可安全操作 stdout
    std::lock_guard<std::mutex> lock(mutex_);
    ClearLines();  // 单次 write 原子清除
}

void ThinkingPane::Loop() {
    while (running_.load(std::memory_order_relaxed)) {
        if (isatty(STDOUT_FILENO)) {
            std::lock_guard<std::mutex> lock(mutex_);
            Render();  // 清除 + 重绘合并为单次 write，无中间空白帧
        }
        ++frame_;
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }
}

void ThinkingPane::ClearLines() {
    // 供 Stop() 调用：单次 write 原子擦除渲染区域
    if (rendered_lines_ == 0) return;
    std::string out = "\033[" + std::to_string(rendered_lines_) + "A\033[J";
    ::write(STDOUT_FILENO, out.data(), out.size());
    rendered_lines_ = 0;
}

void ThinkingPane::Render() {
    // 将清除 + 新内容合并进一个 string，最后一次 ::write() 原子输出。
    // 这样终端永远不会看到"清空后、新内容到达前"的中间空白帧，消除闪烁。
    std::string out;
    out.reserve(1024);

    // ① 清除上一帧（若有）
    if (rendered_lines_ > 0) {
        out += "\033[";
        out += std::to_string(rendered_lines_);
        out += "A\033[J";
    }

    // ② 行 1：spinner + 标题
    out += color::kDim;
    out += kPaneFrames[frame_ % kPaneNumFrames];
    out += " ";
    out += heading_;
    out += color::kReset;
    out += "\n";
    int printed = 1;

    // ③ 行 2..N+1：内容预览（有内容时才输出）
    if (!content_buf_.empty()) {
        int tw = GetTerminalWidth();
        int pw = std::max(20, tw - 6);
        for (const auto& ln : LastLines(pw)) {
            out += color::kDim;
            out += "  ╎ ";
            out += ln;
            out += color::kReset;
            out += "\n";
            ++printed;
        }
    }

    rendered_lines_ = printed;

    // ④ 单次系统调用写出，避免多次写之间的撕裂
    ::write(STDOUT_FILENO, out.data(), out.size());
}

std::vector<std::string> ThinkingPane::LastLines(int width) const {
    // 按 \n 分割 content_buf_
    std::vector<std::string> raw;
    std::string cur;
    for (char c : content_buf_) {
        if (c == '\n') {
            raw.push_back(cur);
            cur.clear();
        } else if (c == '\t') {
            cur += "    ";
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) raw.push_back(cur);

    // 过滤纯空白行
    std::vector<std::string> lines;
    for (const auto& l : raw) {
        bool blank = true;
        for (unsigned char c : l) {
            if (!std::isspace(c)) { blank = false; break; }
        }
        if (!blank) lines.push_back(l);
    }

    // 取最后 kPreviewLines 行，超宽截断
    int start = std::max(0, static_cast<int>(lines.size()) - kPreviewLines);
    std::vector<std::string> out;
    for (int i = start; i < static_cast<int>(lines.size()); ++i) {
        const std::string& l = lines[i];
        if (static_cast<int>(l.size()) <= width) {
            out.push_back(l);
        } else {
            out.push_back(l.substr(0, static_cast<size_t>(width) - 1) + "…");
        }
    }
    return out;
}

}  // namespace utils
}  // namespace termind
