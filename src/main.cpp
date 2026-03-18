#include "termind/config.h"
#include "termind/repl.h"
#include "termind/utils.h"

#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>

#include <curl/curl.h>

namespace {

// 全局退出标志（供信号处理使用）
volatile sig_atomic_t g_interrupt = 0;

void SignalHandler(int /*sig*/) {
    g_interrupt = 1;
}

void PrintUsage(const char* prog) {
    std::cout << "用法: " << prog << " [选项]\n\n"
              << "选项:\n"
              << "  -c, --config <路径>   指定配置文件路径\n"
              << "  -m, --model  <名称>   覆盖模型名称\n"
              << "  -d, --dir    <路径>   设置工作目录\n"
              << "  --no-stream          禁用流式输出\n"
              << "  -h, --help           显示此帮助\n"
              << "  -v, --version        显示版本\n\n"
              << "环境变量:\n"
              << "  TERMIND_API_KEY      API 密钥\n"
              << "  TERMIND_API_BASE_URL API 基础地址（默认 OpenAI）\n"
              << "  TERMIND_MODEL        默认模型\n\n"
              << "配置文件: ~/.config/termind/config.json\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    // ── 信号处理 ──────────────────────────────────────────────────────────
    std::signal(SIGINT,  SignalHandler);
    std::signal(SIGTERM, SignalHandler);
    std::signal(SIGPIPE, SIG_IGN);  // 防止管道断开崩溃

    // ── 初始化 CURL ───────────────────────────────────────────────────────
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        std::cerr << "CURL 初始化失败\n";
        return 1;
    }

    // ── 加载默认配置 ──────────────────────────────────────────────────────
    auto& config_mgr = termind::ConfigManager::GetInstance();
    config_mgr.LoadFromFile();  // 如果文件不存在则静默失败

    // ── 解析命令行参数 ────────────────────────────────────────────────────
    std::string config_path;
    std::string work_dir;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            PrintUsage(argv[0]);
            curl_global_cleanup();
            return 0;
        }

        if (arg == "-v" || arg == "--version") {
            std::cout << "Termind (端脑) v0.1.0\n";
            curl_global_cleanup();
            return 0;
        }

        if ((arg == "-c" || arg == "--config") && i + 1 < argc) {
            config_path = argv[++i];
            config_mgr.LoadFromFile(config_path);
            continue;
        }

        if ((arg == "-m" || arg == "--model") && i + 1 < argc) {
            config_mgr.mutable_config().model = argv[++i];
            continue;
        }

        if ((arg == "-d" || arg == "--dir") && i + 1 < argc) {
            work_dir = argv[++i];
            continue;
        }

        if (arg == "--no-stream") {
            config_mgr.mutable_config().stream = false;
            continue;
        }

        if (arg == "--approve-all") {
            config_mgr.mutable_config().auto_approve_reads = true;
            continue;
        }

        std::cerr << "未知选项: " << arg
                  << "  使用 --help 查看帮助\n";
        curl_global_cleanup();
        return 1;
    }

    // ── 设置工作目录 ──────────────────────────────────────────────────────
    if (!work_dir.empty()) {
        std::error_code ec;
        std::filesystem::path wd = std::filesystem::canonical(work_dir, ec);
        if (!ec && std::filesystem::is_directory(wd)) {
            std::filesystem::current_path(wd, ec);
        } else {
            std::cerr << "无效工作目录: " << work_dir << "\n";
            curl_global_cleanup();
            return 1;
        }
    }

    // ── 首次运行：若无配置文件则提示 ─────────────────────────────────────
    if (config_mgr.config().api_key.empty()) {
        const auto& cfg_path = termind::ConfigManager::GetDefaultConfigPath();
        if (!std::filesystem::exists(cfg_path)) {
            // 仅在交互模式下提示
            termind::utils::PrintInfo(
                "提示：可创建 " + cfg_path + " 来持久化配置。");
            termind::utils::PrintInfo(
                "或设置 TERMIND_API_KEY 环境变量来提供 API Key。");
        }
    }

    // ── 启动 REPL ─────────────────────────────────────────────────────────
    {
        termind::Repl repl;
        repl.Run();
    }

    // ── 清理 ──────────────────────────────────────────────────────────────
    curl_global_cleanup();
    return 0;
}
