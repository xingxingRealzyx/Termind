#pragma once

#include <string>
#include <vector>

namespace termind {

// ── 运行时配置 ─────────────────────────────────────────────────────────────
struct Config {
    // AI API
    std::string api_key;
    std::string api_base_url = "https://api.openai.com/v1";
    std::string model = "gpt-4o";
    int max_tokens = 8192;
    double temperature = 0.7;
    bool stream = true;

    // 行为
    bool auto_approve_reads = true;   // 读操作无需确认
    int max_tool_iterations = 50;     // 最多工具调用循环次数

    // 上下文压缩
    // 超过此 token 估算值时触发自动压缩（0 = 禁用）
    // 压缩策略：先截断旧工具结果 → 再丢弃最老的完整对话轮次
    int max_context_tokens = 200000;

    // 系统提示词（可由配置文件覆盖）
    std::string system_prompt;

    // Skills 扫描目录列表（空时使用默认路径 ~/.config/termind/skills）
    // 每项可以是绝对路径或 ~ 开头的路径
    std::vector<std::string> skills_dirs;
};

// ── 配置管理器（单例）──────────────────────────────────────────────────────
class ConfigManager {
public:
    static ConfigManager& GetInstance();

    // 从文件加载配置；path 为空则使用默认路径 ~/.config/termind/config.json
    bool LoadFromFile(const std::string& path = "");

    // 从环境变量加载（TERMIND_API_KEY / OPENAI_API_KEY / TERMIND_MODEL 等）
    void LoadFromEnvironment();

    // 保存配置到文件
    bool SaveToFile(const std::string& path = "") const;

    const Config& config() const { return config_; }
    Config& mutable_config() { return config_; }

    static std::string GetDefaultConfigPath();
    static std::string GetDefaultConfigDir();

private:
    ConfigManager();
    Config config_;
};

}  // namespace termind
