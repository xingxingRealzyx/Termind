#include "termind/repl.h"
#include "termind/config.h"
#include "termind/utils.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sstream>

#include <readline/history.h>
#include <readline/readline.h>
#include <unistd.h>

namespace termind {

namespace fs = std::filesystem;

// ── 构造 / 析构 ───────────────────────────────────────────────────────────

Repl::Repl() : ai_client_(std::make_unique<AiClient>()) {
    // 工作目录默认为当前目录
    context_.SetWorkingDir(fs::current_path());

    // 若配置文件有自定义系统提示词则覆盖默认
    const auto& cfg = ConfigManager::GetInstance().config();
    if (!cfg.system_prompt.empty()) {
        context_.SetSystemMessage(cfg.system_prompt);
    }

    // 注册内置工具
    RegisterBuiltinTools(ToolRegistry::GetInstance(),
                         context_.GetWorkingDir().string());

    InitReadline();
}

Repl::~Repl() {
    // 保存 readline 历史
    std::string hist_path =
        utils::ExpandHome("~/.config/termind/history");
    write_history(hist_path.c_str());
}

// ── readline 初始化 ───────────────────────────────────────────────────────

void Repl::InitReadline() {
    rl_readline_name = "termind";

    // 加载历史文件
    std::string hist_dir = utils::ExpandHome("~/.config/termind");
    std::string hist_path = hist_dir + "/history";
    fs::create_directories(hist_dir);
    read_history(hist_path.c_str());
    stifle_history(1000);
}

// ── 读取一行输入 ──────────────────────────────────────────────────────────

std::string Repl::ReadLine(const std::string& prompt) {
    char* line = readline(prompt.c_str());
    if (!line) {
        // EOF（Ctrl-D）
        running_ = false;
        return "";
    }
    std::string result(line);
    free(line);

    result = utils::Trim(result);
    if (!result.empty()) add_history(result.c_str());
    return result;
}

// ── 启动 REPL ─────────────────────────────────────────────────────────────

void Repl::Run() {
    PrintWelcome();

    while (running_) {
        std::string input = ReadLine(BuildPrompt());
        if (!running_) break;
        if (input.empty()) continue;

        if (!ProcessInput(input)) break;
    }

    std::cout << "\n" << utils::color::kDim
              << "再见！" << utils::color::kReset << "\n";
}

// ── 输入分发 ──────────────────────────────────────────────────────────────

bool Repl::ProcessInput(const std::string& input) {
    if (input[0] == '/') {
        // 解析命令和参数
        size_t space = input.find(' ');
        std::string cmd  = (space == std::string::npos)
                            ? input.substr(1)
                            : input.substr(1, space - 1);
        std::string args = (space == std::string::npos)
                            ? ""
                            : utils::Trim(input.substr(space + 1));

        std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);
        return DispatchCommand(cmd, args);
    }

    // 普通查询
    RunAgentLoop(input);
    return true;
}

// ── 斜杠命令分发 ──────────────────────────────────────────────────────────

bool Repl::DispatchCommand(const std::string& cmd, const std::string& args) {
    if (cmd == "quit" || cmd == "exit" || cmd == "q") {
        running_ = false;
        return false;
    }
    if (cmd == "help" || cmd == "h" || cmd == "?") { HandleHelp();          return true; }
    if (cmd == "clear" || cmd == "c")               { HandleClear();         return true; }
    if (cmd == "file"  || cmd == "f")               { HandleFile(args);      return true; }
    if (cmd == "files")                             { HandleFiles();         return true; }
    if (cmd == "clearfiles" || cmd == "cf")         { HandleClearFiles();    return true; }
    if (cmd == "model" || cmd == "m")               { HandleModel(args);     return true; }
    if (cmd == "config")                            { HandleConfig();        return true; }
    if (cmd == "cd")                                { HandleCd(args);        return true; }
    if (cmd == "pwd")                               { HandlePwd();           return true; }
    if (cmd == "add"   || cmd == "a")               { HandleAdd(args);       return true; }
    if (cmd == "tokens")                            { HandleTokens();        return true; }

    utils::PrintWarning("未知命令: /" + cmd + "。输入 /help 查看帮助。");
    return true;
}

// ── 斜杠命令实现 ──────────────────────────────────────────────────────────

void Repl::HandleHelp() {
    std::cout << "\n"
              << utils::color::kBold << utils::color::kCyan
              << "Termind 命令列表" << utils::color::kReset << "\n\n"
              << utils::color::kYellow << "对话命令\n" << utils::color::kReset
              << "  直接输入问题或指令即可与 AI 对话\n\n"
              << utils::color::kYellow << "文件操作\n" << utils::color::kReset
              << "  /file <路径>     将文件加入 AI 上下文\n"
              << "  /files           列出当前上下文中的文件\n"
              << "  /clearfiles      清除文件上下文\n"
              << "  /add <内容>      直接附加文字片段到上下文\n\n"
              << utils::color::kYellow << "会话管理\n" << utils::color::kReset
              << "  /clear           清除对话历史\n"
              << "  /tokens          显示预估的 token 用量\n\n"
              << utils::color::kYellow << "配置\n" << utils::color::kReset
              << "  /model <名称>    切换 AI 模型\n"
              << "  /config          查看当前配置\n\n"
              << utils::color::kYellow << "目录\n" << utils::color::kReset
              << "  /cd <路径>       切换工作目录\n"
              << "  /pwd             显示当前工作目录\n\n"
              << utils::color::kYellow << "其他\n" << utils::color::kReset
              << "  /help            显示此帮助\n"
              << "  /quit            退出\n\n";
}

void Repl::HandleClear() {
    context_.ClearHistory();
    utils::PrintSuccess("对话历史已清除");
}

void Repl::HandleFile(const std::string& args) {
    if (args.empty()) {
        utils::PrintWarning("用法: /file <文件路径>");
        return;
    }

    fs::path path = args[0] == '/' ? fs::path(args)
                                    : context_.GetWorkingDir() / args;
    path = fs::weakly_canonical(path);

    if (!fs::exists(path)) {
        utils::PrintError("文件不存在: " + path.string());
        return;
    }

    if (context_.AddFile(path)) {
        std::string rel = utils::GetRelativePath(path, context_.GetWorkingDir());
        utils::PrintSuccess("已添加文件到上下文: " + rel);
    } else {
        utils::PrintError("无法读取文件: " + path.string());
    }
}

void Repl::HandleFiles() {
    const auto& files = context_.GetFiles();
    if (files.empty()) {
        utils::PrintInfo("上下文中没有文件。使用 /file <路径> 添加。");
        return;
    }
    std::cout << utils::color::kCyan << "上下文文件 (" << files.size()
              << " 个):" << utils::color::kReset << "\n";
    for (const auto& fc : files) {
        std::string rel = utils::GetRelativePath(fc.path,
                                                  context_.GetWorkingDir());
        std::cout << "  📄 " << rel << "  "
                  << utils::color::kDim
                  << utils::FormatFileSize(fc.content.size())
                  << utils::color::kReset << "\n";
    }
}

void Repl::HandleClearFiles() {
    context_.ClearFiles();
    utils::PrintSuccess("文件上下文已清除");
}

void Repl::HandleModel(const std::string& args) {
    if (args.empty()) {
        utils::PrintInfo("当前模型: " +
                          ConfigManager::GetInstance().config().model);
        return;
    }
    ConfigManager::GetInstance().mutable_config().model = args;
    // 重新创建 AiClient 以应用新模型
    ai_client_ = std::make_unique<AiClient>();
    utils::PrintSuccess("已切换模型: " + args);
}

void Repl::HandleConfig() {
    const auto& cfg = ConfigManager::GetInstance().config();
    std::cout << "\n"
              << utils::color::kBold << "当前配置" << utils::color::kReset
              << "\n"
              << "  模型:          " << cfg.model << "\n"
              << "  API Base:      " << cfg.api_base_url << "\n"
              << "  Max Tokens:    " << cfg.max_tokens << "\n"
              << "  Temperature:   " << cfg.temperature << "\n"
              << "  Stream:        " << (cfg.stream ? "是" : "否") << "\n"
              << "   工具调用上限:  " << cfg.max_tool_iterations << "\n"
              << "  API Key:       "
              << (cfg.api_key.empty() ? utils::color::kRed + std::string("未设置") + utils::color::kReset
                                       : std::string("***") +
                                         cfg.api_key.substr(
                                             std::max(0, static_cast<int>(cfg.api_key.size()) - 4)))
              << "\n\n";
}

void Repl::HandleCd(const std::string& args) {
    if (args.empty()) {
        utils::PrintWarning("用法: /cd <路径>");
        return;
    }

    fs::path new_dir;
    if (args == "~") {
        new_dir = utils::ExpandHome("~");
    } else if (args[0] == '/') {
        new_dir = args;
    } else {
        new_dir = context_.GetWorkingDir() / args;
    }

    std::error_code ec;
    new_dir = fs::canonical(new_dir, ec);
    if (ec || !fs::is_directory(new_dir)) {
        utils::PrintError("目录不存在: " + args);
        return;
    }

    context_.SetWorkingDir(new_dir);
    // 重新注册内置工具（Register 会覆盖同名工具，从而更新工作目录绑定）
    RegisterBuiltinTools(ToolRegistry::GetInstance(), new_dir.string());
    utils::PrintSuccess("工作目录: " + new_dir.string());
}

void Repl::HandlePwd() {
    std::cout << context_.GetWorkingDir().string() << "\n";
}

void Repl::HandleAdd(const std::string& args) {
    if (args.empty()) {
        utils::PrintWarning("用法: /add <代码或文字>");
        return;
    }
    // 将内容注入到下一次对话的用户消息中（作为系统附加）
    context_.AddUserMessage("[附加上下文]\n" + args);
    context_.AddAssistantMessage("好的，我已记录该内容。");
    utils::PrintSuccess("已添加到上下文");
}

void Repl::HandleTokens() {
    size_t est = context_.EstimateTokens();
    std::cout << utils::color::kCyan << "预估 token 用量: "
              << utils::color::kReset << est << "\n";
}

// ── AI 代理循环 ───────────────────────────────────────────────────────────

void Repl::RunAgentLoop(const std::string& user_message) {
    context_.AddUserMessage(user_message);

    auto& registry = ToolRegistry::GetInstance();
    const auto& cfg = ConfigManager::GetInstance().config();
    auto tools = registry.GetToolDefinitionsJson();

    int iterations = 0;

    while (iterations++ < cfg.max_tool_iterations) {
        auto messages = context_.GetMessages();

        std::cout << "\n";

        // ── 启动思考动画 ──────────────────────────────────────────────────
        utils::Spinner spinner;
        spinner.Start("思考中… (" + std::to_string(iterations) + "/" +
                       std::to_string(cfg.max_tool_iterations) + ")");

        ChatResponse response;

        if (cfg.stream) {
            // 第一个文字 chunk 到达时停止 spinner，再输出头部标记
            bool first_chunk = true;
            response = ai_client_->ChatStream(
                messages, tools,
                [&first_chunk, &spinner](const std::string& chunk) {
                    if (first_chunk) {
                        // 清除 spinner 行，打印 AI 发言头部
                        spinner.Stop();
                        std::cout << utils::color::kBrightGreen
                                  << "Termind" << utils::color::kReset
                                  << utils::color::kDim << " ❯ "
                                  << utils::color::kReset;
                        first_chunk = false;
                    }
                    std::cout << chunk;
                    std::cout.flush();
                });
            // 若整轮均为工具调用（无文字 chunk），保证 spinner 已清除
            spinner.Stop();
        } else {
            response = ai_client_->Chat(messages, tools);
            spinner.Stop();
        }

        // ── 请求失败 ──────────────────────────────────────────────────────
        if (!response.success) {
            utils::PrintError("AI 请求失败: " + response.error_message);
            break;
        }

        // ── 最终文字回答（无工具调用）────────────────────────────────────
        if (!response.HasToolCalls()) {
            if (!cfg.stream) {
                // 非流式：整体打印
                std::cout << utils::color::kBrightGreen
                          << "Termind" << utils::color::kReset
                          << utils::color::kDim << " ❯ "
                          << utils::color::kReset
                          << response.content;
            }
            std::cout << "\n";
            context_.AddAssistantMessage(response.content);
            break;
        }

        // ── 处理工具调用 ──────────────────────────────────────────────────
        std::cout << "\n";
        context_.AddAssistantToolCalls(response.tool_calls, response.content);

        for (const auto& tc : response.tool_calls) {
            PrintToolCallHeader(tc);
            ToolResult result = ExecuteToolWithConfirmation(tc);
            context_.AddToolResult(tc.id, result.output);

            if (result.success) {
                // 工具输出用 dim 色折叠显示（最多显示 40 行避免刷屏）
                auto lines = utils::Split(result.output, '\n');
                int show = std::min(40, static_cast<int>(lines.size()));
                for (int i = 0; i < show; ++i) {
                    std::cout << utils::color::kDim << "  "
                              << lines[i] << utils::color::kReset << "\n";
                }
                if (static_cast<int>(lines.size()) > show) {
                    std::cout << utils::color::kDim << "  … (省略 "
                              << lines.size() - show << " 行)"
                              << utils::color::kReset << "\n";
                }
            } else {
                utils::PrintError("工具执行失败: " + result.output);
            }
        }

        std::cout << "\n";
    }

    if (iterations > cfg.max_tool_iterations) {
        utils::PrintWarning("已达到最大工具调用次数（" +
                             std::to_string(cfg.max_tool_iterations) + "）");
    }
}

// ── 工具调用确认 ──────────────────────────────────────────────────────────

ToolResult Repl::ExecuteToolWithConfirmation(const ToolCallRequest& tc) {
    auto& registry = ToolRegistry::GetInstance();
    bool needs_confirm = registry.RequiresConfirmation(tc.name);

    if (!needs_confirm) {
        // 非破坏性操作：直接执行
        return registry.Execute(tc.name, tc.arguments);
    }

    // 展示操作预览
    if (tc.name == "write_file") {
        std::string path_str = tc.arguments.value("path", "");
        std::string content  = tc.arguments.value("content", "");
        ShowWriteFilePreview(path_str, content);

    } else if (tc.name == "edit_file") {
        std::string path_str    = tc.arguments.value("path", "");
        std::string old_content = tc.arguments.value("old_content", "");
        std::string new_content = tc.arguments.value("new_content", "");
        ShowEditFilePreview(path_str, old_content, new_content);

    } else if (tc.name == "run_shell") {
        std::string cmd = tc.arguments.value("command", "");
        std::cout << "\n" << utils::color::kYellow << "  $ "
                  << utils::color::kReset << cmd << "\n";

    } else {
        std::cout << "\n  " << tc.arguments.dump(2) << "\n";
    }

    char choice = utils::AskYesNoEdit("执行此操作?");
    std::cout << "\n";

    if (choice == 'n') {
        return {false, "用户已拒绝该操作。"};
    }

    if (choice == 'e' &&
        (tc.name == "write_file" || tc.name == "edit_file")) {
        // 打开编辑器
        std::string editor = utils::GetEnv("EDITOR", "vi");
        std::string path_str = tc.arguments.value(
            tc.name == "write_file" ? "path" : "path", "");
        std::string content = tc.arguments.value(
            tc.name == "write_file" ? "content" : "new_content", "");

        // 写入临时文件
        char tmp[] = "/tmp/termind_edit_XXXXXX";
        int fd = mkstemp(tmp);
        if (fd >= 0) {
            if (write(fd, content.data(),
                      static_cast<ssize_t>(content.size())) < 0) {}
            close(fd);
            std::string cmd = editor + " " + tmp;
            if (system(cmd.c_str()) == 0) {
                auto edited = utils::ReadFile(tmp);
                unlink(tmp);
                if (edited) {
                    // 用编辑后的内容执行
                    nlohmann::json new_args = tc.arguments;
                    new_args[tc.name == "write_file" ? "content" : "new_content"] =
                        *edited;
                    return registry.Execute(tc.name, new_args);
                }
            }
            unlink(tmp);
        }
    }

    return registry.Execute(tc.name, tc.arguments);
}

// ── 文件写入预览 ──────────────────────────────────────────────────────────

void Repl::ShowWriteFilePreview(const std::string& path_str,
                                  const std::string& new_content) {
    fs::path path = path_str[0] == '/'
                        ? fs::path(path_str)
                        : context_.GetWorkingDir() / path_str;

    std::cout << "\n";

    if (utils::FileExists(path)) {
        auto old_content = utils::ReadFile(path);
        if (old_content) {
            std::string diff = utils::ComputeDiff(*old_content, new_content,
                                                   path_str);
            utils::PrintColoredDiff(diff);
        }
    } else {
        // 新文件：展示前 30 行
        std::cout << utils::color::kDim << "  新文件: " << path_str
                  << utils::color::kReset << "\n";
        auto lines = utils::Split(new_content, '\n');
        int show   = std::min(30, static_cast<int>(lines.size()));
        for (int i = 0; i < show; ++i) {
            std::cout << utils::color::kGreen << "  + " << lines[i]
                      << utils::color::kReset << "\n";
        }
        if (static_cast<int>(lines.size()) > show) {
            std::cout << utils::color::kDim << "  … (还有 "
                      << lines.size() - show << " 行)"
                      << utils::color::kReset << "\n";
        }
    }
}

// ── edit_file 预览 ────────────────────────────────────────────────────────

void Repl::ShowEditFilePreview(const std::string& path_str,
                                 const std::string& old_content,
                                 const std::string& new_content) {
    fs::path path = path_str[0] == '/'
                        ? fs::path(path_str)
                        : context_.GetWorkingDir() / path_str;

    std::cout << "\n";
    std::cout << utils::color::kDim << "  编辑文件: " << path_str
              << utils::color::kReset << "\n";

    std::string diff = utils::ComputeDiff(old_content, new_content, path_str);
    utils::PrintColoredDiff(diff);
}

// ── 工具调用头部显示 ──────────────────────────────────────────────────────

void Repl::PrintToolCallHeader(const ToolCallRequest& tc) const {
    std::string icon;
    if      (tc.name == "read_file"       ) icon = "📖";
    else if (tc.name == "write_file"      ) icon = "✏️ ";
    else if (tc.name == "edit_file"       ) icon = "🔧";
    else if (tc.name == "list_directory"  ) icon = "📁";
    else if (tc.name == "search_files"    ) icon = "🔍";
    else if (tc.name == "grep_code"       ) icon = "🔎";
    else if (tc.name == "run_shell"       ) icon = "⚡";
    else if (tc.name == "get_file_info"   ) icon = "ℹ️ ";
    else                                    icon = "🔧";

    std::string brief;
    if (tc.arguments.contains("path"))
        brief = tc.arguments["path"].get<std::string>();
    else if (tc.arguments.contains("command"))
        brief = tc.arguments["command"].get<std::string>();
    else if (tc.arguments.contains("pattern"))
        brief = tc.arguments["pattern"].get<std::string>();

    std::cout << utils::color::kDim << "  " << icon << " "
              << utils::color::kYellow << tc.name
              << utils::color::kReset
              << utils::color::kDim << (brief.empty() ? "" : "  " + brief)
              << utils::color::kReset << "\n";
}

// ── 欢迎界面 ──────────────────────────────────────────────────────────────

void Repl::PrintWelcome() const {
    std::cout << "\n"
              << utils::color::kBold << utils::color::kBrightCyan
              << "  ████████╗███████╗██████╗ ███╗   ███╗██╗███╗   ██╗██████╗\n"
              << "     ██╔══╝██╔════╝██╔══██╗████╗ ████║██║████╗  ██║██╔══██╗\n"
              << "     ██║   █████╗  ██████╔╝██╔████╔██║██║██╔██╗ ██║██║  ██║\n"
              << "     ██║   ██╔══╝  ██╔══██╗██║╚██╔╝██║██║██║╚██╗██║██║  ██║\n"
              << "     ██║   ███████╗██║  ██║██║ ╚═╝ ██║██║██║ ╚████║██████╔╝\n"
              << "     ╚═╝   ╚══════╝╚═╝  ╚═╝╚═╝     ╚═╝╚═╝╚═╝  ╚═══╝╚═════╝\n"
              << utils::color::kReset
              << utils::color::kDim
              << "                     端脑 · 终端代码助手 v0.1.0 by xingxing\n"
              << utils::color::kReset << "\n";

    const auto& cfg = ConfigManager::GetInstance().config();
    std::cout << "  模型: " << utils::color::kCyan << cfg.model
              << utils::color::kReset << "   "
              << "目录: " << utils::color::kCyan
              << fs::current_path().string() << utils::color::kReset << "\n";

    if (cfg.api_key.empty()) {
        utils::PrintWarning("未检测到 API Key！请设置 TERMIND_API_KEY 或 OPENAI_API_KEY 环境变量。");
    }

    std::cout << utils::color::kDim
              << "  输入 /help 查看命令，/quit 退出\n"
              << utils::color::kReset << "\n";
}

// ── 构建提示符 ────────────────────────────────────────────────────────────

std::string Repl::BuildPrompt() const {
    std::string cwd = context_.GetWorkingDir().filename().string();
    if (cwd.empty()) cwd = "/";

    return std::string(utils::color::kBold) +
           utils::color::kBrightCyan + "termind" +
           utils::color::kReset +
           utils::color::kDim + " " + cwd + " " +
           utils::color::kReset +
           utils::color::kBrightCyan + "❯ " +
           utils::color::kReset;
}

}  // namespace termind
