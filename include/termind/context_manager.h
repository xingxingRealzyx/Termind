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

private:
    static std::string DetectLanguage(const std::filesystem::path& path);
    std::string BuildFilesBlock() const;

    std::string system_message_;
    std::vector<FileContext> file_contexts_;
    std::vector<Message> history_;
    std::filesystem::path working_dir_;
};

}  // namespace termind
