#include "termind/tool_registry.h"
#include "termind/utils.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <iostream>

#include <sys/wait.h>

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

            std::ostringstream ss;
            ss << "文件: " << path_str << "  ("
               << utils::FormatFileSize(content->size()) << ")\n---\n"
               << *content;
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

            std::string full_cmd = "cd " + utils::EscapeShellArg(wd.string()) +
                                   " && " + command + " 2>&1";

            FILE* pipe = popen(full_cmd.c_str(), "r");
            if (!pipe) return {false, "无法启动命令"};

            std::string output;
            char buf[512];
            while (fgets(buf, sizeof(buf), pipe)) {
                output += buf;
                if (output.size() > 100000) {
                    output += "\n... (输出已截断) ...\n";
                    break;
                }
            }
            int status = pclose(pipe);
            int exit_code = WEXITSTATUS(status);

            std::ostringstream ss;
            ss << "$ " << command << "\n" << output
               << "\n[退出码: " << exit_code << "]";
            return {exit_code == 0, ss.str()};
        },
        true  // 需要确认
    });

    // ════════════════════════════════════════════════════════════════════
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
