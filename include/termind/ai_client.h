#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace termind {

// ── 消息角色 ──────────────────────────────────────────────────────────────
enum class MessageRole {
    kSystem,
    kUser,
    kAssistant,
    kTool,
};

std::string RoleToString(MessageRole role);

// ── AI 工具调用请求 ───────────────────────────────────────────────────────
struct ToolCallRequest {
    std::string id;
    std::string name;
    nlohmann::json arguments;  // 已解析为 JSON 对象
};

// ── 对话消息 ──────────────────────────────────────────────────────────────
struct Message {
    MessageRole role;
    std::string content;
    std::optional<std::string> tool_call_id;                    // role == kTool 时
    std::optional<std::vector<ToolCallRequest>> tool_calls;     // role == kAssistant 且含工具调用时

    nlohmann::json ToJson() const;

    // 工厂方法
    static Message System(const std::string& content);
    static Message User(const std::string& content);
    static Message Assistant(const std::string& content);
    static Message AssistantWithCalls(const std::vector<ToolCallRequest>& calls,
                                       const std::string& content = "");
    static Message Tool(const std::string& tool_call_id,
                         const std::string& content);
};

// ── AI 响应 ───────────────────────────────────────────────────────────────
struct ChatResponse {
    bool success = false;
    std::string error_message  = {};
    std::string content        = {};
    std::vector<ToolCallRequest> tool_calls = {};
    std::string finish_reason  = {};

    bool HasToolCalls() const { return !tool_calls.empty(); }
};

// 文字流回调：每个文字增量 chunk
using TextChunkCallback = std::function<void(const std::string& chunk)>;

// 工具参数流回调：AI 正在构建工具调用时，每个参数增量 chunk 触发
// tool_name 可能在首个 chunk 后才非空（OpenAI 流协议）
using ToolArgChunkCallback =
    std::function<void(const std::string& tool_name,
                       const std::string& arg_chunk)>;

// ── AI 客户端 ─────────────────────────────────────────────────────────────
class AiClient {
public:
    AiClient();

    // 非流式请求（工具调用中间轮次使用）
    ChatResponse Chat(const std::vector<Message>& messages,
                       const std::vector<nlohmann::json>& tools = {});

    // 流式请求：文字块和工具参数块都通过回调实时推送
    ChatResponse ChatStream(const std::vector<Message>& messages,
                             const std::vector<nlohmann::json>& tools = {},
                             TextChunkCallback    on_text_chunk     = nullptr,
                             ToolArgChunkCallback on_tool_arg_chunk = nullptr);

private:
    // SSE 流式解析中间状态
    struct PartialToolCall {
        std::string id;
        std::string name;
        std::string raw_arguments;  // 累积中的 JSON 字符串
    };

    struct StreamContext {
        std::string buffer;
        std::string accumulated_content;
        std::vector<PartialToolCall> tool_calls;
        std::string finish_reason;
        bool finished = false;
        TextChunkCallback    text_callback;
        ToolArgChunkCallback tool_arg_callback;
    };

    static size_t WriteCallback(char* ptr, size_t size, size_t nmemb,
                                 void* userdata);
    static size_t StreamWriteCallback(char* ptr, size_t size, size_t nmemb,
                                       void* userdata);
    static void ProcessStreamChunk(const nlohmann::json& chunk,
                                    StreamContext* ctx);

    ChatResponse ParseResponse(const std::string& body);
    ChatResponse DoRequest(const std::string& payload, bool use_stream,
                            TextChunkCallback    on_text_chunk,
                            ToolArgChunkCallback on_tool_arg_chunk = nullptr);
    std::string BuildPayload(const std::vector<Message>& messages,
                              const std::vector<nlohmann::json>& tools,
                              bool stream) const;

    std::string api_key_;
    std::string api_base_url_;
    std::string model_;
    int max_tokens_;
    double temperature_;
};

}  // namespace termind
