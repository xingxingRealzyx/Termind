#include "termind/tui.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <sstream>
#include <string>

#include <sys/ioctl.h>
#include <unistd.h>

namespace termind {
namespace tui {

// ── 终端信息 ─────────────────────────────────────────────────────────────

int TermWidth() {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0)
        return static_cast<int>(w.ws_col);
    return 80;
}

int TermHeight() {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 0)
        return static_cast<int>(w.ws_row);
    return 24;
}

bool IsAtty() { return isatty(STDOUT_FILENO) != 0; }

void HideCursor() {
    if (IsAtty()) ::write(STDOUT_FILENO, "\033[?25l", 6);
}

void ShowCursor() {
    if (IsAtty()) ::write(STDOUT_FILENO, "\033[?25h", 6);
}

// ── ThinkingPane ─────────────────────────────────────────────────────────

namespace {
constexpr const char* kSpinFrames[] = {
    "⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"};
constexpr int kSpinCount = 10;

// 按字节截断到 max_bytes（粗略，用于预览行）
std::string TruncateBytes(const std::string& s, int max_bytes) {
    if (static_cast<int>(s.size()) <= max_bytes) return s;
    return s.substr(0, static_cast<size_t>(max_bytes) - 1) + "…";
}

// 剥离 ANSI 转义序列（用于计算显示宽度）
std::string StripAnsi(const std::string& s) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '\033' && i + 1 < s.size() && s[i+1] == '[') {
            i += 2;
            while (i < s.size() && !std::isalpha(static_cast<unsigned char>(s[i]))) ++i;
            if (i < s.size()) ++i;
        } else {
            out += s[i++];
        }
    }
    return out;
}

// 近似计算 UTF-8 字符串的终端显示宽度（CJK = 2 列，其余 = 1 列）
int DisplayWidth(const std::string& s) {
    int width = 0;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            ++width; ++i;
        } else if (c < 0xC0) {
            ++i;  // continuation byte
        } else if (c < 0xE0) {
            ++width; i += 2;
        } else if (c < 0xF0) {
            // 3-byte sequence
            uint32_t code = 0;
            if (i + 2 < s.size()) {
                code = ((static_cast<uint32_t>(c) & 0x0F) << 12)
                     | ((static_cast<uint32_t>(static_cast<unsigned char>(s[i+1])) & 0x3F) << 6)
                     |  (static_cast<uint32_t>(static_cast<unsigned char>(s[i+2])) & 0x3F);
            }
            // Wide characters: CJK (U+2E80+) and commonly-2-wide symbols
            // U+2600-U+27FF: Misc Symbols & Dingbats (⚡●◆▶ etc.)
            // U+2B00-U+2BFF: Misc Symbols & Arrows
            bool wide = (code >= 0x2E80u)
                     || (code >= 0x2600u && code <= 0x27FFu)
                     || (code >= 0x2B00u && code <= 0x2BFFu);
            width += wide ? 2 : 1;
            i += 3;
        } else {
            width += 2; i += 4;  // 4-byte (emoji 等)
        }
    }
    return width;
}

// 构造 N 个 '─' 的填充串（'─' 是 3 字节 UTF-8）
std::string Dashes(int n) {
    std::string out;
    out.reserve(static_cast<size_t>(n) * 3);
    for (int i = 0; i < n; ++i) out += "─";
    return out;
}
}  // namespace

void ThinkingPane::Start(const std::string& heading) {
    if (running_.exchange(true)) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        heading_           = heading;
        content_buf_.clear();
        rendered_lines_    = 0;
        frame_             = 0;
        last_content_size_ = 0;
    }
    HideCursor();
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
        if (c != '\r') content_buf_ += c;
    }
}

void ThinkingPane::Stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ClearLines();
    }
    ShowCursor();
}

void ThinkingPane::Loop() {
    while (running_.load(std::memory_order_relaxed)) {
        if (IsAtty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            bool changed = (content_buf_.size() != last_content_size_);
            last_content_size_ = content_buf_.size();
            if (changed || rendered_lines_ == 0)
                Render();
            else
                RenderSpinnerOnly();
        }
        ++frame_;
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }
}

void ThinkingPane::ClearLines() {
    if (rendered_lines_ == 0) return;
    std::string out = "\033[" + std::to_string(rendered_lines_) + "A\033[J";
    ::write(STDOUT_FILENO, out.data(), out.size());
    rendered_lines_ = 0;
}

// 构建顶部边框行（含 spinner + heading），不含换行
static std::string BuildTopBorder(int tw, const std::string& spin,
                                   const std::string& heading, size_t frame) {
    (void)frame;  // spin 已由调用方选好
    std::string stripped = StripAnsi(heading);
    int H = DisplayWidth(stripped);
    // "  ╭─ " (5) + spin (1) + " " (1) + heading (H) + " " (1) + dashes (F) + "╮" (1) = tw
    // 额外 -2 安全边距，防止 ambiguous-width 字符（如 ⚡）导致行溢出换行
    int F = std::max(1, tw - H - 9 - 2);
    std::string out;
    out += color::kDim;
    out += "  ╭─ ";
    out += color::kReset;
    out += spin + " " + heading;
    out += std::string(color::kDim) + " ";
    out += Dashes(F);
    out += "╮";
    out += color::kReset;
    return out;
}

void ThinkingPane::RenderSpinnerOnly() {
    if (rendered_lines_ == 0) { Render(); return; }
    int tw = TermWidth();
    std::string spin(kSpinFrames[frame_ % kSpinCount]);
    std::string out;
    out += "\033[" + std::to_string(rendered_lines_) + "A\r\033[2K";
    out += BuildTopBorder(tw, spin, heading_, frame_);
    out += "\n";
    if (rendered_lines_ > 1) {
        out += "\033[" + std::to_string(rendered_lines_ - 1) + "B";
    }
    ::write(STDOUT_FILENO, out.data(), out.size());
}

void ThinkingPane::Render() {
    int tw = TermWidth();
    // 顶部边框使用 tw-2 宽（BuildTopBorder 有 -2 安全边距），
    // 内容行和底部边框同样用 tw-2 保持三段对齐
    int inner_w = std::max(10, tw - 8);  // 4("  │ ") + inner_w + 2(" │") = tw-2

    std::string out;
    out.reserve(4096);

    if (rendered_lines_ > 0) {
        // \r 先回到行首，再 \033[J 从列 0 清除到屏幕末尾，避免残留字符
        out += "\033[" + std::to_string(rendered_lines_) + "A\r\033[J";
    }

    // 顶部边框
    std::string spin(kSpinFrames[frame_ % kSpinCount]);
    out += BuildTopBorder(tw, spin, heading_, frame_);
    out += "\n";
    int printed = 1;

    // 内容行
    if (!content_buf_.empty()) {
        for (const auto& ln : LastLines(inner_w)) {
            int dw = DisplayWidth(ln);
            int pad = std::max(0, inner_w - dw);
            out += color::kDim;
            out += "  │ ";
            out += ln;
            out += std::string(static_cast<size_t>(pad), ' ');
            out += " │";
            out += std::string(color::kReset) + "\n";
            ++printed;
        }
    }

    // 底部边框（tw-2 宽，与顶部对齐）
    out += color::kDim;
    out += "  ╰";
    out += Dashes(tw - 6);   // 3("  ╰") + (tw-6) + 1("╯") = tw-2
    out += "╯";
    out += std::string(color::kReset) + "\n";
    ++printed;

    rendered_lines_ = printed;
    ::write(STDOUT_FILENO, out.data(), out.size());
}

std::vector<std::string> ThinkingPane::LastLines(int width) const {
    std::vector<std::string> raw;
    std::string cur;
    for (char c : content_buf_) {
        if (c == '\n') { raw.push_back(cur); cur.clear(); }
        else if (c == '\t') cur += "    ";
        else cur += c;
    }
    if (!cur.empty()) raw.push_back(cur);

    std::vector<std::string> lines;
    for (const auto& l : raw) {
        bool blank = true;
        for (unsigned char c : l)
            if (!std::isspace(c)) { blank = false; break; }
        if (!blank) lines.push_back(l);
    }

    int start = std::max(0, static_cast<int>(lines.size()) - kPreviewLines);
    std::vector<std::string> out;
    for (int i = start; i < static_cast<int>(lines.size()); ++i)
        out.push_back(TruncateBytes(lines[i], width));
    return out;
}

// ── TaskPanel ─────────────────────────────────────────────────────────────

void TaskPanel::SetTasks(const std::vector<std::string>& descs) {
    tasks_.clear();
    for (const auto& d : descs)
        tasks_.push_back({d, Status::kPending});
    rendered_lines_ = 0;
}

void TaskPanel::EnsureAtLeast(size_t n) {
    while (tasks_.size() < n)
        tasks_.push_back({"步骤 " + std::to_string(tasks_.size() + 1), Status::kPending});
}

void TaskPanel::MarkDone(size_t idx) {
    if (idx < tasks_.size()) tasks_[idx].status = Status::kDone;
    for (size_t i = idx + 1; i < tasks_.size(); ++i) {
        if (tasks_[i].status == Status::kPending) {
            tasks_[i].status = Status::kActive;
            break;
        }
    }
}

void TaskPanel::ActivateFirst() {
    for (auto& t : tasks_) {
        if (t.status == Status::kPending) { t.status = Status::kActive; return; }
    }
}

void TaskPanel::AdvanceActive() {
    size_t ai = tasks_.size();
    for (size_t i = 0; i < tasks_.size(); ++i) {
        if (tasks_[i].status == Status::kActive) {
            tasks_[i].status = Status::kDone; ai = i; break;
        }
    }
    for (size_t i = ai + 1; i < tasks_.size(); ++i) {
        if (tasks_[i].status == Status::kPending) {
            tasks_[i].status = Status::kActive; break;
        }
    }
}

void TaskPanel::DoClear() {
    if (rendered_lines_ == 0) return;
    std::string out = "\033[" + std::to_string(rendered_lines_) + "A\033[J";
    ::write(STDOUT_FILENO, out.data(), out.size());
    rendered_lines_ = 0;
}

void TaskPanel::Clear() { rendered_lines_ = 0; }

void TaskPanel::Render() {
    DoClear();
    int done  = DoneCount();
    int total = static_cast<int>(tasks_.size());
    int tw    = std::max(40, TermWidth());
    std::string out;
    out.reserve(2048);

    std::string badge = " 执行计划 " + std::to_string(done) + "/" +
                        std::to_string(total) + " ";
    int inner = tw - 2;
    int right = inner - 1 - static_cast<int>(badge.size());
    if (right < 0) right = 0;

    out += color::kDim;  out += "╭─"; out += color::kReset;
    out += color::kBold; out += badge; out += color::kReset;
    out += color::kDim;
    for (int i = 0; i < right; ++i) out += "─";
    out += "╮\n"; out += color::kReset;
    int lines = 1;

    for (size_t i = 0; i < tasks_.size(); ++i) {
        const auto& t = tasks_[i];
        const char* icon   = nullptr;
        const char* c_desc = nullptr;
        switch (t.status) {
            case Status::kDone:   icon = "✅"; c_desc = color::kDim;          break;
            case Status::kActive: icon = "⏳"; c_desc = color::kBrightYellow; break;
            default:              icon = "⬜"; c_desc = color::kDim;           break;
        }
        std::string num    = std::to_string(i + 1) + ".";
        int prefix_cols    = 3 + static_cast<int>(num.size()) + 2;
        int max_desc       = tw - prefix_cols - 2;
        if (max_desc < 8) max_desc = 8;
        std::string desc   = t.desc;
        if (static_cast<int>(desc.size()) > max_desc)
            desc = desc.substr(0, static_cast<size_t>(max_desc) - 1) + "…";

        out += color::kDim; out += "│ "; out += color::kReset;
        out += icon; out += " ";
        out += color::kDim; out += num; out += color::kReset;
        out += " "; out += c_desc; out += desc; out += color::kReset;
        out += "\n"; ++lines;
    }

    out += color::kDim; out += "╰";
    for (int i = 0; i < tw - 2; ++i) out += "─";
    out += "╯\n"; out += color::kReset; ++lines;

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

// ── StreamRenderer ────────────────────────────────────────────────────────
//
// 状态机说明：
//   kNormal   正文，'<' 进 kTag，'[' 进 kStep
//   kTag      积累潜在 <think> 或 </think>；若 buf_=="<think>" 进 kInThink；
//             若 buf_=="</think>" 回 kNormal；
//             若不能继续匹配，输出 buf_ 回 kNormal 并重新处理当前字符
//   kInThink  think 内容，路由到 think_out_（为空则静默丢弃）；'<' 进 kThinkTag
//   kThinkTag 积累潜在 </think>；匹配成功回 kNormal；否则原样路由并回 kInThink
//   kStep     积累 [[DONE:N]]；匹配成功静默丢弃；否则输出 buf_ 回 kNormal

void StreamRenderer::Emit(const std::string& s) {
    if (s.empty()) return;
    has_emitted_ = true;
    out_(s);
}

// think 内容路由：有 think_out_ 则转发，否则静默丢弃
void StreamRenderer::EmitThink(const std::string& s) {
    if (s.empty()) return;
    if (think_out_) think_out_(s);
}

void StreamRenderer::Feed(const std::string& chunk) {
    for (char c : chunk) ProcessChar(c);
}

void StreamRenderer::Flush() {
    if (!buf_.empty()) {
        if (state_ == State::kInThink || state_ == State::kThinkTag) {
            EmitThink(buf_);  // 冲刷残留的 think 内容
        } else {
            Emit(buf_);
        }
        buf_.clear();
    }
    state_ = State::kNormal;
}

void StreamRenderer::ProcessChar(char c) {
    static const std::string kOpen  = "<think>";
    static const std::string kClose = "</think>";

    switch (state_) {

    // ── kNormal ──────────────────────────────────────────────────────────
    case State::kNormal:
        if (c == '<') { state_ = State::kTag; buf_ = "<"; return; }
        if (c == '[') { state_ = State::kStep; buf_ = "["; return; }
        Emit(std::string(1, c));
        return;

    // ── kTag（正文中的 < 开头）──────────────────────────────────────────
    case State::kTag:
        buf_ += c;
        if (buf_ == kOpen) {
            buf_.clear();
            state_ = State::kInThink;
            // think 内容将路由到 think_out_，正文不输出任何样式前缀
            return;
        }
        if (buf_ == kClose) {
            // 在正文里出现 </think> 不合预期，直接输出
            Emit(buf_); buf_.clear(); state_ = State::kNormal;
            return;
        }
        // 还能继续匹配
        if (kOpen.substr(0, buf_.size())  == buf_) return;
        if (kClose.substr(0, buf_.size()) == buf_) return;
        // 无法匹配：emit 首字符 '<' 后重新处理其余（避免把 '<' 重新喂入而无限递归）
        {
            std::string rest(buf_, 1);
            buf_.clear(); state_ = State::kNormal;
            Emit("<");
            for (char x : rest) ProcessChar(x);
        }
        return;

    // ── kInThink（think 内容，路由到 think_out_）────────────────────────
    case State::kInThink:
        if (c == '<') { state_ = State::kThinkTag; buf_ = "<"; return; }
        EmitThink(std::string(1, c));  // 路由给 ThinkingPane，不输出到正文
        return;

    // ── kThinkTag（think 内容中的 < 开头）──────────────────────────────
    case State::kThinkTag:
        buf_ += c;
        if (buf_ == kClose) {
            buf_.clear(); state_ = State::kNormal;
            // think 结束，不输出任何内容到正文
            return;
        }
        if (kClose.substr(0, buf_.size()) == buf_) return;  // 继续匹配
        // 匹配失败：把 '<' 及余下内容路由到 think_out_，回 kInThink
        {
            std::string rest(buf_, 1);
            buf_.clear(); state_ = State::kInThink;
            EmitThink("<");
            for (char x : rest) ProcessChar(x);
        }
        return;

    // ── kStep（[[DONE:N]] 匹配）─────────────────────────────────────────
    case State::kStep: {
        buf_ += c;
        static const std::string kPfx = "[[DONE:";
        size_t end = buf_.find("]]");
        if (end != std::string::npos) {
            // 完整 [[DONE:N]] → 静默丢弃
            buf_.clear(); state_ = State::kNormal;
            return;
        }
        if (buf_.size() > 20) {
            // 缓冲过长，肯定不是 [[DONE:N]]；emit '[' 后重新处理其余（避免无限递归）
            std::string rest(buf_, 1);
            buf_.clear(); state_ = State::kNormal;
            Emit("[");
            for (char x : rest) ProcessChar(x);
            return;
        }
        if (buf_.size() <= kPfx.size() &&
            kPfx.substr(0, buf_.size()) != buf_) {
            // 前缀不匹配；emit '[' 后重新处理其余（避免无限递归）
            std::string rest(buf_, 1);
            buf_.clear(); state_ = State::kNormal;
            Emit("[");
            for (char x : rest) ProcessChar(x);
            return;
        }
        // 还能继续匹配
        return;
    }

    }  // switch
}

}  // namespace tui
}  // namespace termind
