#pragma once

#include "ai_client.h"

#include <filesystem>
#include <string>
#include <vector>

namespace termind {

// ── 上下文中的文件记录 ────────────────────────────────────────────────────
struct FileContext {
    std::filesystem::path path;
    std::string content;
    std::string language;  // 检测到的编程语言（用于代码块标记）
};

// ── 上下文管理器 ──────────────────────────────────────────────────────────
// 维护完整的对话历史，以及附加的文件上下文。
class ContextManager {
public:
    explicit ContextManager();

    // ── 工作目录 ──────────────────────────────────────────────────────────
    void SetWorkingDir(const std::filesystem::path& dir);
    const std::filesystem::path& GetWorkingDir() const { return working_dir_; }

    // ── 系统消息 ──────────────────────────────────────────────────────────
    void SetSystemMessage(const std::string& content);
    const std::string& GetSystemMessage() const { return system_message_; }

    // ── 文件上下文 ────────────────────────────────────────────────────────
    bool AddFile(const std::filesystem::path& path);
    void RemoveFile(const std::filesystem::path& path);
    void ClearFiles();
    const std::vector<FileContext>& GetFiles() const { return file_contexts_; }

    // ── 对话历史 ──────────────────────────────────────────────────────────
    void AddUserMessage(const std::string& content);
    void AddAssistantMessage(const std::string& content);
    void AddAssistantToolCalls(const std::vector<ToolCallRequest>& calls,
                                const std::string& content = "");
    void AddToolResult(const std::string& tool_call_id,
                        const std::string& result);

    // 清除对话历史（保留文件上下文和系统消息）
    void ClearHistory();

    // 清除全部（含文件上下文）
    void ClearAll();

    // ── 构建发给 AI 的完整消息列表 ────────────────────────────────────────
    std::vector<Message> GetMessages() const;

    // 预估 token 数（按 4 字符 ≈ 1 token 粗算）
    size_t EstimateTokens() const;

    // ── 上下文压缩 ────────────────────────────────────────────────────────
    // 当 EstimateTokens() > max_tokens 时自动压缩：
    //   阶段 1：将 keep_recent_turns 之前的旧工具结果截断到 old_tool_max_chars
    //   阶段 2：逐步丢弃最老的完整对话轮次，直到低于 max_tokens
    // 返回实际丢弃的消息数量（0 = 未压缩）
    int TrimToFit(size_t max_tokens,
                   int keep_recent_turns = 2,
                   size_t old_tool_max_chars = 800);

    // 历史上累计触发压缩的次数
    int GetCompressCount() const { return compress_count_; }

    // 当前对话历史条数（不含系统消息和文件上下文）
    size_t GetHistorySize() const { return history_.size(); }

private:
    static std::string DetectLanguage(const std::filesystem::path& path);
    std::string BuildFilesBlock() const;

    // 在 history_ 中找到"第 n 个 user 消息"的下标（从后往前数第 n 个）
    // 返回 history_.size() 表示未找到
    size_t FindUserTurnBoundary(int n_from_end) const;

    std::string system_message_;
    std::vector<FileContext> file_contexts_;
    std::vector<Message> history_;
    std::filesystem::path working_dir_;
    int compress_count_ = 0;
};

}  // namespace termind
