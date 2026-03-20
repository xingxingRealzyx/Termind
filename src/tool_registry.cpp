#include "termind/tool_registry.h"
#include "termind/skill_manager.h"
#include "termind/utils.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <iostream>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace termind {

namespace fs = std::filesystem;

// ── ToolDefinition::ToJson ────────────────────────────────────────────────

nlohmann::json ToolDefinition::ToJson() const {
    nlohmann::json props = nlohmann::json::object();
    nlohmann::json required_list = nlohmann::json::array();

    for (const auto& p : parameters) {
        nlohmann::json pj;
        pj["type"] = p.type;
        pj["description"] = p.description;
        if (!p.enum_values.empty()) {
            pj["enum"] = p.enum_values;
        }
        props[p.name] = pj;
        if (p.required) required_list.push_back(p.name);
    }

    return {
        {"type", "function"},
        {"function", {
            {"name", name},
            {"description", description},
            {"parameters", {
                {"type", "object"},
                {"properties", props},
                {"required", required_list}
            }}
        }}
    };
}

// ── ToolRegistry ──────────────────────────────────────────────────────────

ToolRegistry& ToolRegistry::GetInstance() {
    static ToolRegistry instance;
    return instance;
}

void ToolRegistry::Register(Tool tool) {
    std::string name = tool.definition.name;
    tools_[std::move(name)] = std::move(tool);
}

std::vector<nlohmann::json> ToolRegistry::GetToolDefinitionsJson() const {
    std::vector<nlohmann::json> result;
    result.reserve(tools_.size());
    for (const auto& [_, tool] : tools_) {
        result.push_back(tool.definition.ToJson());
    }
    return result;
}

ToolResult ToolRegistry::Execute(const std::string& name,
                                   const nlohmann::json& args) {
    auto it = tools_.find(name);
    if (it == tools_.end()) {
        return {false, "未知工具: " + name};
    }
    try {
        return it->second.function(args);
    } catch (const nlohmann::json::exception& e) {
        return {false, std::string("参数错误: ") + e.what()};
    } catch (const std::exception& e) {
        return {false, std::string("工具执行错误: ") + e.what()};
    }
}

bool ToolRegistry::HasTool(const std::string& name) const {
    return tools_.count(name) > 0;
}

bool ToolRegistry::RequiresConfirmation(const std::string& name) const {
    auto it = tools_.find(name);
    return it != tools_.end() && it->second.requires_confirmation;
}

std::vector<std::string> ToolRegistry::GetToolNames() const {
    std::vector<std::string> names;
    names.reserve(tools_.size());
    for (const auto& [name, _] : tools_) names.push_back(name);
    return names;
}

// ── 辅助：解析路径 ────────────────────────────────────────────────────────

static fs::path ResolvePath(const std::string& path_str,
                              const fs::path& working_dir) {
    fs::path p(path_str);
    if (p.is_absolute()) return p;
    return fs::weakly_canonical(working_dir / p);
}

// ── 内置工具注册 ──────────────────────────────────────────────────────────

void RegisterBuiltinTools(ToolRegistry& registry,
                           const std::string& working_dir_str) {
    fs::path wd = working_dir_str.empty() ? fs::current_path()
                                           : fs::path(working_dir_str);

    // ════════════════════════════════════════════════════════════════════
    // read_file
    // ════════════════════════════════════════════════════════════════════
    registry.Register({
        {
            "read_file",
            "读取文件内容。可选择只读特定行范围。",
            {
                {"path",       "string",  "文件路径（绝对或相对工作目录）", true},
                {"start_line", "integer", "起始行号（从 1 开始，可选）",   false},
                {"end_line",   "integer", "结束行号（含，可选）",          false},
            }
        },
        [wd](const nlohmann::json& args) -> ToolResult {
            std::string path_str = args.at("path").get<std::string>();
            fs::path path = ResolvePath(path_str, wd);

            if (!fs::exists(path))
                return {false, "文件不存在: " + path_str};
            if (fs::is_directory(path))
                return {false, "路径是目录: " + path_str};

            auto content = utils::ReadFile(path);
            if (!content)
                return {false, "读取文件失败: " + path_str};

            // 行范围截取
            if (args.contains("start_line") || args.contains("end_line")) {
                auto lines = utils::Split(*content, '\n');
                int total = static_cast<int>(lines.size());
                int start = args.value("start_line", 1) - 1;
                int end   = args.value("end_line", total) - 1;
                start = std::max(0, std::min(start, total - 1));
                end   = std::max(start, std::min(end, total - 1));

                std::ostringstream ss;
                ss << "文件: " << path_str << " (行 " << start + 1
                   << "~" << end + 1 << " / 共 " << total << " 行)\n---\n";
                for (int i = start; i <= end; ++i) {
                    ss << std::setw(5) << (i + 1) << " | " << lines[i] << "\n";
                }
                return {true, ss.str()};
            }

            // 统计行数
            int total_lines = 0;
            for (char c : *content) if (c == '\n') ++total_lines;
            if (!content->empty() && content->back() != '\n') ++total_lines;

            std::ostringstream ss;
            ss << "文件: " << path_str << "  ("
               << utils::FormatFileSize(content->size())
               << "，共 " << total_lines << " 行)\n---\n"
               << *content;

            // 大文件无 offset：附加提示，引导下次先用 get_file_outline
            if (total_lines > 200) {
                ss << "\n---\n[提示：此文件共 " << total_lines
                   << " 行，下次阅读前建议先调用 get_file_outline 获取结构摘要，"
                   "再用 start_line/end_line 精确读取目标片段以节省上下文]";
            }
            return {true, ss.str()};
        },
        false  // 读操作无需确认
    });

    // ════════════════════════════════════════════════════════════════════
    // write_file
    // ════════════════════════════════════════════════════════════════════
    registry.Register({
        {
            "write_file",
            "将内容写入文件（覆盖写）。如父目录不存在将自动创建。"
            "执行前会展示 diff 并需要用户确认。",
            {
                {"path",    "string", "目标文件路径", true},
                {"content", "string", "要写入的完整内容", true},
            }
        },
        [wd](const nlohmann::json& args) -> ToolResult {
            std::string path_str = args.at("path").get<std::string>();
            std::string content  = args.at("content").get<std::string>();
            fs::path path = ResolvePath(path_str, wd);

            if (!utils::WriteFile(path, content))
                return {false, "写入文件失败: " + path_str};

            return {true, "已写入 " + utils::FormatFileSize(content.size()) +
                          " → " + path_str};
        },
        true  // 需要用户确认
    });

    // ════════════════════════════════════════════════════════════════════
    // edit_file  — 精准替换文件中某段内容
    // ════════════════════════════════════════════════════════════════════
    registry.Register({
        {
            "edit_file",
            "精准替换文件中指定的旧内容为新内容。old_content 必须在文件中唯一存在。"
            "执行前会展示 diff 并需要用户确认。",
            {
                {"path",        "string", "目标文件路径", true},
                {"old_content", "string", "要被替换的精确原始内容",  true},
                {"new_content", "string", "替换后的新内容",          true},
            }
        },
        [wd](const nlohmann::json& args) -> ToolResult {
            std::string path_str    = args.at("path").get<std::string>();
            std::string old_content = args.at("old_content").get<std::string>();
            std::string new_content = args.at("new_content").get<std::string>();
            fs::path path = ResolvePath(path_str, wd);

            auto file_content = utils::ReadFile(path);
            if (!file_content)
                return {false, "读取文件失败: " + path_str};

            size_t pos = file_content->find(old_content);
            if (pos == std::string::npos)
                return {false, "在文件中未找到指定的 old_content，请检查内容是否完全匹配"};

            // 检查唯一性
            size_t pos2 = file_content->find(old_content, pos + 1);
            if (pos2 != std::string::npos)
                return {false, "old_content 在文件中出现多次，请提供更多上下文以确保唯一性"};

            std::string updated = file_content->substr(0, pos) +
                                  new_content +
                                  file_content->substr(pos + old_content.size());

            if (!utils::WriteFile(path, updated))
                return {false, "写入文件失败: " + path_str};

            return {true, "文件已更新: " + path_str};
        },
        true
    });

    // ════════════════════════════════════════════════════════════════════
    // list_directory
    // ════════════════════════════════════════════════════════════════════
    registry.Register({
        {
            "list_directory",
            "列出目录中的文件和子目录。",
            {
                {"path",      "string",  "要列出的目录路径（默认为工作目录）", false},
                {"recursive", "boolean", "是否递归列出子目录",                false},
            }
        },
        [wd](const nlohmann::json& args) -> ToolResult {
            std::string path_str = args.value("path", ".");
            bool recursive       = args.value("recursive", false);
            fs::path path = ResolvePath(path_str, wd);

            if (!fs::exists(path))
                return {false, "路径不存在: " + path_str};
            if (!fs::is_directory(path))
                return {false, "路径不是目录: " + path_str};

            std::ostringstream ss;
            ss << path_str << "/\n";

            auto print_entry = [&](const fs::directory_entry& entry) {
                std::string rel = utils::GetRelativePath(entry.path(), path);
                if (entry.is_directory()) {
                    ss << "  📁 " << rel << "/\n";
                } else {
                    std::error_code ec;
                    auto sz = entry.file_size(ec);
                    ss << "  📄 " << rel;
                    if (!ec) ss << "  (" << utils::FormatFileSize(sz) << ")";
                    ss << "\n";
                }
            };

            try {
                if (recursive) {
                    for (const auto& e : fs::recursive_directory_iterator(
                             path, fs::directory_options::skip_permission_denied)) {
                        print_entry(e);
                    }
                } else {
                    std::vector<fs::directory_entry> entries;
                    for (const auto& e : fs::directory_iterator(path))
                        entries.push_back(e);
                    std::sort(entries.begin(), entries.end(),
                              [](const auto& a, const auto& b) {
                                  return a.path().filename() < b.path().filename();
                              });
                    for (const auto& e : entries) print_entry(e);
                }
            } catch (const std::exception& e) {
                return {false, std::string("遍历目录失败: ") + e.what()};
            }

            return {true, ss.str()};
        },
        false
    });

    // ════════════════════════════════════════════════════════════════════
    // search_files  — 按文件名 glob 搜索
    // ════════════════════════════════════════════════════════════════════
    registry.Register({
        {
            "search_files",
            "按文件名模式搜索文件（支持 * 通配符）。例如 '*.cpp'、'test_*'。",
            {
                {"pattern",   "string", "文件名模式",                             true},
                {"directory", "string", "搜索目录（默认为工作目录）",             false},
            }
        },
        [wd](const nlohmann::json& args) -> ToolResult {
            std::string pattern  = args.at("pattern").get<std::string>();
            std::string dir_str  = args.value("directory", ".");
            fs::path search_dir  = ResolvePath(dir_str, wd);

            if (!fs::exists(search_dir))
                return {false, "目录不存在: " + dir_str};

            // 将 glob 模式转为 regex
            std::string re_str;
            re_str += "^";
            for (char c : pattern) {
                switch (c) {
                    case '*': re_str += ".*"; break;
                    case '?': re_str += ".";  break;
                    case '.': re_str += "\\."; break;
                    default:  re_str += c;     break;
                }
            }
            re_str += "$";

            std::regex re(re_str, std::regex::icase);
            std::vector<std::string> matches;

            try {
                for (const auto& entry : fs::recursive_directory_iterator(
                         search_dir,
                         fs::directory_options::skip_permission_denied)) {
                    if (!entry.is_regular_file()) continue;
                    if (std::regex_match(entry.path().filename().string(), re)) {
                        matches.push_back(
                            utils::GetRelativePath(entry.path(), wd));
                    }
                }
            } catch (const std::exception& e) {
                return {false, std::string("搜索失败: ") + e.what()};
            }

            if (matches.empty())
                return {true, "未找到匹配 '" + pattern + "' 的文件"};

            std::ostringstream ss;
            ss << "找到 " << matches.size() << " 个文件（匹配 '"
               << pattern << "'）:\n";
            for (const auto& m : matches) ss << "  " << m << "\n";
            return {true, ss.str()};
        },
        false
    });

    // ════════════════════════════════════════════════════════════════════
    // grep_code  — 搜索代码内容
    // ════════════════════════════════════════════════════════════════════
    registry.Register({
        {
            "grep_code",
            "在文件中搜索文本或正则表达式，返回匹配行及上下文。",
            {
                {"pattern",       "string",  "搜索模式（支持正则）",               true},
                {"path",          "string",  "搜索路径（文件或目录，默认工作目录）", false},
                {"file_pattern",  "string",  "文件名过滤，如 '*.cpp'",             false},
                {"context_lines", "integer", "上下文行数（默认 2）",               false},
                {"case_sensitive","boolean", "是否区分大小写（默认 true）",         false},
            }
        },
        [wd](const nlohmann::json& args) -> ToolResult {
            std::string pattern      = args.at("pattern").get<std::string>();
            std::string path_str     = args.value("path", ".");
            std::string file_pattern = args.value("file_pattern", "");
            int context_lines        = args.value("context_lines", 2);
            bool case_sens           = args.value("case_sensitive", true);

            fs::path search_path = ResolvePath(path_str, wd);

            std::string cmd = "grep -rn";
            if (!case_sens) cmd += "i";
            if (context_lines > 0)
                cmd += " -C " + std::to_string(context_lines);
            if (!file_pattern.empty())
                cmd += " --include=" + utils::EscapeShellArg(file_pattern);
            cmd += " -E " + utils::EscapeShellArg(pattern);
            cmd += " " + utils::EscapeShellArg(search_path.string());
            cmd += " 2>/dev/null | head -300";

            FILE* pipe = popen(cmd.c_str(), "r");
            if (!pipe) return {false, "启动 grep 失败"};

            std::string result;
            char buf[512];
            while (fgets(buf, sizeof(buf), pipe)) result += buf;
            int status = pclose(pipe);

            if (result.empty()) {
                if (WEXITSTATUS(status) == 1)
                    return {true, "未找到匹配 '" + pattern + "' 的内容"};
                return {false, "grep 执行失败"};
            }

            return {true, "搜索 '" + pattern + "' 的结果:\n" + result};
        },
        false
    });

    // ════════════════════════════════════════════════════════════════════
    // search_symbol  — 智能符号搜索（函数/类/变量定义及用法）
    // ════════════════════════════════════════════════════════════════════
    registry.Register({
        {
            "search_symbol",
            "在代码库中搜索函数、类、结构体、变量的定义或使用位置。"
            "比 grep_code 更智能：会针对定义模式构造搜索，适合"
            "\"函数在哪里定义\"、\"类在哪里声明\" 等场景。",
            {
                {"symbol",     "string", "要搜索的符号名（函数名、类名、变量名等）", true},
                {"search_type","string",
                 "搜索类型：\"definition\"（仅定义）、\"usage\"（仅用法）、"
                 "\"all\"（全部，默认）", false},
                {"file_glob",  "string", "限定文件范围，如 \"*.cpp\" 或 \"*.py\"", false},
                {"path",       "string", "搜索根目录，默认为工作目录", false},
            }
        },
        [wd](const nlohmann::json& args) -> ToolResult {
            std::string symbol      = args.at("symbol").get<std::string>();
            std::string search_type = args.value("search_type", "all");
            std::string file_glob   = args.value("file_glob", "");
            std::string path_str    = args.value("path", ".");
            fs::path search_path    = ResolvePath(path_str, wd);

            // 转义符号中的特殊正则字符
            std::string esc_sym;
            for (char c : symbol) {
                if (std::string(".[*+?^${}()|\\").find(c) != std::string::npos)
                    esc_sym += '\\';
                esc_sym += c;
            }

            // 按语言类型和搜索类型构造不同的正则模式
            std::vector<std::string> patterns;

            if (search_type == "definition" || search_type == "all") {
                // C/C++：函数定义/声明
                patterns.push_back(
                    "(^|\\s)(void|int|bool|auto|char|float|double|long|short|"
                    "unsigned|static|inline|virtual|explicit|constexpr|std::\\w+|\\w+)\\s+"
                    + esc_sym + "\\s*\\(");
                // C/C++：类/结构体/枚举定义
                patterns.push_back(
                    "\\b(class|struct|enum|union|typedef)\\b[\\s\\w]*\\b"
                    + esc_sym + "\\b");
                // Python：函数/类定义
                patterns.push_back("^\\s*(def|class|async def)\\s+" + esc_sym + "\\b");
                // Go/Rust/Java/JS：函数/类/接口定义
                patterns.push_back(
                    "\\b(func|fn|function|def|class|interface|trait|type)\\s+"
                    + esc_sym + "\\b");
                // 变量/常量定义（通用）
                patterns.push_back(
                    "\\b(const|let|var|val|static|extern)\\s+[\\w<>*&:\\s]*\\b"
                    + esc_sym + "\\b");
            }

            if (search_type == "usage" || search_type == "all") {
                // 任何包含符号的行（用法）
                patterns.push_back("\\b" + esc_sym + "\\b");
            }

            // 若 all 模式，先用精确定义模式搜，结果少则再搜 usage
            std::string result;
            std::string include_arg = file_glob.empty()
                ? "" : " --include=" + utils::EscapeShellArg(file_glob);

            if (search_type == "all") {
                // 先只搜定义，输出数量少、更有价值
                std::string def_pattern = "(^|\\s)(" 
                    "void|int|bool|auto|char|float|double|long|unsigned|"
                    "static|inline|virtual|std::\\w+|\\w+)\\s+" + esc_sym + "\\s*\\("
                    "|\\b(class|struct|enum|union|typedef)\\b[\\s\\w]*\\b" + esc_sym + "\\b"
                    "|^\\s*(def|class|async def|func|fn|function|interface|trait)\\s+" + esc_sym + "\\b";

                std::string cmd = "grep -rn -E "
                    + utils::EscapeShellArg(def_pattern)
                    + include_arg
                    + " " + utils::EscapeShellArg(search_path.string())
                    + " 2>/dev/null | head -50";

                FILE* p = popen(cmd.c_str(), "r");
                if (p) {
                    char buf[512];
                    while (fgets(buf, sizeof(buf), p)) result += buf;
                    pclose(p);
                }

                // 如果定义结果足够，直接返回
                if (!result.empty()) {
                    return {true, "符号 '" + symbol + "' 的定义位置:\n" + result};
                }

                // 没有定义，退而求其次搜用法
                std::string use_cmd = "grep -rn -E "
                    + utils::EscapeShellArg("\\b" + esc_sym + "\\b")
                    + include_arg
                    + " " + utils::EscapeShellArg(search_path.string())
                    + " 2>/dev/null | head -50";

                FILE* p2 = popen(use_cmd.c_str(), "r");
                if (p2) {
                    char buf[512];
                    while (fgets(buf, sizeof(buf), p2)) result += buf;
                    pclose(p2);
                }

                if (result.empty())
                    return {true, "未找到符号 '" + symbol + "' 的定义或用法"};
                return {true, "未找到 '" + symbol + "' 的定义，以下是使用位置:\n" + result};
            }

            // 非 all 模式：用对应模式列表逐一搜索
            for (const auto& pat : patterns) {
                std::string cmd = "grep -rn -E "
                    + utils::EscapeShellArg(pat)
                    + include_arg
                    + " " + utils::EscapeShellArg(search_path.string())
                    + " 2>/dev/null | head -80";
                FILE* p = popen(cmd.c_str(), "r");
                if (p) {
                    char buf[512];
                    while (fgets(buf, sizeof(buf), p)) result += buf;
                    pclose(p);
                }
                if (!result.empty()) break;
            }

            if (result.empty())
                return {true, "未找到符号 '" + symbol + "'"};

            std::string label = (search_type == "definition") ? "定义" : "用法";
            return {true, "符号 '" + symbol + "' 的" + label + ":\n" + result};
        },
        false
    });

    // ════════════════════════════════════════════════════════════════════
    // run_shell  — 执行 shell 命令（需要确认）
    // ════════════════════════════════════════════════════════════════════
    registry.Register({
        {
            "run_shell",
            "在工作目录执行 shell 命令。会显示命令并请求用户确认。",
            {
                {"command", "string", "要执行的 shell 命令", true},
            }
        },
        [wd](const nlohmann::json& args) -> ToolResult {
            std::string command = args.at("command").get<std::string>();

            // 危险命令黑名单（防止命令注入或误操作）
            static const std::vector<std::string> kDangerousPatterns = {
                "rm -rf /", "rm -rf /*", "mkfs ", "dd if=",
            };
            for (const auto& pat : kDangerousPatterns) {
                if (command.find(pat) != std::string::npos)
                    return {false, "拒绝执行：命令包含危险模式 \"" + pat + "\""};
            }

            std::string full_cmd = "cd " + utils::EscapeShellArg(wd.string()) +
                                   " && " + command + " 2>&1";

            FILE* pipe = popen(full_cmd.c_str(), "r");
            if (!pipe) return {false, "无法启动命令"};

            // 截断命令标题（太长时省略中间部分）
            auto make_heading = [](const std::string& cmd) -> std::string {
                constexpr size_t kMaxLen = 60;
                std::string disp = cmd;
                if (disp.size() > kMaxLen)
                    disp = disp.substr(0, kMaxLen / 2) + " … " +
                           disp.substr(disp.size() - kMaxLen / 2);
                return std::string(utils::color::kYellow) + "⚡ " +
                       utils::color::kReset +
                       utils::color::kDim + "$ " + disp +
                       utils::color::kReset;
            };

            utils::ThinkingPane pane;
            pane.Start(make_heading(command));

            // 用 poll() + 非阻塞读替代阻塞 fgets，解决后台进程（&）占住管道
            // 导致永久阻塞的问题
            int fd = fileno(pipe);
            fcntl(fd, F_SETFL, O_NONBLOCK);

            // 空闲超时：连续 30 秒无新输出时停止等待（处理 & 后台进程）
            // 硬性超时：总计 120 秒（兜底）
            constexpr int kIdleSecs = 30;
            constexpr int kHardSecs = 120;

            auto wall_start = std::chrono::steady_clock::now();
            auto last_data  = wall_start;

            std::string output;
            bool truncated  = false;
            bool timed_out  = false;
            char buf[4096];

            while (true) {
                auto now       = std::chrono::steady_clock::now();
                auto wall_secs = std::chrono::duration_cast<std::chrono::seconds>(
                                     now - wall_start).count();
                auto idle_secs = std::chrono::duration_cast<std::chrono::seconds>(
                                     now - last_data).count();

                if (wall_secs >= kHardSecs) {
                    output += "\n[硬性超时：命令运行超过 " +
                              std::to_string(kHardSecs) + " 秒，已停止等待]\n";
                    timed_out = true;
                    break;
                }

                struct pollfd pfd{fd, POLLIN, 0};
                int ret = poll(&pfd, 1, 200);  // 每 200ms 轮询一次

                if (ret < 0) {
                    if (errno == EINTR) continue;
                    break;
                }

                if (ret == 0) {
                    // 200ms 内无数据
                    if (!output.empty() && idle_secs >= kIdleSecs) {
                        // 已有输出，但超过空闲阈值 → 后台进程占管道，退出等待
                        output += "\n[空闲超时：" + std::to_string(kIdleSecs) +
                                  " 秒无新输出，已停止等待（后台进程可能仍在运行）]\n";
                        timed_out = true;
                        break;
                    }
                    continue;
                }

                // 有事件
                if (pfd.revents & (POLLHUP | POLLERR)) {
                    // 管道关闭，排干剩余数据
                    while (true) {
                        ssize_t n = read(fd, buf, sizeof(buf) - 1);
                        if (n <= 0) break;
                        buf[n] = '\0';
                        std::string chunk(buf, static_cast<size_t>(n));
                        pane.FeedRaw(chunk);
                        output += chunk;
                    }
                    break;
                }

                if (pfd.revents & POLLIN) {
                    ssize_t n = read(fd, buf, sizeof(buf) - 1);
                    if (n < 0 && errno == EAGAIN) continue;
                    if (n <= 0) break;  // EOF
                    buf[n] = '\0';
                    last_data = now;
                    std::string chunk(buf, static_cast<size_t>(n));
                    pane.FeedRaw(chunk);
                    output += chunk;
                    if (output.size() > 100000) {
                        output += "\n... (输出已截断) ...\n";
                        truncated = true;
                        break;
                    }
                }
            }

            // pclose 等待 shell 子进程退出；若子进程已退出则立即返回
            int status = pclose(pipe);
            pane.Stop();

            int exit_code = (WIFEXITED(status) && !timed_out)
                                ? WEXITSTATUS(status) : -1;

            std::ostringstream ss;
            ss << "$ " << command << "\n" << output;
            if (truncated || timed_out)
                ss << "\n[退出码: 未知]";
            else
                ss << "\n[退出码: " << exit_code << "]";
            return {(!timed_out && !truncated && exit_code == 0), ss.str()};
        },
        true  // 需要确认
    });

    // ════════════════════════════════════════════════════════════════════
    // list_skills  — 列出可用 Skills
    // ════════════════════════════════════════════════════════════════════
    registry.Register({
        {
            "list_skills",
            "列出当前已发现的所有可用 Skills，包含每个 Skill 的名称和描述。"
            "调用 load_skill 前可先用此工具查看可选项。",
            {}  // 无参数
        },
        [](const nlohmann::json& /*args*/) -> ToolResult {
            auto& sm = SkillManager::GetInstance();
            const auto& skills = sm.GetSkills();

            if (skills.empty()) {
                return {true, "当前没有可用的 Skills。\n"
                              "将 Skill 目录放到 ~/.config/termind/skills/ 或"
                              " <工作目录>/.termind/skills/ 下，重启后自动加载。"};
            }

            std::ostringstream ss;
            ss << "可用 Skills (" << skills.size() << " 个):\n\n";
            for (const auto& s : skills) {
                bool loaded = sm.IsLoaded(s.name);
                ss << "  " << (loaded ? "✅" : "⬜") << " **" << s.name << "**\n"
                   << "     " << s.description << "\n"
                   << "     路径: " << s.dir.string() << "\n\n";
            }
            ss << "使用 load_skill 工具加载指定 Skill 的完整指导。";
            return {true, ss.str()};
        },
        false
    });

    // ════════════════════════════════════════════════════════════════════
    // load_skill  — 加载指定 Skill 的完整指导到上下文
    // ════════════════════════════════════════════════════════════════════
    registry.Register({
        {
            "load_skill",
            "加载指定 Skill 的完整 SKILL.md 指导内容。"
            "当用户请求与某个 Skill 相关时，调用此工具获取完整指导，然后再执行任务。"
            "已加载的 Skill 再次调用会返回缓存内容。",
            {
                {"name", "string", "Skill 的名称（使用 list_skills 查看可用列表）", true},
            }
        },
        [](const nlohmann::json& args) -> ToolResult {
            std::string name = args.at("name").get<std::string>();
            auto& sm = SkillManager::GetInstance();

            const SkillMeta* meta = sm.FindSkill(name);
            if (!meta) {
                return {false, "未找到 Skill: " + name +
                               "。使用 list_skills 查看可用列表。"};
            }

            auto body = sm.GetSkillBody(name);
            if (!body) {
                return {false, "无法读取 Skill 内容: " + name};
            }

            sm.MarkLoaded(name);

            std::ostringstream ss;
            ss << "# Skill 已加载: " << name << "\n\n"
               << *body;
            return {true, ss.str()};
        },
        false
    });

    // ════════════════════════════════════════════════════════════════════
    // load_skill_file  — 加载 Skill 目录下的附属文件
    // ════════════════════════════════════════════════════════════════════
    registry.Register({
        {
            "load_skill_file",
            "加载指定 Skill 目录下的附属文件（如 scripts/、reference/ 下的文件）。"
            "路径相对于 Skill 根目录。",
            {
                {"skill_name", "string", "Skill 名称",                         true},
                {"file_path",  "string", "相对于 Skill 根目录的文件路径", true},
            }
        },
        [](const nlohmann::json& args) -> ToolResult {
            std::string skill_name = args.at("skill_name").get<std::string>();
            std::string file_path  = args.at("file_path").get<std::string>();

            auto& sm = SkillManager::GetInstance();
            const SkillMeta* meta = sm.FindSkill(skill_name);
            if (!meta) {
                return {false, "未找到 Skill: " + skill_name};
            }

            auto content = sm.GetSkillFile(skill_name, file_path);
            if (!content) {
                return {false, "无法读取文件: " + file_path +
                               "（Skill: " + skill_name + "）"};
            }

            std::ostringstream ss;
            ss << "# " << skill_name << " / " << file_path << "\n\n"
               << *content;
            return {true, ss.str()};
        },
        false
    });

    // ════════════════════════════════════════════════════════════════════
    // get_file_outline  — 文件结构摘要（类/函数/行号）
    // ════════════════════════════════════════════════════════════════════
    registry.Register({
        {
            "get_file_outline",
            "获取源代码文件的结构摘要：列出所有类、函数、方法及其行号。"
            "建议在阅读超过 200 行的文件前先调用此工具，"
            "再用 read_file 的 start_line/end_line 精确读取目标片段，避免一次性读入整个大文件。",
            {
                {"path", "string", "文件路径（绝对或相对工作目录）", true},
            }
        },
        [wd](const nlohmann::json& args) -> ToolResult {
            std::string path_str = args.at("path").get<std::string>();
            fs::path path = ResolvePath(path_str, wd);

            if (!fs::exists(path))
                return {false, "文件不存在: " + path_str};
            if (fs::is_directory(path))
                return {false, "路径是目录，请使用 list_directory"};

            // 统计总行数
            int total_lines = 0;
            {
                auto c = utils::ReadFile(path);
                if (c) {
                    for (char ch : *c) if (ch == '\n') ++total_lines;
                    if (!c->empty() && c->back() != '\n') ++total_lines;
                }
            }

            std::string outline;
            outline += "文件: " + path_str + "  共 " +
                       std::to_string(total_lines) + " 行\n";
            outline += std::string(50, '-') + "\n";

            // ── 方案 A：ctags（精准，需要系统安装 universal-ctags 或 exuberant-ctags）
            bool used_ctags = false;
            {
                // 过滤掉太细粒度的 kind（member/variable/enumerator/macro）
                static const std::string kSkipKinds =
                    "member variable enumerator macro local externvar";

                std::string cmd = "ctags -x --sort=no "
                    + utils::EscapeShellArg(path.string()) + " 2>/dev/null";
                FILE* p = popen(cmd.c_str(), "r");
                if (p) {
                    std::string raw;
                    char buf[1024];
                    while (fgets(buf, sizeof(buf), p)) raw += buf;
                    pclose(p);

                    if (!raw.empty()) {
                        std::istringstream ss(raw);
                        std::string line;
                        int count = 0;
                        while (std::getline(ss, line) && count < 300) {
                            std::istringstream ls(line);
                            std::string name, kind, lineno, file;
                            ls >> name >> kind >> lineno >> file;
                            if (name.empty() || lineno.empty()) continue;
                            if (kSkipKinds.find(kind) != std::string::npos) continue;

                            // pattern（源码片段）
                            std::string pat;
                            std::getline(ls, pat);
                            size_t ps = pat.find_first_not_of(" \t");
                            if (ps != std::string::npos) pat = pat.substr(ps);
                            if (pat.size() > 72) pat = pat.substr(0, 72) + "…";

                            // 格式：L 行号  kind  name  — pattern
                            std::string lpad(5 - std::min<size_t>(5, lineno.size()), ' ');
                            outline += "  L" + lpad + lineno
                                    + "  " + kind
                                    + std::string(std::max<int>(1, 12 - (int)kind.size()), ' ')
                                    + name;
                            if (!pat.empty()) outline += "  —  " + pat;
                            outline += "\n";
                            ++count;
                        }
                        if (count > 0) used_ctags = true;
                    }
                }
            }

            // ── 方案 B：grep 语言特定正则（ctags 不可用时的退路）
            if (!used_ctags) {
                std::string ext = path.extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

                // 语言→grep 正则（捕获函数/类/方法定义行）
                std::string pattern;
                if (ext == ".cpp" || ext == ".cxx" || ext == ".cc" ||
                    ext == ".c"   || ext == ".h"   || ext == ".hpp") {
                    pattern = "^(class|struct|enum|union)\\s+\\w"
                              "|^[a-zA-Z_][a-zA-Z0-9_:<>*& ]+[a-zA-Z_][a-zA-Z0-9_]*\\s*\\([^;{]*$";
                } else if (ext == ".py") {
                    pattern = "^\\s*(def|class|async def)\\s+\\w";
                } else if (ext == ".go") {
                    pattern = "^(func|type)\\s+";
                } else if (ext == ".rs") {
                    pattern = "^(pub\\s+)?(fn|struct|impl|trait|enum|type)\\s+";
                } else if (ext == ".java") {
                    pattern = "^\\s*(public|private|protected|static|void|class|interface|enum)";
                } else if (ext == ".js" || ext == ".ts" || ext == ".tsx") {
                    pattern = "^(export\\s+)?(function|class|const\\s+\\w+\\s*=\\s*(async\\s+)?function|interface|type)\\s+";
                } else {
                    // 通用：行首非空白、包含括号或冒号
                    pattern = "^[a-zA-Z_].*[(:{}]\\s*$";
                }

                if (!pattern.empty()) {
                    std::string cmd = "grep -nE " + utils::EscapeShellArg(pattern) +
                                      " " + utils::EscapeShellArg(path.string()) +
                                      " 2>/dev/null | head -200";
                    FILE* p = popen(cmd.c_str(), "r");
                    if (p) {
                        char buf[512];
                        while (fgets(buf, sizeof(buf), p)) {
                            std::string row(buf);
                            // 去尾换行
                            while (!row.empty() && (row.back() == '\n' || row.back() == '\r'))
                                row.pop_back();
                            // row 格式："行号:源码"，截短过长行
                            if (row.size() > 100) row = row.substr(0, 100) + "…";
                            outline += "  " + row + "\n";
                        }
                        pclose(p);
                        outline += "\n（来源：grep 退路，建议安装 ctags 以获取更精准的结构）";
                    }
                }
            }

            if (outline.find('\n', 60) == std::string::npos)
                outline += "\n（未找到可识别的结构，请直接使用 read_file 阅读）";

            return {true, outline};
        },
        false
    });

    // get_file_info  — 文件元数据
    // ════════════════════════════════════════════════════════════════════
    registry.Register({
        {
            "get_file_info",
            "获取文件或目录的元数据（大小、修改时间、类型等）。",
            {
                {"path", "string", "文件路径", true},
            }
        },
        [wd](const nlohmann::json& args) -> ToolResult {
            std::string path_str = args.at("path").get<std::string>();
            fs::path path = ResolvePath(path_str, wd);

            if (!fs::exists(path))
                return {false, "路径不存在: " + path_str};

            std::ostringstream ss;
            ss << "路径: " << path_str << "\n";
            ss << "类型: " << (fs::is_directory(path) ? "目录" : "文件") << "\n";

            if (fs::is_regular_file(path)) {
                std::error_code ec;
                auto sz = fs::file_size(path, ec);
                if (!ec) ss << "大小: " << utils::FormatFileSize(sz) << "\n";
            }

            // 修改时间
            std::error_code ec;
            auto ftime = fs::last_write_time(path, ec);
            if (!ec) {
                auto sctp = std::chrono::time_point_cast<
                    std::chrono::system_clock::duration>(
                    ftime - fs::file_time_type::clock::now() +
                    std::chrono::system_clock::now());
                std::time_t tt = std::chrono::system_clock::to_time_t(sctp);
                char tbuf[64];
                if (std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S",
                                  std::localtime(&tt))) {
                    ss << "修改时间: " << tbuf << "\n";
                }
            }

            return {true, ss.str()};
        },
        false
    });
}

}  // namespace termind
