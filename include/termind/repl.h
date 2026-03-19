#pragma once

#include "ai_client.h"
#include "context_manager.h"
#include "tool_registry.h"

#include <memory>
#include <string>

namespace termind {

// ── REPL 主循环 ───────────────────────────────────────────────────────────
class Repl {
public:
    Repl();
    ~Repl();

    // 启动交互循环
    void Run();

private:
    // ── 输入 ──────────────────────────────────────────────────────────────
    void InitReadline();

    // ── Skills 初始化 ─────────────────────────────────────────────────────
    void InitSkills();
    std::string ReadLine(const std::string& prompt);

    // ── 处理输入 ──────────────────────────────────────────────────────────
    // 返回 false 表示退出
    bool ProcessInput(const std::string& input);

    // ── 斜杠命令处理 ──────────────────────────────────────────────────────
    bool DispatchCommand(const std::string& cmd, const std::string& args);
    void HandleHelp();
    void HandleClear();
    void HandleFile(const std::string& args);
    void HandleFiles();
    void HandleClearFiles();
    void HandleModel(const std::string& args);
    void HandleConfig();
    void HandleCd(const std::string& args);
    void HandlePwd();
    void HandleAdd(const std::string& args);
    void HandleTokens();
    void HandleSkills(const std::string& args);

    // ── AI 代理循环 ───────────────────────────────────────────────────────
    // 发送用户消息，循环处理工具调用，直到得到最终答复
    void RunAgentLoop(const std::string& user_message);

    // ── 工具执行（含确认流程）────────────────────────────────────────────
    ToolResult ExecuteToolWithConfirmation(const ToolCallRequest& tc);

    // 为 write_file 工具生成带 diff 的预览
    void ShowWriteFilePreview(const std::string& path_str,
                               const std::string& new_content);

    // 为 edit_file 工具生成预览
    void ShowEditFilePreview(const std::string& path_str,
                              const std::string& old_content,
                              const std::string& new_content);

    // ── 显示 ──────────────────────────────────────────────────────────────
    void PrintWelcome() const;
    std::string BuildPrompt() const;
    // 生成带颜色的上下文 token 徽章，供提示符和 AI 回答头部共用
    // 格式："42% 34k/80k "（启用压缩）或 "34k "（禁用）
    std::string BuildContextBadge() const;
    void PrintToolCallHeader(const ToolCallRequest& tc) const;

    // ── 状态 ──────────────────────────────────────────────────────────────
    std::unique_ptr<AiClient> ai_client_;
    ContextManager context_;
    bool running_ = true;
};

}  // namespace termind
