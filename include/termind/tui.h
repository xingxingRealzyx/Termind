#pragma once

// ── Termind TUI 组件库 ────────────────────────────────────────────────────
// 包含所有终端 UI 相关组件，与纯工具函数（utils.h）分离。
//
// 组件：
//   color        ANSI 转义常量
//   TermInfo     终端尺寸/类型查询
//   ThinkingPane 后台线程驱动的滚动预览面板（用于工具执行进度等）
//   TaskPanel    任务列表面板（可选，当前由 AI 自行在正文输出进度）
//   StreamRenderer AI 流式响应渲染器（处理 <think> 内联暗色、[[DONE]] 剥除）

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace termind {
namespace tui {

// ── ANSI 颜色/样式常量 ───────────────────────────────────────────────────
namespace color {
    constexpr const char* kReset        = "\033[0m";
    constexpr const char* kBold         = "\033[1m";
    constexpr const char* kDim          = "\033[2m";
    constexpr const char* kItalic       = "\033[3m";
    constexpr const char* kRed          = "\033[31m";
    constexpr const char* kGreen        = "\033[32m";
    constexpr const char* kYellow       = "\033[33m";
    constexpr const char* kBlue         = "\033[34m";
    constexpr const char* kMagenta      = "\033[35m";
    constexpr const char* kCyan         = "\033[36m";
    constexpr const char* kWhite        = "\033[37m";
    constexpr const char* kGray         = "\033[90m";
    constexpr const char* kBrightGreen  = "\033[92m";
    constexpr const char* kBrightYellow = "\033[93m";
    constexpr const char* kBrightCyan   = "\033[96m";
}

// ── 终端信息 ─────────────────────────────────────────────────────────────
int  TermWidth();
int  TermHeight();
bool IsAtty();
void HideCursor();
void ShowCursor();

// ── ThinkingPane ─────────────────────────────────────────────────────────
// 后台线程驱动的滚动面板：第 1 行 spinner+标题，第 2~5 行内容预览。
// Stop() 阻塞直到后台线程退出，并清除所有渲染行，之后可安全写 stdout。
class ThinkingPane {
public:
    static constexpr int kPreviewLines = 4;

    ThinkingPane()  = default;
    ~ThinkingPane() { Stop(); }

    ThinkingPane(const ThinkingPane&)            = delete;
    ThinkingPane& operator=(const ThinkingPane&) = delete;

    void Start(const std::string& heading);
    void SetHeading(const std::string& heading);

    // Feed：自动展开 JSON \n \t 转义（适合工具参数流）
    void Feed(const std::string& chunk);
    // FeedRaw：不做转义（适合 shell 输出）
    void FeedRaw(const std::string& chunk);

    void Stop();   // 幂等
    bool IsRunning() const { return running_.load(std::memory_order_relaxed); }

private:
    void Loop();
    void Render();
    void RenderSpinnerOnly();
    void ClearLines();
    std::vector<std::string> LastLines(int width) const;

    std::string       heading_;
    std::string       content_buf_;
    int               rendered_lines_    = 0;
    size_t            frame_             = 0;
    size_t            last_content_size_ = 0;

    std::atomic<bool> running_{false};
    std::thread       thread_;
    std::mutex        mutex_;
};

// ── TaskPanel ─────────────────────────────────────────────────────────────
// 静态任务列表面板（含边框、状态图标）。
// 当前由 AI 在正文自行输出进度，TaskPanel 保留供将来使用。
class TaskPanel {
public:
    enum class Status { kPending, kActive, kDone };

    TaskPanel() = default;
    TaskPanel(const TaskPanel&)            = delete;
    TaskPanel& operator=(const TaskPanel&) = delete;

    void SetTasks(const std::vector<std::string>& descs);
    void EnsureAtLeast(size_t n);
    void MarkDone(size_t idx);
    void ActivateFirst();
    void AdvanceActive();
    void Render();
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

// ── StreamRenderer ────────────────────────────────────────────────────────
// AI 流式响应渲染器：
//   - <think>...</think>：实时内联暗色输出（kDim 一次开，字节直透传，kReset 一次关）
//   - [[DONE:N]] 标记：静默剥除，不输出到终端
//   - 其余正文：通过 TextOut 回调透传给调用方
//
// 设计：
//   构造时传入 TextOut 回调，渲染器每产生可见文本就调用它。
//   调用方在回调中处理 first_chunk（停外层 pane、打印前缀等）。
//   渲染器内部无 ThinkingPane，不持有终端状态，职责单一。
class StreamRenderer {
public:
    // 每次产生可见正文（含 ANSI 转义）时回调
    using TextOut  = std::function<void(const std::string&)>;
    // <think>...</think> 内容回调（可选）；为空时 think 内容静默丢弃
    using ThinkOut = std::function<void(const std::string&)>;

    explicit StreamRenderer(TextOut out, ThinkOut think_out = nullptr)
        : out_(std::move(out)), think_out_(std::move(think_out)) {}

    StreamRenderer(const StreamRenderer&)            = delete;
    StreamRenderer& operator=(const StreamRenderer&) = delete;

    // 喂入一段流式 chunk（可多次调用）
    void Feed(const std::string& chunk);

    // 流结束时调用：冲刷未闭合的标签缓冲
    void Flush();

    // 是否已产生过可见输出
    bool HasEmitted() const { return has_emitted_; }

private:
    enum class State {
        kNormal,   // 正文模式
        kTag,      // 正在匹配 <think> 或 </think>（正文中的 <）
        kInThink,  // think 内容模式
        kThinkTag, // 正在匹配 </think>（think 内容中的 <）
        kStep,     // 正在匹配 [[DONE:N]]
    };

    State       state_       = State::kNormal;
    std::string buf_;          // 标签缓冲 或 [[DONE]] 缓冲
    bool        has_emitted_ = false;
    TextOut     out_;
    ThinkOut    think_out_;  // think 内容路由（为空时静默丢弃）

    void Emit(const std::string& s);
    void EmitThink(const std::string& s);  // 路由 think 内容到 think_out_
    void ProcessChar(char c);
};

}  // namespace tui
}  // namespace termind
