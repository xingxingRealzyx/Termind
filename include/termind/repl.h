#pragma once

#include "ai_client.h"
#include "context_manager.h"
#include "tool_registry.h"
#include "utils.h"

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

    // ── 项目记忆 ──────────────────────────────────────────────────────────
    // 在工作目录（及上级直到 Git 根）查找 TERMIND.md 并加载
    void InitProjectMemory();
    // 从工作目录向上查找 TERMIND.md，返回找到的路径（失败返回空）
    std::filesystem::path FindProjectMemoryPath() const;
    // 重建 system message：base_system_prompt + skills 摘要 + 项目记忆 + 构建信息
    void RebuildSystemMessage();
    // 注册 update_project_memory 工具（捕获 this，需在每次 InitProjectMemory 后调用）
    void RegisterMemoryTool();

    // ── 构建系统探测 ──────────────────────────────────────────────────────
    // 检测工作目录下的构建文件，返回可注入系统消息的描述字符串（空=未探测到）
    std::string DetectBuildSystem() const;
    std::string ReadLine(const std::string& prompt);

    // ── 处理输入 ──────────────────────────────────────────────────────────
    // 返回 false 表示退出
    bool ProcessInput(const std::string& input);

    // ── 斜杠命令处理 ──────────────────────────────────────────────────────
    bool DispatchCommand(const std::string& cmd, const std::string& args);
    void HandleHelp();
    void HandleClear();
    void HandlePlan(const std::string& args);
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
    void HandleMemory(const std::string& args);

    // ── AI 代理循环 ───────────────────────────────────────────────────────
    // 发送用户消息，循环处理工具调用，直到得到最终答复
    // 若提供 panel，则在每轮迭代间渲染任务进度，并从 AI 输出中解析 [[DONE:N]] 标记
    void RunAgentLoop(const std::string& user_message,
                      utils::TaskPanel* panel = nullptr);

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
    bool running_        = true;
    bool interrupt_loop_ = false;  // 用户在工具确认时选择中断整个任务

    // system message 的各层内容（用于重建）
    std::string base_system_prompt_;     // 来自 config 或默认值
    std::string project_memory_content_; // TERMIND.md 正文
    std::filesystem::path project_memory_path_; // TERMIND.md 路径（空=未找到）
    std::string build_info_;             // 探测到的构建系统描述
};

}  // namespace termind
