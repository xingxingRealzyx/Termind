#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace termind {
namespace utils {

// ── ANSI 终端颜色 ─────────────────────────────────────────────────────────
namespace color {
    constexpr const char* kReset   = "\033[0m";
    constexpr const char* kBold    = "\033[1m";
    constexpr const char* kDim     = "\033[2m";
    constexpr const char* kItalic  = "\033[3m";
    constexpr const char* kRed     = "\033[31m";
    constexpr const char* kGreen   = "\033[32m";
    constexpr const char* kYellow  = "\033[33m";
    constexpr const char* kBlue    = "\033[34m";
    constexpr const char* kMagenta = "\033[35m";
    constexpr const char* kCyan    = "\033[36m";
    constexpr const char* kWhite   = "\033[37m";
    constexpr const char* kGray    = "\033[90m";
    constexpr const char* kBrightGreen  = "\033[92m";
    constexpr const char* kBrightYellow = "\033[93m";
    constexpr const char* kBrightCyan   = "\033[96m";
}  // namespace color

// ── 字符串工具 ────────────────────────────────────────────────────────────
std::string Trim(const std::string& s);
std::string TrimLeft(const std::string& s);
std::string TrimRight(const std::string& s);
std::vector<std::string> Split(const std::string& s, char delim);
std::string Join(const std::vector<std::string>& parts, const std::string& sep);
bool StartsWith(const std::string& s, const std::string& prefix);
bool EndsWith(const std::string& s, const std::string& suffix);
std::string ToLower(const std::string& s);
std::string EscapeShellArg(const std::string& s);

// ── 文件工具 ──────────────────────────────────────────────────────────────
std::optional<std::string> ReadFile(const std::filesystem::path& path);
bool WriteFile(const std::filesystem::path& path, const std::string& content);
bool FileExists(const std::filesystem::path& path);
std::string GetRelativePath(const std::filesystem::path& path,
                             const std::filesystem::path& base);
std::string FormatFileSize(std::uintmax_t bytes);

// ── 终端工具 ──────────────────────────────────────────────────────────────
int GetTerminalWidth();
bool IsAtty();
std::string Repeat(const std::string& s, int n);
void PrintHorizontalRule(const std::string& title = "");
void PrintInfo(const std::string& msg);
void PrintWarning(const std::string& msg);
void PrintError(const std::string& msg);
void PrintSuccess(const std::string& msg);
void PrintDim(const std::string& msg);

// 彩色打印 diff 输出
void PrintColoredDiff(const std::string& diff_text);

// ── 差异比较（调用系统 diff）────────────────────────────────────────────
std::string ComputeDiff(const std::string& old_content,
                         const std::string& new_content,
                         const std::string& filename);

// ── 用户交互 ──────────────────────────────────────────────────────────────
// 返回 y/n/e (yes/no/edit)
char AskYesNoEdit(const std::string& prompt);
bool AskYesNo(const std::string& prompt, bool default_yes = false);
std::string AskString(const std::string& prompt,
                       const std::string& default_val = "");

// ── 环境变量 ──────────────────────────────────────────────────────────────
std::string GetEnv(const std::string& name,
                   const std::string& default_val = "");
std::string ExpandHome(const std::string& path);

// ── 终端动画 Spinner ──────────────────────────────────────────────────────
// 在后台线程显示旋转动画，调用 Stop() 后立即清除当前行。
// 线程安全：Stop() 会等待后台线程退出再返回，之后可安全写 stdout。
class Spinner {
public:
    Spinner() = default;
    ~Spinner() { Stop(); }

    Spinner(const Spinner&)            = delete;
    Spinner& operator=(const Spinner&) = delete;

    void Start(const std::string& message);
    void Stop();
    void SetMessage(const std::string& message);
    bool IsRunning() const { return running_.load(std::memory_order_relaxed); }

private:
    void ThreadFunc();

    std::atomic<bool>  running_{false};
    std::string        message_;
    std::mutex         message_mutex_;
    std::thread        thread_;
};

// ── 多行思考预览面板 ThinkingPane ─────────────────────────────────────────
// 后台线程负责动画刷新；主线程通过 Feed() 注入实时流内容（文字或工具参数）。
// 渲染：第 1 行是 spinner + 标题，第 2-5 行是内容的最后 N 行滚动预览。
// Stop() 等待后台线程并清除所有渲染行，之后可安全写 stdout。
class ThinkingPane {
public:
    static constexpr int kPreviewLines = 4;  // 内容预览区最大行数

    ThinkingPane() = default;
    ~ThinkingPane() { Stop(); }

    ThinkingPane(const ThinkingPane&)            = delete;
    ThinkingPane& operator=(const ThinkingPane&) = delete;

    // 开始显示，heading 作为标题行
    void Start(const std::string& heading);

    // 更新标题行（线程安全）
    void SetHeading(const std::string& heading);

    // 注入内容 chunk（线程安全）；自动展开 JSON \n \t 转义
    void Feed(const std::string& chunk);

    // 停止并清除所有渲染行（可多次调用，幂等）
    void Stop();

    bool IsRunning() const { return running_.load(std::memory_order_relaxed); }

private:
    void Loop();
    void Render();      // 调用方必须持有 mutex_
    void ClearLines();  // 调用方必须持有 mutex_
    std::vector<std::string> LastLines(int width) const;  // 调用方必须持有 mutex_

    std::string       heading_;
    std::string       content_buf_;
    int               rendered_lines_ = 0;
    size_t            frame_          = 0;

    std::atomic<bool> running_{false};
    std::thread       thread_;
    std::mutex        mutex_;
};

}  // namespace utils
}  // namespace termind
