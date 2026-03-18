#include "termind/config.h"
#include "termind/utils.h"

#include <filesystem>
#include <fstream>
#include <iostream>

#include <nlohmann/json.hpp>

namespace termind {

namespace fs = std::filesystem;

// ── 单例 ──────────────────────────────────────────────────────────────────
ConfigManager& ConfigManager::GetInstance() {
    static ConfigManager instance;
    return instance;
}

ConfigManager::ConfigManager() {
    LoadFromEnvironment();
}

// ── 默认路径 ──────────────────────────────────────────────────────────────
std::string ConfigManager::GetDefaultConfigDir() {
    return utils::ExpandHome("~/.config/termind");
}

std::string ConfigManager::GetDefaultConfigPath() {
    return GetDefaultConfigDir() + "/config.json";
}

// ── 从文件加载 ────────────────────────────────────────────────────────────
bool ConfigManager::LoadFromFile(const std::string& path) {
    std::string config_path = path.empty() ? GetDefaultConfigPath() : path;

    std::ifstream f(config_path);
    if (!f.is_open()) {
        return false;
    }

    try {
        nlohmann::json j = nlohmann::json::parse(f);

        auto get_str = [&](const char* key, std::string& val) {
            if (j.contains(key) && j[key].is_string()) val = j[key].get<std::string>();
        };
        auto get_int = [&](const char* key, int& val) {
            if (j.contains(key) && j[key].is_number_integer()) val = j[key].get<int>();
        };
        auto get_dbl = [&](const char* key, double& val) {
            if (j.contains(key) && j[key].is_number()) val = j[key].get<double>();
        };
        auto get_bool = [&](const char* key, bool& val) {
            if (j.contains(key) && j[key].is_boolean()) val = j[key].get<bool>();
        };

        get_str("api_key",             config_.api_key);
        get_str("api_base_url",        config_.api_base_url);
        get_str("model",               config_.model);
        get_int("max_tokens",          config_.max_tokens);
        get_dbl("temperature",         config_.temperature);
        get_bool("stream",             config_.stream);
        get_bool("auto_approve_reads", config_.auto_approve_reads);
        get_int("max_tool_iterations", config_.max_tool_iterations);
        get_str("system_prompt",       config_.system_prompt);
        get_int("max_context_tokens",  config_.max_context_tokens);

        return true;
    } catch (const std::exception& e) {
        std::cerr << utils::color::kRed << "配置文件解析失败: "
                  << e.what() << utils::color::kReset << "\n";
        return false;
    }
}

// ── 从环境变量加载 ────────────────────────────────────────────────────────
void ConfigManager::LoadFromEnvironment() {
    // API key：优先级 TERMIND_API_KEY > OPENAI_API_KEY
    for (const char* var : {"TERMIND_API_KEY", "OPENAI_API_KEY", "ANTHROPIC_API_KEY"}) {
        std::string val = utils::GetEnv(var);
        if (!val.empty()) {
            config_.api_key = val;
            break;
        }
    }

    if (std::string val = utils::GetEnv("TERMIND_API_BASE_URL"); !val.empty())
        config_.api_base_url = val;
    else if (std::string v2 = utils::GetEnv("OPENAI_API_BASE"); !v2.empty())
        config_.api_base_url = v2;

    if (std::string val = utils::GetEnv("TERMIND_MODEL"); !val.empty())
        config_.model = val;
}

// ── 保存到文件 ────────────────────────────────────────────────────────────
bool ConfigManager::SaveToFile(const std::string& path) const {
    std::string config_path = path.empty() ? GetDefaultConfigPath() : path;

    fs::path dir = fs::path(config_path).parent_path();
    if (!dir.empty()) {
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) {
            std::cerr << "创建配置目录失败: " << ec.message() << "\n";
            return false;
        }
    }

    nlohmann::json j;
    j["api_key"]             = config_.api_key;
    j["api_base_url"]        = config_.api_base_url;
    j["model"]               = config_.model;
    j["max_tokens"]          = config_.max_tokens;
    j["temperature"]         = config_.temperature;
    j["stream"]              = config_.stream;
    j["auto_approve_reads"]  = config_.auto_approve_reads;
    j["max_tool_iterations"] = config_.max_tool_iterations;
    j["system_prompt"]       = config_.system_prompt;
    j["max_context_tokens"]  = config_.max_context_tokens;

    std::ofstream f(config_path);
    if (!f.is_open()) return false;
    f << j.dump(4) << "\n";
    return f.good();
}

}  // namespace termind
