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
int GetTerminalHeight();
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

// ── 任务进度面板 TaskPanel ────────────────────────────────────────────────
// 静态打印（无动画），调用 Render() 打印当前状态块，再次调用自动清除并重绘。
class TaskPanel {
public:
    enum class Status { kPending, kActive, kDone };

    TaskPanel() = default;

    TaskPanel(const TaskPanel&)            = delete;
    TaskPanel& operator=(const TaskPanel&) = delete;

    void SetTasks(const std::vector<std::string>& descs);

    // 标记第 idx 个任务完成（0-based），并自动激活下一个 Pending 任务
    void MarkDone(size_t idx);

    // 激活第一个 Pending 任务（通常在开始执行前调用）
    void ActivateFirst();

    // 将当前 Active 任务标为 Done，并激活下一个 Pending（用于自动推进）
    void AdvanceActive();

    // 清除上次渲染区域，重新打印当前状态
    void Render();

    // 仅清除上次渲染区域（不重绘）
    void Clear();

    bool   AllDone()   const;
    int    DoneCount() const;
    size_t Size()      const { return tasks_.size(); }
    bool   Empty()     const { return tasks_.empty(); }

private:
    struct Task {
        std::string desc;
        Status      status = Status::kPending;
    };
    std::vector<Task> tasks_;
    int rendered_lines_ = 0;

    void DoClear();
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

    // 注入原始文本 chunk（线程安全）；不做转义处理，适合 shell 命令输出
    void FeedRaw(const std::string& chunk);

    // 停止并清除所有渲染行（可多次调用，幂等）
    void Stop();

    bool IsRunning() const { return running_.load(std::memory_order_relaxed); }

private:
    void Loop();
    void Render();            // 调用方必须持有 mutex_：全量清除+重绘
    void RenderSpinnerOnly(); // 调用方必须持有 mutex_：仅就地刷新 spinner 行
    void ClearLines();        // 调用方必须持有 mutex_
    std::vector<std::string> LastLines(int width) const;  // 调用方必须持有 mutex_

    std::string       heading_;
    std::string       content_buf_;
    int               rendered_lines_    = 0;
    size_t            frame_             = 0;
    size_t            last_content_size_ = 0;

    std::atomic<bool> running_{false};
    std::thread       thread_;
    std::mutex        mutex_;
};

}  // namespace utils
}  // namespace termind
