#include "termind/ai_client.h"
#include "termind/config.h"
#include "termind/utils.h"

#include <iostream>
#include <sstream>

#include <curl/curl.h>

namespace termind {

// ── RoleToString ──────────────────────────────────────────────────────────

std::string RoleToString(MessageRole role) {
    switch (role) {
        case MessageRole::kSystem:    return "system";
        case MessageRole::kUser:      return "user";
        case MessageRole::kAssistant: return "assistant";
        case MessageRole::kTool:      return "tool";
    }
    return "user";
}

// ── Message::ToJson ───────────────────────────────────────────────────────

nlohmann::json Message::ToJson() const {
    nlohmann::json j;
    j["role"] = RoleToString(role);

    if (role == MessageRole::kTool) {
        j["content"] = content;
        if (tool_call_id) j["tool_call_id"] = *tool_call_id;
        return j;
    }

    if (role == MessageRole::kAssistant && tool_calls && !tool_calls->empty()) {
        j["content"] = content.empty() ? nlohmann::json(nullptr) : nlohmann::json(content);
        nlohmann::json tc_arr = nlohmann::json::array();
        for (const auto& tc : *tool_calls) {
            tc_arr.push_back({
                {"id",   tc.id},
                {"type", "function"},
                {"function", {
                    {"name",      tc.name},
                    {"arguments", tc.arguments.dump()}
                }}
            });
        }
        j["tool_calls"] = tc_arr;
        return j;
    }

    j["content"] = content;
    return j;
}

// ── Message 工厂方法 ──────────────────────────────────────────────────────

Message Message::System(const std::string& content) {
    return {MessageRole::kSystem, content, std::nullopt, std::nullopt};
}

Message Message::User(const std::string& content) {
    return {MessageRole::kUser, content, std::nullopt, std::nullopt};
}

Message Message::Assistant(const std::string& content) {
    return {MessageRole::kAssistant, content, std::nullopt, std::nullopt};
}

Message Message::AssistantWithCalls(const std::vector<ToolCallRequest>& calls,
                                     const std::string& content) {
    return {MessageRole::kAssistant, content, std::nullopt, calls};
}

Message Message::Tool(const std::string& tool_call_id,
                       const std::string& content) {
    return {MessageRole::kTool, content, tool_call_id, std::nullopt};
}

// ── AiClient ──────────────────────────────────────────────────────────────

AiClient::AiClient() {
    const auto& cfg = ConfigManager::GetInstance().config();
    api_key_      = cfg.api_key;
    api_base_url_ = cfg.api_base_url;
    model_        = cfg.model;
    max_tokens_   = cfg.max_tokens;
    temperature_  = cfg.temperature;
}

// ── CURL 写回调（非流式）─────────────────────────────────────────────────

size_t AiClient::WriteCallback(char* ptr, size_t size, size_t nmemb,
                                 void* userdata) {
    auto* body = static_cast<std::string*>(userdata);
    body->append(ptr, size * nmemb);
    return size * nmemb;
}

// ── CURL 写回调（流式 SSE）───────────────────────────────────────────────

size_t AiClient::StreamWriteCallback(char* ptr, size_t size, size_t nmemb,
                                       void* userdata) {
    auto* ctx = static_cast<StreamContext*>(userdata);
    ctx->buffer.append(ptr, size * nmemb);

    // 逐行处理完整的 SSE 事件
    std::string& buf = ctx->buffer;
    size_t pos = 0;

    while (true) {
        size_t nl = buf.find('\n', pos);
        if (nl == std::string::npos) break;

        std::string line = buf.substr(pos, nl - pos);
        pos = nl + 1;

        // 去掉末尾 \r
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (utils::StartsWith(line, "data: ")) {
            std::string data = line.substr(6);
            if (data == "[DONE]") {
                ctx->finished = true;
                buf.erase(0, pos);
                // 返回 0 让 curl 结束
                return 0;
            }
            try {
                auto chunk = nlohmann::json::parse(data);
                ProcessStreamChunk(chunk, ctx);
            } catch (...) {
                // 忽略解析错误
            }
        } else if (!line.empty()) {
            // 非 SSE 格式的行（通常是错误响应体）
            ctx->error_body += line + "\n";
        }
    }

    buf.erase(0, pos);
    return size * nmemb;
}

// ── 处理单个 SSE 数据块 ───────────────────────────────────────────────────

void AiClient::ProcessStreamChunk(const nlohmann::json& chunk,
                                    StreamContext* ctx) {
    if (!chunk.contains("choices") || chunk["choices"].empty()) return;

    const auto& choice = chunk["choices"][0];

    if (choice.contains("finish_reason") &&
        !choice["finish_reason"].is_null()) {
        ctx->finish_reason = choice["finish_reason"].get<std::string>();
    }

    if (!choice.contains("delta")) return;
    const auto& delta = choice["delta"];

    // 文字内容
    if (delta.contains("content") && !delta["content"].is_null()) {
        std::string text = delta["content"].get<std::string>();
        ctx->accumulated_content += text;
        if (ctx->text_callback) ctx->text_callback(text);
    }

    // 工具调用（增量累积）
    if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
        for (const auto& tc : delta["tool_calls"]) {
            int idx = tc.value("index", 0);
            if (idx >= static_cast<int>(ctx->tool_calls.size())) {
                ctx->tool_calls.resize(idx + 1);
            }
            auto& ptc = ctx->tool_calls[idx];
            if (tc.contains("id")   && tc["id"].is_string())
                ptc.id = tc["id"].get<std::string>();
            if (tc.contains("function")) {
                const auto& fn = tc["function"];
                if (fn.contains("name") && fn["name"].is_string())
                    ptc.name = fn["name"].get<std::string>();
                if (fn.contains("arguments") && fn["arguments"].is_string()) {
                    std::string arg_chunk = fn["arguments"].get<std::string>();
                    ptc.raw_arguments += arg_chunk;
                    // 触发工具参数流回调（供 UI 显示进度）
                    if (ctx->tool_arg_callback && !arg_chunk.empty()) {
                        ctx->tool_arg_callback(ptc.name, arg_chunk);
                    }
                }
            }
        }
    }
}

// ── 解析非流式响应体 ──────────────────────────────────────────────────────

ChatResponse AiClient::ParseResponse(const std::string& body) {
    try {
        auto j = nlohmann::json::parse(body);

        // 错误响应
        if (j.contains("error")) {
            std::string msg  = j["error"].value("message", body.substr(0, 400));
            std::string code = j["error"].value("code", "");
            if (!code.empty()) msg = "[" + code + "] " + msg;
            return {false, msg};
        }

        ChatResponse resp;
        resp.success = true;

        if (!j.contains("choices") || j["choices"].empty())
            return {false, "响应中没有 choices 字段，原始响应: " + body.substr(0, 400)};

        const auto& choice = j["choices"][0];
        resp.finish_reason = choice.value("finish_reason", "");

        const auto& msg = choice["message"];
        if (msg.contains("content") && !msg["content"].is_null())
            resp.content = msg["content"].get<std::string>();

        if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
            for (const auto& tc : msg["tool_calls"]) {
                ToolCallRequest req;
                req.id   = tc.value("id", "");
                req.name = tc["function"].value("name", "");
                std::string raw_args = tc["function"].value("arguments", "{}");
                try {
                    req.arguments = nlohmann::json::parse(raw_args);
                } catch (...) {
                    req.arguments = nlohmann::json::object();
                }
                resp.tool_calls.push_back(std::move(req));
            }
        }

        return resp;
    } catch (const std::exception& e) {
        return {false, std::string("解析响应失败: ") + e.what() +
                       "\n原始响应: " + body.substr(0, 500)};
    }
}

// ── 构建请求 payload ──────────────────────────────────────────────────────

std::string AiClient::BuildPayload(const std::vector<Message>& messages,
                                    const std::vector<nlohmann::json>& tools,
                                    bool stream) const {
    nlohmann::json j;
    j["model"]       = model_;
    j["max_tokens"]  = max_tokens_;
    j["temperature"] = temperature_;
    j["stream"]      = stream;

    nlohmann::json msgs = nlohmann::json::array();
    for (const auto& m : messages) msgs.push_back(m.ToJson());
    j["messages"] = msgs;

    if (!tools.empty()) {
        j["tools"] = tools;
        j["tool_choice"] = "auto";
    }

    return j.dump();
}

// ── 执行 HTTP 请求 ────────────────────────────────────────────────────────

ChatResponse AiClient::DoRequest(const std::string& payload, bool use_stream,
                                   TextChunkCallback    on_text_chunk,
                                   ToolArgChunkCallback on_tool_arg_chunk) {
    CURL* curl = curl_easy_init();
    if (!curl) return {false, "初始化 CURL 失败"};

    std::string url = api_base_url_ + "/chat/completions";

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    std::string auth_hdr = "Authorization: Bearer " + api_key_;
    headers = curl_slist_append(headers, auth_hdr.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    // 支持 HTTPS
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    ChatResponse response;

    if (use_stream) {
        StreamContext ctx;
        ctx.text_callback     = on_text_chunk;
        ctx.tool_arg_callback = on_tool_arg_chunk;
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, StreamWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

        CURLcode res = curl_easy_perform(curl);

        // CURLE_WRITE_ERROR 是我们主动返回 0 触发的，属于正常结束
        if (res != CURLE_OK && res != CURLE_WRITE_ERROR) {
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            return {false, std::string("网络请求失败: ") + curl_easy_strerror(res)};
        }

        // 检查 HTTP 状态码
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (http_code != 200) {
            // 优先用 SSE 内容，其次用非 SSE 错误体，最后仅报状态码
            std::string detail;
            if (!ctx.accumulated_content.empty())
                detail = ctx.accumulated_content;
            else if (!ctx.error_body.empty())
                detail = ctx.error_body.substr(0, 800);
            else
                detail = "(无错误详情)";
            return {false, "HTTP " + std::to_string(http_code) + ": " + detail};
        }

        response.success = true;
        response.content = ctx.accumulated_content;
        response.finish_reason = ctx.finish_reason;

        // 整理 partial tool calls → ToolCallRequest
        for (auto& ptc : ctx.tool_calls) {
            if (ptc.id.empty() || ptc.name.empty()) continue;
            ToolCallRequest tc;
            tc.id   = ptc.id;
            tc.name = ptc.name;
            try {
                tc.arguments = nlohmann::json::parse(ptc.raw_arguments);
            } catch (...) {
                tc.arguments = nlohmann::json::object();
            }
            response.tool_calls.push_back(std::move(tc));
        }

    } else {
        std::string body;
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);

        CURLcode res = curl_easy_perform(curl);

        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK)
            return {false, std::string("网络请求失败: ") + curl_easy_strerror(res)};

        response = ParseResponse(body);
    }

    return response;
}

// ── 公共接口 ──────────────────────────────────────────────────────────────

ChatResponse AiClient::Chat(const std::vector<Message>& messages,
                              const std::vector<nlohmann::json>& tools) {
    if (api_key_.empty()) {
        return {false, "未设置 API Key。请设置环境变量 TERMIND_API_KEY 或 OPENAI_API_KEY，"
                       "或在 ~/.config/termind/config.json 中配置。"};
    }
    std::string payload = BuildPayload(messages, tools, false);
    return DoRequest(payload, false, nullptr);
}

ChatResponse AiClient::ChatStream(const std::vector<Message>& messages,
                                    const std::vector<nlohmann::json>& tools,
                                    TextChunkCallback    on_text_chunk,
                                    ToolArgChunkCallback on_tool_arg_chunk) {
    if (api_key_.empty()) {
        return {false, "未设置 API Key。请设置环境变量 TERMIND_API_KEY 或 OPENAI_API_KEY，"
                       "或在 ~/.config/termind/config.json 中配置。"};
    }
    std::string payload = BuildPayload(messages, tools, true);
    return DoRequest(payload, true, on_text_chunk, on_tool_arg_chunk);
}

}  // namespace termind
