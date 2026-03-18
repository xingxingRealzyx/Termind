#include "termind/context_manager.h"
#include "termind/utils.h"

#include <sstream>
#include <unordered_map>

namespace termind {

namespace fs = std::filesystem;

// ── 默认系统提示词 ────────────────────────────────────────────────────────
static constexpr const char* kDefaultSystemPrompt = R"(你是 Termind（端脑），一个运行在终端中的智能代码助手，类似于 Claude Code。

你能够读取、搜索和修改代码文件，并执行 shell 命令来帮助用户解决编程问题。

## 工作原则
1. **先阅读，再修改**：修改代码前，先用工具阅读相关文件，理解现有实现。
2. **精准修改**：优先使用 edit_file（精准替换）而非 write_file（全量覆盖）。
3. **解释清楚**：说明你为什么这么做，以及修改了什么。
4. **最小变更**：只修改必要的部分，避免不必要的重构。
5. **验证结果**：修改后可以读取文件或执行命令来验证效果。

## 工具说明
- read_file：读取文件内容（支持行范围）
- write_file：覆盖写入文件（需确认）
- edit_file：精准替换文件中的某段内容（需确认，推荐用于小改动）
- list_directory：列出目录结构
- search_files：按文件名 glob 搜索文件
- grep_code：在代码中搜索文本或正则
- run_shell：执行 shell 命令（需确认）
- get_file_info：获取文件元数据

对于需要确认的操作，用户会看到预览并决定是否执行。)";

// ── 构造函数 ──────────────────────────────────────────────────────────────

ContextManager::ContextManager() {
    working_dir_ = fs::current_path();
    system_message_ = kDefaultSystemPrompt;
}

// ── 工作目录 ──────────────────────────────────────────────────────────────

void ContextManager::SetWorkingDir(const fs::path& dir) {
    working_dir_ = dir;
}

// ── 系统消息 ──────────────────────────────────────────────────────────────

void ContextManager::SetSystemMessage(const std::string& content) {
    system_message_ = content;
}

// ── 文件上下文 ────────────────────────────────────────────────────────────

bool ContextManager::AddFile(const fs::path& path) {
    // 检查是否已添加
    for (const auto& fc : file_contexts_) {
        if (fs::equivalent(fc.path, path)) return true;
    }

    auto content = utils::ReadFile(path);
    if (!content) return false;

    FileContext fc;
    fc.path     = path;
    fc.content  = *content;
    fc.language = DetectLanguage(path);
    file_contexts_.push_back(std::move(fc));
    return true;
}

void ContextManager::RemoveFile(const fs::path& path) {
    file_contexts_.erase(
        std::remove_if(file_contexts_.begin(), file_contexts_.end(),
                       [&](const FileContext& fc) {
                           std::error_code ec;
                           return fs::equivalent(fc.path, path, ec);
                       }),
        file_contexts_.end());
}

void ContextManager::ClearFiles() {
    file_contexts_.clear();
}

// ── 对话历史 ──────────────────────────────────────────────────────────────

void ContextManager::AddUserMessage(const std::string& content) {
    history_.push_back(Message::User(content));
}

void ContextManager::AddAssistantMessage(const std::string& content) {
    history_.push_back(Message::Assistant(content));
}

void ContextManager::AddAssistantToolCalls(
    const std::vector<ToolCallRequest>& calls,
    const std::string& content) {
    history_.push_back(Message::AssistantWithCalls(calls, content));
}

void ContextManager::AddToolResult(const std::string& tool_call_id,
                                    const std::string& result) {
    history_.push_back(Message::Tool(tool_call_id, result));
}

void ContextManager::ClearHistory() {
    history_.clear();
}

void ContextManager::ClearAll() {
    history_.clear();
    file_contexts_.clear();
}

// ── 构建发送给 AI 的完整消息列表 ─────────────────────────────────────────

std::vector<Message> ContextManager::GetMessages() const {
    std::vector<Message> messages;

    // 1. 系统消息（含工作目录信息）
    std::string sys = system_message_;
    sys += "\n\n当前工作目录: " + working_dir_.string();
    messages.push_back(Message::System(sys));

    // 2. 文件上下文（作为 user 消息注入）
    if (!file_contexts_.empty()) {
        std::string files_block = BuildFilesBlock();
        messages.push_back(Message::User(
            "以下是相关文件的内容，请在回答时参考：\n\n" + files_block));
        // 占位 assistant 响应，避免对话结构问题
        messages.push_back(Message::Assistant(
            "已阅读这些文件，我会在回答时参考它们。"));
    }

    // 3. 对话历史
    for (const auto& m : history_) {
        messages.push_back(m);
    }

    return messages;
}

// ── 预估 token 数 ──────────────────────────────────────────────────────────

size_t ContextManager::EstimateTokens() const {
    size_t chars = system_message_.size();
    for (const auto& fc : file_contexts_) chars += fc.content.size();
    for (const auto& m : history_) {
        chars += m.content.size();
        // tool_calls 的 JSON 也计入
        if (m.tool_calls) {
            for (const auto& tc : *m.tool_calls)
                chars += tc.name.size() + tc.arguments.dump().size();
        }
    }
    return chars / 4;  // 粗略估算：4 字符 ≈ 1 token
}

// ── 上下文压缩 ────────────────────────────────────────────────────────────

// 从后往前找第 n 个 user 消息的下标；找不到返回 history_.size()
size_t ContextManager::FindUserTurnBoundary(int n_from_end) const {
    int count = 0;
    for (int i = static_cast<int>(history_.size()) - 1; i >= 0; --i) {
        if (history_[i].role == MessageRole::kUser) {
            ++count;
            if (count == n_from_end)
                return static_cast<size_t>(i);
        }
    }
    return history_.size();  // 未找到
}

int ContextManager::TrimToFit(size_t max_tokens,
                                int keep_recent_turns,
                                size_t old_tool_max_chars) {
    if (max_tokens == 0 || EstimateTokens() <= max_tokens) return 0;

    int dropped = 0;

    // ── 阶段 1：截断旧工具结果 ───────────────────────────────────────────
    // 找到"保留区"起始：从后往前第 keep_recent_turns 个 user 消息
    size_t keep_from = FindUserTurnBoundary(keep_recent_turns);

    for (size_t i = 0; i < keep_from && i < history_.size(); ++i) {
        auto& msg = history_[i];
        if (msg.role == MessageRole::kTool &&
            msg.content.size() > old_tool_max_chars) {
            size_t orig = msg.content.size();
            msg.content = msg.content.substr(0, old_tool_max_chars) +
                          "\n… [已压缩，原 " + std::to_string(orig) +
                          " 字符，省略 " +
                          std::to_string(orig - old_tool_max_chars) + " 字符]";
        }
    }

    // ── 阶段 2：丢弃最老的完整对话轮次 ──────────────────────────────────
    // 每次循环：找到第一个 user 消息之后的下一个 user 消息位置，
    // 把 [0, next_user) 整段丢弃（保留至少 keep_recent_turns 个轮次）
    while (EstimateTokens() > max_tokens) {
        // 至少保留最后 keep_recent_turns 个完整轮次
        size_t safe_keep = FindUserTurnBoundary(keep_recent_turns);
        if (safe_keep == 0 || safe_keep >= history_.size()) break;

        // 找第一个 user 之后的下一个 user（即第二个 user）的位置
        size_t next_user = history_.size();
        for (size_t i = 1; i < safe_keep; ++i) {
            if (history_[i].role == MessageRole::kUser) {
                next_user = i;
                break;
            }
        }
        if (next_user >= safe_keep) break;  // 找不到可丢弃的轮次

        dropped += static_cast<int>(next_user);
        history_.erase(history_.begin(),
                        history_.begin() + static_cast<std::ptrdiff_t>(next_user));
    }

    if (dropped > 0 || /* phase1 changed something */ true) {
        // 只有真正丢弃了消息才计数
        if (dropped > 0) ++compress_count_;
    }

    return dropped;
}

// ── 私有方法 ──────────────────────────────────────────────────────────────

std::string ContextManager::BuildFilesBlock() const {
    std::ostringstream ss;
    for (const auto& fc : file_contexts_) {
        std::string rel = utils::GetRelativePath(fc.path, working_dir_);
        ss << "```" << fc.language << " " << rel << "\n"
           << fc.content
           << (fc.content.empty() || fc.content.back() != '\n' ? "\n" : "")
           << "```\n\n";
    }
    return ss.str();
}

// static
std::string ContextManager::DetectLanguage(const fs::path& path) {
    static const std::unordered_map<std::string, std::string> kLangMap = {
        {".cpp",  "cpp"},   {".cxx", "cpp"},  {".cc",  "cpp"},
        {".c",   "c"},      {".h",   "cpp"},  {".hpp", "cpp"},
        {".py",  "python"}, {".js",  "javascript"}, {".ts", "typescript"},
        {".rs",  "rust"},   {".go",  "go"},   {".java", "java"},
        {".rb",  "ruby"},   {".sh",  "bash"}, {".zsh", "bash"},
        {".md",  "markdown"},{".json","json"}, {".yaml","yaml"},
        {".yml", "yaml"},   {".toml","toml"}, {".cmake","cmake"},
        {".txt", ""},       {".xml", "xml"},  {".html","html"},
        {".css", "css"},    {".sql", "sql"},
    };

    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    auto it = kLangMap.find(ext);
    return it != kLangMap.end() ? it->second : "";
}

}  // namespace termind
