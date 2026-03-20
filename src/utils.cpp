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

int GetTerminalHeight() {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 0) {
        return static_cast<int>(w.ws_row);
    }
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

    // RAII：确保临时文件在任何退出路径都被删除
    struct TmpGuard {
        char* path;
        int   fd;
        ~TmpGuard() {
            if (fd >= 0) close(fd);
            unlink(path);
        }
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

// ── TaskPanel ─────────────────────────────────────────────────────────────

void TaskPanel::SetTasks(const std::vector<std::string>& descs) {
    tasks_.clear();
    for (const auto& d : descs)
        tasks_.push_back({d, Status::kPending});
    rendered_lines_ = 0;
}

void TaskPanel::MarkDone(size_t idx) {
    if (idx < tasks_.size())
        tasks_[idx].status = Status::kDone;
    // 激活下一个未开始的任务
    for (size_t i = idx + 1; i < tasks_.size(); ++i) {
        if (tasks_[i].status == Status::kPending) {
            tasks_[i].status = Status::kActive;
            break;
        }
    }
}

void TaskPanel::ActivateFirst() {
    for (auto& t : tasks_) {
        if (t.status == Status::kPending) {
            t.status = Status::kActive;
            return;
        }
    }
}

void TaskPanel::AdvanceActive() {
    // 找到当前 Active 任务 → 标为 Done
    size_t active_idx = tasks_.size();
    for (size_t i = 0; i < tasks_.size(); ++i) {
        if (tasks_[i].status == Status::kActive) {
            tasks_[i].status = Status::kDone;
            active_idx = i;
            break;
        }
    }
    // 激活下一个 Pending 任务
    for (size_t i = active_idx + 1; i < tasks_.size(); ++i) {
        if (tasks_[i].status == Status::kPending) {
            tasks_[i].status = Status::kActive;
            break;
        }
    }
}

void TaskPanel::DoClear() {
    if (rendered_lines_ == 0) return;
    std::string out = "\033[" + std::to_string(rendered_lines_) + "A\033[J";
    ::write(STDOUT_FILENO, out.data(), out.size());
    rendered_lines_ = 0;
}

void TaskPanel::Clear() {
    rendered_lines_ = 0;
}

void TaskPanel::Render() {
    DoClear();

    int done  = DoneCount();
    int total = static_cast<int>(tasks_.size());
    int tw    = std::max(40, GetTerminalWidth());

    std::string out;
    out.reserve(2048);

    // ── 顶部边框 ──────────────────────────────────────────────────────────
    std::string badge = " 执行计划 " + std::to_string(done) + "/" +
                        std::to_string(total) + " ";
    int inner = tw - 2;  // ╭ 和 ╮ 各占 1 列
    int right = inner - 1 - static_cast<int>(badge.size());
    if (right < 0) right = 0;

    out += color::kDim;
    out += "╭─";
    out += color::kReset;
    out += color::kBold;
    out += badge;
    out += color::kReset;
    out += color::kDim;
    out += Repeat("─", right);
    out += "╮\n";
    out += color::kReset;
    int lines = 1;

    // ── 任务行 ────────────────────────────────────────────────────────────
    for (size_t i = 0; i < tasks_.size(); ++i) {
        const auto& t = tasks_[i];

        const char* icon   = nullptr;
        const char* c_desc = nullptr;
        const char* c_num  = color::kDim;
        switch (t.status) {
            case Status::kDone:
                icon   = "✅"; c_desc = color::kDim;          break;
            case Status::kActive:
                icon   = "⏳"; c_desc = color::kBrightYellow; break;
            default:
                icon   = "⬜"; c_desc = color::kDim;          break;
        }

        std::string num  = std::to_string(i + 1) + ".";
        // "│ " + icon(2字符宽) + " " + num + " " = 固定前缀
        // icon 是 UTF-8 三字节但终端显示 2 列，所以不能用 size() 算宽度
        // 保守估计前缀占 (3 + num.size() + 3) 列：│ [icon] [num] desc │
        int prefix_cols = 3 + static_cast<int>(num.size()) + 2;
        int max_desc    = tw - prefix_cols - 2;  // 留 2 列给右侧 " │"
        if (max_desc < 8) max_desc = 8;

        std::string desc = t.desc;
        // 粗略截断（按字节，非完美但够用）
        if (static_cast<int>(desc.size()) > max_desc)
            desc = desc.substr(0, static_cast<size_t>(max_desc) - 1) + "…";

        out += color::kDim;
        out += "│ ";
        out += color::kReset;
        out += icon;
        out += " ";
        out += c_num;
        out += num;
        out += color::kReset;
        out += " ";
        out += c_desc;
        out += desc;
        out += color::kReset;
        out += "\n";
        ++lines;
    }

    // ── 底部边框 ──────────────────────────────────────────────────────────
    out += color::kDim;
    out += "╰";
    out += Repeat("─", tw - 2);
    out += "╯\n";
    out += color::kReset;
    ++lines;

    rendered_lines_ = lines;
    ::write(STDOUT_FILENO, out.data(), out.size());
}


bool TaskPanel::AllDone() const {
    if (tasks_.empty()) return false;
    for (const auto& t : tasks_)
        if (t.status != Status::kDone) return false;
    return true;
}

int TaskPanel::DoneCount() const {
    int c = 0;
    for (const auto& t : tasks_)
        if (t.status == Status::kDone) ++c;
    return c;
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
        last_content_size_ = 0;
    }
    // 隐藏光标，避免光标在帧间跳动造成闪烁
    if (isatty(STDOUT_FILENO)) {
        static constexpr char kHide[] = "\033[?25l";
        ::write(STDOUT_FILENO, kHide, sizeof(kHide) - 1);
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

void ThinkingPane::FeedRaw(const std::string& chunk) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (char c : chunk) {
        if (c == '\r') continue;
        content_buf_ += c;
    }
}

void ThinkingPane::Stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    // 后台线程已退出，可安全操作 stdout
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ClearLines();
    }
    // 恢复光标可见
    if (isatty(STDOUT_FILENO)) {
        static constexpr char kShow[] = "\033[?25h";
        ::write(STDOUT_FILENO, kShow, sizeof(kShow) - 1);
    }
}

void ThinkingPane::Loop() {
    while (running_.load(std::memory_order_relaxed)) {
        if (isatty(STDOUT_FILENO)) {
            std::lock_guard<std::mutex> lock(mutex_);
            bool content_changed = (content_buf_.size() != last_content_size_);
            last_content_size_ = content_buf_.size();
            if (content_changed || rendered_lines_ == 0) {
                // 有新内容：全量重绘
                Render();
            } else {
                // 仅更新第一行 spinner，不擦除内容行，避免多余刷新
                RenderSpinnerOnly();
            }
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

void ThinkingPane::RenderSpinnerOnly() {
    // 只刷新 spinner 行（第 1 行），内容行原地保留，避免整体擦屏引起的闪烁。
    // 光标约定：Render() 结束后，光标在所有内容行的下一行行首。
    // 本函数必须保持相同约定，否则下一次 Render() 的 \033[NA 会偏移。
    if (rendered_lines_ == 0) { Render(); return; }

    std::string out;
    out.reserve(256);

    // ① 上移到 spinner 行（行 1）
    out += "\033[";
    out += std::to_string(rendered_lines_);
    out += "A";

    // ② 擦除并重写 spinner（必须以 \n 结尾，让光标落在行 2 的行首）
    out += "\r\033[2K";
    out += color::kDim;
    out += kPaneFrames[frame_ % kPaneNumFrames];
    out += " ";
    out += heading_;
    out += color::kReset;
    out += "\n";  // 光标现在在行 2 行首

    // ③ 向下移到最后一行的下一行（行 rendered_lines_+1 行首）
    //    已经在行 2，还需再下移 rendered_lines_-1 行
    if (rendered_lines_ > 1) {
        out += "\033[";
        out += std::to_string(rendered_lines_ - 1);
        out += "B";
    }

    ::write(STDOUT_FILENO, out.data(), out.size());
}

void ThinkingPane::Render() {
    // 将清除 + 新内容合并进一个 string，最后一次 ::write() 原子输出。
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
