#include "termind/context_manager.h"
#include "termind/utils.h"

#include <sstream>
#include <unordered_map>

namespace termind {

namespace fs = std::filesystem;

// ── 默认系统提示词 ────────────────────────────────────────────────────────
static constexpr const char* kDefaultSystemPrompt = R"(你是 Termind（端脑），一个运行在终端中的智能代码助手，类似于 Claude Code。
你能够读取、搜索和修改代码文件，执行 shell 命令，帮助用户解决各类编程问题。

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
## 核心工作流程（必须遵守）

处理任何编程任务时，必须按以下四个阶段执行：

### 阶段一：探索（Explore）
**在写任何代码之前**，先充分了解现有代码库：
- 用 `list_directory` 查看项目结构，理解模块划分
- 用 `search_symbol` 或 `grep_code` 定位相关函数、类、接口的实际位置
- 用 `read_file` 阅读目标文件，理解现有实现风格和接口约定
- **绝对不要依赖猜测**，一切以实际代码为准

### 阶段二：实现（Implement）
- 优先使用 `edit_file`（精准替换）而非 `write_file`（全量覆盖）
- 遵循现有代码的命名风格、缩进和注释习惯
- 每次只修改必要的部分，避免无关重构
- 如需新建文件，先确认目录结构后再创建

### 阶段三：验证（Verify）— 关键步骤，不可跳过
**代码修改后必须立即验证**，不得在未验证的情况下声称任务完成：
- 如果知道构建命令（来自 TERMIND.md 或上下文），立即执行 `run_shell` 编译
- 编译失败时：读取错误信息 → 定位问题 → 修复 → 再次编译，循环直到通过
- 如果项目有测试，运行相关测试验证行为正确
- 如果不确定构建命令，询问用户或查找 Makefile/CMakeLists.txt/package.json 等

### 阶段四：总结（Summarize）
- 简洁说明做了哪些改动，以及为什么这样改
- 如果遇到并解决了非显而易见的问题，主动用 `update_project_memory` 记录下来
- 完成编程任务后，在总结末尾列出 2~4 条具体的后续建议（编号列表），建议要可直接执行，不写"优化代码"这类泛泛的表述。

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
## 工具使用指南

| 工具 | 何时使用 |
|------|----------|
| `read_file` | 阅读文件，支持 start_line/end_line 精确读取某一段 |
| `get_file_outline` | **大文件必先调用**：获取类/函数/行号摘要，再按需切片读取 |
| `write_file` | 创建新文件或全量覆盖（需确认） |
| `edit_file` | 修改已有文件中的特定片段，old_content 必须唯一（推荐，需确认） |
| `search_symbol` | **优先**用于查找函数/类/变量的定义位置，支持按语言模式智能匹配 |
| `grep_code` | 搜索任意文本或正则表达式，适合搜索字符串、注释等 |
| `search_files` | 按文件名 glob 模式查找文件（如 `*.h`、`test_*.py`） |
| `list_directory` | 查看目录结构，支持递归，了解项目布局 |
| `run_shell` | 执行 shell 命令，如编译、测试、代码格式化（需确认） |
| `get_file_info` | 查看文件大小、修改时间等元数据 |

**工具选用原则**：
- 找函数定义 → `search_symbol`（比 grep 更精准）
- 找代码中的字符串/模式 → `grep_code`
- 找文件 → `search_files`
- 修改代码 → `edit_file`（小改动）或 `write_file`（新建/大改）

**大文件分层阅读策略（文件超过 200 行时必须遵守）**：
1. 先调 `get_file_outline` 获取结构（类名、函数名、行号）
2. 根据任务，用 `read_file` 的 `start_line`/`end_line` 只读相关片段
3. **不要一次性 `read_file` 整个大文件**——既浪费 token，又淹没关键信息

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
## 错误处理原则

遇到工具调用失败时，不要立即放弃：
1. **分析错误**：仔细阅读错误信息，判断是文件路径错误、权限问题还是逻辑错误
2. **验证假设**：用 `search_symbol` 或 `grep_code` 确认函数名/文件路径是否正确
3. **逐步修复**：每次只修复一个问题，修复后立即验证
4. **说明原因**：告诉用户遇到了什么问题，如何解决的

编译错误处理示例：
- 未定义符号 → 用 `search_symbol` 找到正确的声明位置和头文件
- 类型不匹配 → 用 `read_file` 查看相关类型定义
- 链接错误 → 检查 CMakeLists.txt 或 Makefile 的依赖配置

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
## 项目记忆维护（TERMIND.md）

以下情况**必须**主动调用 `update_project_memory` 将信息保存到 TERMIND.md：
- 用户告知或你发现了构建/运行/测试命令
- 发现了项目特有的代码约定或架构设计
- 解决了一个非显而易见的问题（如特殊的配置要求、已知 bug 的绕过方式）
- 了解到重要的文件路径或依赖关系

这些记忆将在下次对话中自动加载，让你更快上手项目。

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
## 代码风格原则

- 遵循现有文件的代码风格，不引入风格不一致的改动
- 修改 C++ 时：保持头文件的声明和实现文件的定义同步更新
- 新增功能时：参考同文件中类似功能的实现方式
- 不添加无实际意义的注释（如"// 读取文件"这类描述性注释）

对于需要用户确认的操作（write_file、edit_file、run_shell），用户会看到预览后决定是否执行。)";

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

// 将截断点退到最近的 UTF-8 字符边界，避免切断多字节字符（中文等）
// UTF-8 延续字节格式为 10xxxxxx（0x80~0xBF），前导字节不是延续字节
static size_t Utf8SafeTruncate(const std::string& s, size_t max_bytes) {
    if (max_bytes >= s.size()) return s.size();
    size_t pos = max_bytes;
    while (pos > 0 && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80)
        --pos;
    return pos;
}

// 截断字符串并附加压缩说明（UTF-8 安全）
static std::string TruncateWithNote(const std::string& s, size_t max_bytes) {
    size_t safe = Utf8SafeTruncate(s, max_bytes);
    return s.substr(0, safe) +
           "\n… [已压缩，原 " + std::to_string(s.size()) +
           " 字节，省略 " + std::to_string(s.size() - safe) + " 字节]";
}

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
            msg.content = TruncateWithNote(msg.content, old_tool_max_chars);
        }
    }

    // ── 阶段 2：丢弃最老的完整对话轮次 ──────────────────────────────────
    // 每次循环：找到第一个 user 消息之后的下一个 user 消息位置（含边界），
    // 把 [0, next_user) 整段丢弃（保留至少 keep_recent_turns 个轮次）
    while (EstimateTokens() > max_tokens) {
        size_t safe_keep = FindUserTurnBoundary(keep_recent_turns);
        if (safe_keep == 0 || safe_keep >= history_.size()) break;

        // 搜索范围包含 safe_keep 本身（它就是"第二个 user"的位置）
        size_t next_user = history_.size();
        for (size_t i = 1; i <= safe_keep && i < history_.size(); ++i) {
            if (history_[i].role == MessageRole::kUser) {
                next_user = i;
                break;
            }
        }
        // next_user > safe_keep 表示保护区以外找不到第二个 user，无法继续
        if (next_user > safe_keep) break;

        dropped += static_cast<int>(next_user);
        history_.erase(history_.begin(),
                        history_.begin() + static_cast<std::ptrdiff_t>(next_user));
    }

    // ── 阶段 3：如果保护区内的工具结果本身太大，适当截断 ─────────────────
    // 阶段 1/2 无法压缩保护区，若仍超限则对最近轮次的工具结果做更宽松的截断
    if (EstimateTokens() > max_tokens) {
        // 保护区内允许的单条工具输出上限：4 倍于旧轮次
        const size_t kRecentToolMax = old_tool_max_chars * 4;
        for (auto& msg : history_) {
            if (EstimateTokens() <= max_tokens) break;
            if (msg.role == MessageRole::kTool &&
                msg.content.size() > kRecentToolMax) {
                msg.content = TruncateWithNote(msg.content, kRecentToolMax);
                if (dropped == 0) ++compress_count_;
            }
        }
    }

    if (dropped > 0) ++compress_count_;

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
