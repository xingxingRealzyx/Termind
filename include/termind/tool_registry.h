#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace termind {

// ── 工具参数描述 ──────────────────────────────────────────────────────────
struct ToolParam {
    std::string name;
    std::string type;        // "string" | "integer" | "boolean" | "array" | "object"
    std::string description;
    bool required = true;
    std::vector<std::string> enum_values = {};  // 枚举限制（可选）

    // 便捷构造（避免聚合初始化警告）
    ToolParam(std::string n, std::string t, std::string d,
               bool req = true,
               std::vector<std::string> enums = {})
        : name(std::move(n)), type(std::move(t)), description(std::move(d)),
          required(req), enum_values(std::move(enums)) {}
};

// ── 工具定义（发给 AI 的元数据）──────────────────────────────────────────
struct ToolDefinition {
    std::string name;
    std::string description;
    std::vector<ToolParam> parameters;

    // 转换为 OpenAI function calling JSON 格式
    nlohmann::json ToJson() const;
};

// ── 工具执行结果 ──────────────────────────────────────────────────────────
struct ToolResult {
    bool success = false;
    std::string output;
};

// ── 工具函数签名 ──────────────────────────────────────────────────────────
using ToolFn = std::function<ToolResult(const nlohmann::json& args)>;

// ── 工具条目 ──────────────────────────────────────────────────────────────
struct Tool {
    ToolDefinition definition;
    ToolFn function;
    bool requires_confirmation = false;  // 执行前需用户确认
};

// ── 工具注册表（单例）────────────────────────────────────────────────────
class ToolRegistry {
public:
    static ToolRegistry& GetInstance();

    void Register(Tool tool);

    // 获取所有工具的 OpenAI JSON 定义列表
    std::vector<nlohmann::json> GetToolDefinitionsJson() const;

    // 执行工具（确认逻辑由 REPL 层负责）
    ToolResult Execute(const std::string& name, const nlohmann::json& args);

    bool HasTool(const std::string& name) const;
    bool RequiresConfirmation(const std::string& name) const;
    std::vector<std::string> GetToolNames() const;

private:
    ToolRegistry() = default;
    std::unordered_map<std::string, Tool> tools_;
};

// 注册所有内置工具
void RegisterBuiltinTools(ToolRegistry& registry,
                           const std::string& working_dir);

}  // namespace termind
