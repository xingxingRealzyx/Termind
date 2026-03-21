#include "termind/repl.h"
#include "termind/config.h"
#include "termind/skill_manager.h"
#include "termind/tui.h"
#include "termind/utils.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/table.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

#include <readline/history.h>
#include <readline/readline.h>
#include <unistd.h>

namespace termind {

namespace fs = std::filesystem;

// ── FTXUI 渲染辅助 ────────────────────────────────────────────────────────
// 将 FTXUI Element 渲染到 stdout；indent 指定每行左侧缩进空格数。
static int PrintFTXUI(const ftxui::Element& doc_in, int indent = 2) {
    using namespace ftxui;
    // Fit() takes non-const ref in v6
    auto doc = doc_in;
    int tw = std::max(40, utils::GetTerminalWidth()) - indent - 2;
    tw = std::min(tw, 90);
    auto screen = Screen::Create(Dimension::Fixed(tw), Dimension::Fit(doc));
    Render(screen, doc);
    std::string s   = screen.ToString();
    std::string pad(static_cast<size_t>(std::max(0, indent)), ' ');
    std::istringstream iss(s);
    std::string line;
    int lines = 0;
    while (std::getline(iss, line)) {
        std::cout << pad << line << "\n";
        ++lines;
    }
    std::cout.flush();
    return lines;  // 返回实际输出行数（供 ReadGroup 光标回溯）
}

// ── 简单边框辅助（write/edit/run_shell 等输出段） ─────────────────────────
static void PrintBoxTop(const std::string& label, const std::string& path) {
    int tw = utils::GetTerminalWidth();
    int inner = 5 + static_cast<int>(label.size()) + 2 + static_cast<int>(path.size()) + 1;
    int dashes = std::max(1, tw - inner - 1);
    std::cout << utils::color::kDim << "  ╭─ " << utils::color::kReset
              << utils::color::kBold << label << utils::color::kReset
              << utils::color::kDim << "  " << path << " ";
    for (int i = 0; i < dashes; ++i) std::cout << "─";
    std::cout << "╮" << utils::color::kReset << "\n";
}

static void PrintBoxBottom() {
    int tw = utils::GetTerminalWidth();
    std::cout << utils::color::kDim << "  ╰";
    for (int i = 0; i < tw - 4; ++i) std::cout << "─";
    std::cout << "╯" << utils::color::kReset << "\n";
}

// ── 命令补全列表（供 FTXUI 输入框 ghost-text 使用）──────────────────────
static const char* const kGhostCmds[] = {
    "/file ",    "/files",        "/clearfiles",    "/add ",
    "/clear",    "/tokens",       "/model ",        "/config",
    "/cd ",      "/pwd",          "/memory",        "/memory init",
    "/memory edit", "/memory reload", "/plan ",     "/help",
    "/quit",     "/skills",       "/skills load ",  "/skills reload",
    nullptr
};

// ── @file:N-M 引用解析与注入 ────────────────────────────────────────────
// 用法：用户在提示词中写 @path 或 @path:N 或 @path:N-M，
//       自动将对应行内容注入到发送给 AI 的消息中，并在终端显示摘要卡片。

struct AtRef {
    std::string original;       // @path:N-M
    std::string path;           // 文件路径
    std::string injected_block; // 注入的代码块
    int start_line{1};
    int end_line{1};
    int total_lines{0};
    bool ok{false};
};

static std::vector<AtRef> ParseAtRefs(const std::string& input,
                                       const fs::path& cwd) {
    std::vector<AtRef> refs;
    size_t i = 0;
    bool   prev_ended_at_at = false;  // 上一个 token 因遇到 '@' 而结束

    while (i < input.size()) {
        // '@' 必须在行首、前面是空白，或紧跟在另一个 @ref 之后（无空格分隔的连续引用）
        bool at_boundary = (i == 0) ||
                           std::isspace(static_cast<unsigned char>(input[i - 1])) ||
                           prev_ended_at_at;
        prev_ended_at_at = false;

        if (input[i] == '@' && at_boundary) {
            size_t j = i + 1;
            // 遇到空白 或 遇到下一个 '@' 都结束当前 token
            while (j < input.size() &&
                   !std::isspace(static_cast<unsigned char>(input[j])) &&
                   input[j] != '@')
                ++j;
            std::string token = input.substr(i + 1, j - i - 1);
            if (!token.empty()) {
                AtRef ref;
                ref.original = "@" + token;

                // 尝试解析 :N 或 :N-M 后缀
                std::string path_part = token;
                int start = 0, end = 0;
                size_t colon = token.rfind(':');
                if (colon != std::string::npos) {
                    std::string range = token.substr(colon + 1);
                    if (!range.empty() &&
                        std::isdigit(static_cast<unsigned char>(range[0]))) {
                        size_t dash = range.find('-');
                        try {
                            if (dash != std::string::npos) {
                                start = std::stoi(range.substr(0, dash));
                                end   = std::stoi(range.substr(dash + 1));
                            } else {
                                start = end = std::stoi(range);
                            }
                            path_part = token.substr(0, colon);
                        } catch (...) {}
                    }
                }
                ref.path = path_part;

                fs::path fpath = (path_part[0] == '/')
                                 ? fs::path(path_part)
                                 : cwd / path_part;
                if (auto content = utils::ReadFile(fpath)) {
                    auto all_lines = utils::Split(*content, '\n');
                    int total = static_cast<int>(all_lines.size());
                    int s = (start > 0) ? std::max(1, start) : 1;
                    int e = (end   > 0) ? std::min(total, end) : total;

                    std::ostringstream blk;
                    blk << "\n```" << path_part;
                    if (start > 0)
                        blk << " lines " << s << "-" << e;
                    blk << "\n";
                    for (int li = s; li <= e; ++li)
                        blk << li << "  "
                            << all_lines[static_cast<size_t>(li - 1)] << "\n";
                    blk << "```\n";

                    ref.injected_block = blk.str();
                    ref.start_line  = s;
                    ref.end_line    = e;
                    ref.total_lines = e - s + 1;
                    ref.ok = true;
                }
                refs.push_back(ref);
            }
            i = j;
            // 若 token 以 '@' 结束（紧跟下一个引用），标记下次循环可识别边界
            if (i < input.size() && input[i] == '@')
                prev_ended_at_at = true;
        } else {
            ++i;
        }
    }
    return refs;
}

// 将 @ref 替换为注入块（倒序替换以保持偏移）
static std::string InjectAtRefs(const std::string& input,
                                 const std::vector<AtRef>& refs) {
    std::string result = input;
    for (auto it = refs.rbegin(); it != refs.rend(); ++it) {
        if (!it->ok) continue;
        size_t pos = result.find(it->original);
        if (pos != std::string::npos)
            result.replace(pos, it->original.size(), it->injected_block);
    }
    return result;
}

// ── 工具调用结果 FTXUI 卡片 ───────────────────────────────────────────────
// 在工具执行完毕后，将工具名、关键参数和输出渲染为带边框的紧凑卡片。

static void RenderToolCard(const ToolCallRequest& tc,
                            const ToolResult& result) {
    using namespace ftxui;

    // 工具标签 + 颜色
    struct LabelColor { std::string label; Color color; };
    auto lc = [&]() -> LabelColor {
        if (tc.name == "read_file")        return {"Read",    Color::CyanLight   };
        if (tc.name == "list_directory")   return {"List",    Color::CyanLight   };
        if (tc.name == "get_file_info")    return {"Stat",    Color::CyanLight   };
        if (tc.name == "get_file_outline") return {"Outline", Color::CyanLight   };
        if (tc.name == "search_files")     return {"Search",  Color::YellowLight };
        if (tc.name == "grep_code")        return {"Grep",    Color::YellowLight };
        if (tc.name == "search_symbol")    return {"Symbol",  Color::YellowLight };
        if (tc.name == "run_shell")        return {"Bash",    Color::Yellow      };
        return {"Tool", Color::GrayLight};
    }();

    // 关键参数摘要
    std::string brief;
    if      (tc.arguments.contains("command")) brief = tc.arguments["command"].get<std::string>();
    else if (tc.arguments.contains("path")   ) brief = tc.arguments["path"].get<std::string>();
    else if (tc.arguments.contains("pattern")) brief = tc.arguments["pattern"].get<std::string>();
    else if (tc.arguments.contains("query")  ) brief = tc.arguments["query"].get<std::string>();
    // 截断过长 brief
    const int kMaxBrief = 60;
    if (static_cast<int>(brief.size()) > kMaxBrief)
        brief = brief.substr(0, static_cast<size_t>(kMaxBrief)) + "…";

    std::vector<Element> rows;
    rows.push_back(hbox({
        text(lc.label) | bold | color(lc.color),
        text("  " + brief) | dim,
    }));

    // 内容行
    // read_file / get_file_outline 成功时只显示行数，不显示内容
    bool content_only_count =
        (tc.name == "read_file" || tc.name == "get_file_outline") && result.success;

    if (content_only_count) {
        auto lines = utils::Split(result.output, '\n');
        rows.push_back(separator());
        rows.push_back(
            text(std::to_string(lines.size()) + " 行") | dim
        );
    } else {
        auto lines = utils::Split(result.output, '\n');
        while (!lines.empty() && utils::Trim(lines.back()).empty())
            lines.pop_back();
        const int kMaxShow = 8;
        int show = std::min(kMaxShow, static_cast<int>(lines.size()));
        if (show > 0) {
            rows.push_back(separator());
            Color tc_color = result.success ? Color() : Color::Red;
            for (int i = 0; i < show; ++i)
                rows.push_back(text(lines[static_cast<size_t>(i)])
                                | color(tc_color) | dim);
            if (static_cast<int>(lines.size()) > show) {
                rows.push_back(
                    text("… 还有 " +
                         std::to_string(lines.size() - show) + " 行") | dim
                );
            }
        }
    }

    PrintFTXUI(vbox(rows) | border, 2);
}

// ── 项目记忆默认模板 ──────────────────────────────────────────────────────
static const char* kMemoryTemplate = R"(# 项目记忆

<!-- Termind 每次启动时自动加载此文件，向 AI 提供持久化的项目背景知识 -->

## 项目简介

<!-- 项目目的、技术栈、主要功能 -->

## 构建与运行

```bash
# 构建命令
# make -j$(nproc)

# 运行命令
# ./build/myapp
```

## 代码规范

<!-- 编码风格、命名约定、注意事项 -->

## 架构说明

<!-- 主要模块及职责 -->

## 常用路径

<!-- 关键文件、配置文件位置 -->

## 注意事项

<!-- 已知问题、需要避免的操作、特殊依赖 -->
)";

// ── 构造 / 析构 ───────────────────────────────────────────────────────────

Repl::Repl() : ai_client_(std::make_unique<AiClient>()) {
    // 工作目录默认为当前目录
    context_.SetWorkingDir(fs::current_path());

    // 保存基础 system prompt（配置文件 > 默认值）
    const auto& cfg = ConfigManager::GetInstance().config();
    base_system_prompt_ = cfg.system_prompt.empty()
                              ? context_.GetSystemMessage()  // 使用 ContextManager 默认值
                              : cfg.system_prompt;

    // 注册内置工具
    RegisterBuiltinTools(ToolRegistry::GetInstance(),
                         context_.GetWorkingDir().string());

    // ── 初始化 Skills（会调用 RebuildSystemMessage）──────────────────
    InitSkills();

    // ── 初始化项目记忆（会调用 RebuildSystemMessage）────────────────
    InitProjectMemory();

    InitReadline();
}

Repl::~Repl() {
    // 保存 readline 历史
    std::string hist_path =
        utils::ExpandHome("~/.config/termind/history");
    write_history(hist_path.c_str());
}

// ── System Message 重建 ───────────────────────────────────────────────────
//
// 组装顺序：基础提示词 → Skills 摘要 → 项目记忆（TERMIND.md）

void Repl::RebuildSystemMessage() {
    std::string sys = base_system_prompt_;

    // 追加构建系统信息
    if (!build_info_.empty()) {
        sys += build_info_;
    }

    // 追加 Skills 摘要
    auto& sm = SkillManager::GetInstance();
    if (sm.HasSkills()) {
        sys += "\n\n" + sm.GetSummaryBlock();
    }

    // 追加项目记忆
    if (!project_memory_content_.empty()) {
        sys += "\n\n---\n## 项目记忆（TERMIND.md）\n\n";
        sys += project_memory_content_;
        sys += "\n\n> **记忆更新指引**：当对话中出现以下内容时，"
               "主动调用 `update_project_memory` 工具将其保存到 TERMIND.md：\n"
               "> - 构建/运行/测试命令\n"
               "> - 代码规范与命名约定\n"
               "> - 架构决策或模块说明\n"
               "> - 重要文件/配置路径\n"
               "> - 已知问题或特殊依赖\n"
               "> 每次对话结束前，若发现值得长期记住的信息，请主动更新。\n";
    }

    context_.SetSystemMessage(sys);
}

// ── Skills 初始化 ─────────────────────────────────────────────────────────

void Repl::InitSkills() {
    const auto& cfg = ConfigManager::GetInstance().config();
    auto& sm = SkillManager::GetInstance();
    sm.Clear();

    // 扫描目录：
    //   1. 配置文件中指定的路径
    //   2. ~/.config/termind/skills/（全局）
    //   3. <工作目录>/.termind/skills/（项目本地）
    std::vector<fs::path> dirs;

    for (const auto& d : cfg.skills_dirs) {
        dirs.push_back(utils::ExpandHome(d));
    }
    dirs.push_back(utils::ExpandHome("~/.config/termind/skills"));
    dirs.push_back(context_.GetWorkingDir() / ".termind" / "skills");

    sm.LoadFromDirs(dirs);
    // 注意：调用方（构造函数）在 InitProjectMemory 之后统一调用 RebuildSystemMessage
    // 此处不单独重建，避免后续又被 InitProjectMemory 覆盖
}

// ── 项目记忆 ──────────────────────────────────────────────────────────────

fs::path Repl::FindProjectMemoryPath() const {
    fs::path cwd = context_.GetWorkingDir();

    // 第一步：向上找 git 根，确定搜索上界
    fs::path git_root;
    {
        fs::path d = cwd;
        for (int i = 0; i < 20; ++i) {
            if (fs::exists(d / ".git")) { git_root = d; break; }
            fs::path p = d.parent_path();
            if (p == d) break;
            d = p;
        }
    }

    // 第二步：在 [cwd, git_root] 范围内搜索 TERMIND.md
    // 若没有 git 根，只看 cwd 本身（不向上走，避免误用其他项目的记忆）
    fs::path d = cwd;
    for (int i = 0; i < 20; ++i) {
        if (fs::exists(d / "TERMIND.md")) return d / "TERMIND.md";
        if (git_root.empty()) break;   // 无 git 根：只看 cwd
        if (d == git_root) break;      // 已到 git 根，停止
        fs::path p = d.parent_path();
        if (p == d) break;
        d = p;
    }
    return {};
}

void Repl::InitProjectMemory() {
    project_memory_path_    = FindProjectMemoryPath();
    project_memory_content_ = "";

    if (!project_memory_path_.empty()) {
        auto content = utils::ReadFile(project_memory_path_);
        if (content) {
            project_memory_content_ = *content;
        }
    } else {
        // 首次启动，自动在 git 根（或当前目录）创建默认 TERMIND.md
        fs::path create_dir = context_.GetWorkingDir();

        // 向上找 git 根
        fs::path d = create_dir;
        for (int i = 0; i < 10; ++i) {
            if (fs::exists(d / ".git")) { create_dir = d; break; }
            fs::path p = d.parent_path();
            if (p == d) break;
            d = p;
        }

        fs::path target = create_dir / "TERMIND.md";
        if (utils::WriteFile(target, kMemoryTemplate)) {
            project_memory_path_    = target;
            project_memory_content_ = kMemoryTemplate;
            std::cout << utils::color::kDim
                      << "  📋 已在 " << target.string()
                      << " 创建 TERMIND.md\n"
                      << utils::color::kReset;
        }
    }

    build_info_ = DetectBuildSystem();

    RebuildSystemMessage();
    RegisterMemoryTool();
}

// ── 构建系统探测 ──────────────────────────────────────────────────────────

std::string Repl::DetectBuildSystem() const {
    const fs::path& wd = context_.GetWorkingDir();

    struct BuildSpec {
        std::string marker;     // 检测文件名
        std::string name;       // 构建系统名
        std::string build_cmd;  // 默认构建命令
        std::string test_cmd;   // 默认测试命令
    };

    // 按优先级排列；CMake 需特殊处理（区分有无 build 目录）
    const std::vector<BuildSpec> specs = {
        {"CMakeLists.txt", "CMake",  "",              ""},
        {"Makefile",       "Make",   "make",           "make test"},
        {"GNUmakefile",    "Make",   "make",           "make test"},
        {"Cargo.toml",     "Cargo",  "cargo build",    "cargo test"},
        {"package.json",   "npm",    "npm run build",  "npm test"},
        {"pyproject.toml", "Python", "pip install -e .","pytest"},
        {"setup.py",       "Python", "python setup.py build", "pytest"},
        {"go.mod",         "Go",     "go build ./...", "go test ./..."},
        {"build.gradle",   "Gradle", "./gradlew build","./gradlew test"},
        {"pom.xml",        "Maven",  "mvn compile",    "mvn test"},
        {"meson.build",    "Meson",  "meson compile -C build", "meson test -C build"},
    };

    for (const auto& spec : specs) {
        if (!fs::exists(wd / spec.marker)) continue;

        std::string build_cmd = spec.build_cmd;
        std::string test_cmd  = spec.test_cmd;

        // CMake：根据是否已有 build 目录给出不同命令
        if (spec.name == "CMake") {
            if (fs::exists(wd / "build" / "CMakeCache.txt")) {
                build_cmd = "cmake --build build";
                test_cmd  = "ctest --test-dir build";
            } else if (fs::exists(wd / "build")) {
                build_cmd = "cmake --build build";
                test_cmd  = "ctest --test-dir build";
            } else {
                build_cmd = "cmake -B build && cmake --build build";
                test_cmd  = "cmake -B build && cmake --build build && ctest --test-dir build";
            }
        }

        // npm：优先用 yarn（若 yarn.lock 存在）
        if (spec.name == "npm" && fs::exists(wd / "yarn.lock")) {
            build_cmd = "yarn build";
            test_cmd  = "yarn test";
        }

        std::ostringstream ss;
        ss << "\n\n---\n## 当前项目构建信息（自动探测）\n\n"
           << "- **构建系统**：" << spec.name << "（检测到 `" << spec.marker << "`）\n"
           << "- **构建命令**：`" << build_cmd << "`\n";
        if (!test_cmd.empty())
            ss << "- **测试命令**：`" << test_cmd << "`\n";
        ss << "\n验证代码修改后，请使用上述命令编译确认无误。";
        return ss.str();
    }
    return "";
}

// ── update_project_memory 工具注册 ───────────────────────────────────────
//
// 此工具捕获 this 指针，在 AI 对话中按段更新 TERMIND.md。
// 若 TERMIND.md 不存在，工具以禁用形式注册（调用时报错提示创建）。
//
// section 匹配规则（大小写不敏感，前缀匹配）：
//   - "构建" 匹配 "## 构建与运行"
//   - 若找不到匹配段，追加为新段
//
// 更新流程：
//   1. 解析 TERMIND.md 各段
//   2. 显示 diff（删除行红色，新增行绿色）
//   3. 请求用户确认
//   4. 写入文件并 reload

static std::string UpdateSection(const std::string& doc,
                                  const std::string& section_title,
                                  const std::string& new_content) {
    // 将文档按 "## " 头分割
    // 找到完整匹配的 section（大小写不敏感）
    std::string lower_title = section_title;
    std::transform(lower_title.begin(), lower_title.end(),
                   lower_title.begin(), ::tolower);

    // 逐行扫描，定位 section 的起止
    auto lines = utils::Split(doc, '\n');
    int section_start = -1;  // ## 行的下标
    int section_end   = static_cast<int>(lines.size());  // 下一个 ## 行（不含）

    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        const std::string& ln = lines[i];
        if (ln.size() >= 3 && ln[0] == '#' && ln[1] == '#' && ln[2] == ' ') {
            if (section_start == -1) {
                // 检查是否匹配
                std::string hdr = ln.substr(3);
                std::string lower_hdr = hdr;
                std::transform(lower_hdr.begin(), lower_hdr.end(),
                               lower_hdr.begin(), ::tolower);
                if (lower_hdr.find(lower_title) != std::string::npos ||
                    lower_title.find(lower_hdr) != std::string::npos) {
                    section_start = i;
                }
            } else {
                // 找到了 section_start，这行是下一个 section
                section_end = i;
                break;
            }
        }
    }

    std::ostringstream result;
    if (section_start == -1) {
        // 未找到对应段，追加到文档末尾
        result << doc;
        if (!doc.empty() && doc.back() != '\n') result << "\n";
        result << "\n## " << section_title << "\n\n" << new_content << "\n";
    } else {
        // 重建文档：section 头 + 新内容 + 剩余
        for (int i = 0; i < section_start; ++i)
            result << lines[i] << "\n";
        result << lines[section_start] << "\n\n";  // ## 标题行
        result << new_content;
        if (!new_content.empty() && new_content.back() != '\n') result << "\n";
        result << "\n";
        for (int i = section_end; i < static_cast<int>(lines.size()); ++i)
            result << lines[i] << "\n";
    }
    return result.str();
}

void Repl::RegisterMemoryTool() {
    // 注册工具，捕获 this（Repl 生命周期覆盖所有工具调用）
    ToolRegistry::GetInstance().Register({
        {
            "update_project_memory",
            "将对话中发现的重要项目信息写入 TERMIND.md（项目记忆文件）。"
            "当发现以下类型的信息时主动调用：构建/运行/测试命令、代码规范与约定、"
            "架构决策、重要文件路径、已知问题或特殊依赖。"
            "会显示改动 diff 并请求用户确认后再写入。",
            {
                {"section",  "string",
                 "要更新的章节标题，如【构建与运行】、【注意事项】、【架构说明】等，"
                 "大小写不敏感，支持前缀匹配。若章节不存在将自动追加。",
                 true},
                {"content",  "string",
                 "该章节的完整新内容（Markdown 格式，不含 ## 标题行）。",
                 true},
            }
        },
        [this](const nlohmann::json& args) -> ToolResult {
            if (project_memory_path_.empty()) {
                return {false,
                        "当前目录没有 TERMIND.md，请先执行 /memory init 创建。"};
            }

            std::string section = args.at("section").get<std::string>();
            std::string content = args.at("content").get<std::string>();

            // 生成更新后的文档
            std::string updated = UpdateSection(
                project_memory_content_, section, content);

            // 显示 diff（简单行级：删去旧行、加入新行）
            std::cout << "\n" << utils::color::kBold
                      << "  📋 update_project_memory [" << section << "]"
                      << utils::color::kReset << "\n\n";

            auto old_lines = utils::Split(project_memory_content_, '\n');
            auto new_lines = utils::Split(updated, '\n');

            // 只显示变化的上下文（简单差异：行集合对比）
            std::set<std::string> old_set(old_lines.begin(), old_lines.end());
            std::set<std::string> new_set(new_lines.begin(), new_lines.end());

            bool any_diff = false;
            for (const auto& ln : old_lines) {
                if (!new_set.count(ln)) {
                    std::cout << utils::color::kRed << "  - " << ln
                              << utils::color::kReset << "\n";
                    any_diff = true;
                }
            }
            for (const auto& ln : new_lines) {
                if (!old_set.count(ln)) {
                    std::cout << utils::color::kGreen << "  + " << ln
                              << utils::color::kReset << "\n";
                    any_diff = true;
                }
            }

            if (!any_diff) {
                return {true, "内容与现有记忆相同，无需更新。"};
            }
            std::cout << "\n";

            // 确认
            char choice = utils::AskYesNoEdit("将此变更写入 TERMIND.md?");
            std::cout << "\n";
            if (choice == 'n') {
                return {false, "用户已取消记忆更新。"};
            }

            // 写入
            if (!utils::WriteFile(project_memory_path_, updated)) {
                return {false, "写入 TERMIND.md 失败: " +
                               project_memory_path_.string()};
            }

            // 热更新
            project_memory_content_ = updated;
            RebuildSystemMessage();

            return {true, "✅ TERMIND.md [" + section + "] 已更新"};
        },
        false  // 工具内部自行处理确认，不走外部 requires_confirmation 流程
    });
}

// ── readline 初始化 ───────────────────────────────────────────────────────

void Repl::InitReadline() {
    // readline 保留用于历史目录创建（FTXUI 自管历史，不调用 readline/add_history）
    std::string hist_dir = utils::ExpandHome("~/.config/termind");
    fs::create_directories(hist_dir);
}

// ── 读取一行输入（FTXUI 交互式输入框）──────────────────────────────────────
// 替代原 readline 实现：提供多行编辑、幽灵补全、历史回溯等功能。

std::string Repl::ReadLine(const std::string& /*prompt*/) {
    using namespace ftxui;

    // ── 历史持久化（替代 readline history）───────────────────────────────
    static std::vector<std::string> s_hist;
    static bool                     s_hist_loaded = false;
    const std::string kHistFile = utils::ExpandHome("~/.config/termind/history");
    if (!s_hist_loaded) {
        s_hist_loaded = true;
        if (auto data = utils::ReadFile(kHistFile)) {
            s_hist = utils::Split(*data, '\n');
            while (!s_hist.empty() && s_hist.back().empty())
                s_hist.pop_back();
        }
    }
    auto save_hist = [&](const std::string& entry) {
        if (entry.empty()) return;
        if (!s_hist.empty() && s_hist.back() == entry) return; // 去重
        s_hist.push_back(entry);
        if (s_hist.size() > 1000) s_hist.erase(s_hist.begin());
        std::string data;
        for (const auto& h : s_hist) data += h + "\n";
        utils::WriteFile(kHistFile, data);
    };

    // ── 状态 ──────────────────────────────────────────────────────────────
    std::string content;
    bool        submitted  = false;
    int         hist_idx   = -1;
    std::string saved_buf; // 历史导航前的缓冲区快照

    auto screen  = ScreenInteractive::FitComponent();
    auto exit_fn = screen.ExitLoopClosure();

    // ── 计算命令候选列表（/ 命令 + @ 文件引用补全） ─────────────────────────
    // 命令说明表
    static const std::pair<const char*, const char*> kCmdDescs[] = {
        {"/file ",          "插入文件内容"},
        {"/files",          "已插入文件"},
        {"/clearfiles",     "清除插入文件"},
        {"/add ",           "添加到上下文"},
        {"/clear",          "清除对话历史"},
        {"/tokens",         "Token 用量"},
        {"/model ",         "切换模型"},
        {"/config",         "查看配置"},
        {"/cd ",            "切换目录"},
        {"/pwd",            "当前目录"},
        {"/memory",         "项目记忆"},
        {"/memory init",    "初始化记忆"},
        {"/memory edit",    "编辑记忆"},
        {"/memory reload",  "重载记忆"},
        {"/plan ",          "执行规划"},
        {"/help",           "查看帮助"},
        {"/quit",           "退出"},
        {"/skills",         "技能列表"},
        {"/skills load ",   "加载技能"},
        {"/skills reload",  "重载技能"},
        {nullptr, nullptr}
    };
    auto get_cmd_desc = [](const std::string& cmd) -> std::string {
        for (int i = 0; kCmdDescs[i].first; ++i) {
            if (std::string(kCmdDescs[i].first) == cmd) return kCmdDescs[i].second;
        }
        return "";
    };

    // 返回当前输入对应的补全候选（完整替换后的 content 字符串）
    auto get_suggestions = [&]() -> std::vector<std::string> {
        std::vector<std::string> res;
        if (content.empty() || content.find('\n') != std::string::npos) return res;

        // ── 命令补全：整行以 / 开头 ──────────────────────────────────────
        if (content[0] == '/') {
            for (int i = 0; kGhostCmds[i]; ++i) {
                std::string c(kGhostCmds[i]);
                if (c.size() >= content.size() && c.substr(0, content.size()) == content)
                    res.push_back(c);  // 命令数量有限，不加条数限制
            }
            return res;
        }

        // ── 文件引用补全：content 末尾含 @ ───────────────────────────────
        size_t at_pos = content.rfind('@');
        if (at_pos == std::string::npos) return res;
        std::string after_at = content.substr(at_pos + 1);
        if (after_at.find(' ') != std::string::npos) return res;  // @ 后有空格则放弃

        fs::path search_dir = context_.GetWorkingDir();
        std::string prefix_dir, file_prefix;
        size_t slash = after_at.rfind('/');
        if (slash != std::string::npos) {
            prefix_dir  = after_at.substr(0, slash + 1);
            file_prefix = after_at.substr(slash + 1);
            search_dir  = search_dir / prefix_dir;
        } else {
            file_prefix = after_at;
        }

        try {
            std::vector<std::string> dirs_v, files_v;
            for (const auto& entry : fs::directory_iterator(search_dir)) {
                std::string name = entry.path().filename().string();
                if (name.empty() || name[0] == '.') continue;
                if (!file_prefix.empty()) {
                    if (name.size() < file_prefix.size()) continue;
                    if (name.substr(0, file_prefix.size()) != file_prefix) continue;
                }
                std::string full_after = prefix_dir + name;
                if (entry.is_directory()) dirs_v.push_back(full_after + "/");
                else                     files_v.push_back(full_after);
            }
            for (const auto& d : dirs_v)  { res.push_back(content.substr(0, at_pos + 1) + d);  if (res.size() >= 16) break; }
            for (const auto& f : files_v) { res.push_back(content.substr(0, at_pos + 1) + f);  if (res.size() >= 16) break; }
        } catch (...) {}

        return res;
    };

    // 旧的单行幽灵补全（仅用于 Tab 后退到该路径的兼容，渲染侧已改用下拉列表）
    auto get_ghost = [&]() -> std::string {
        if (content.empty() || content[0] != '/') return "";
        if (content.find('\n') != std::string::npos)  return "";
        for (int i = 0; kGhostCmds[i]; ++i) {
            std::string c(kGhostCmds[i]);
            if (c.size() > content.size() &&
                c.substr(0, content.size()) == content)
                return c.substr(content.size());
        }
        return "";
    };
    (void)get_ghost;  // 已被 get_suggestions 取代，保留备用

    // ── Input component ────────────────────────────────────────────────────
    int  cursor_pos   = 0;   // FTXUI Input 光标位置（与 iopt.cursor_position 绑定）
    int  sugg_sel_idx = -1;  // 当前高亮的补全候选索引（-1 = 未选中）
    int  sugg_scroll  = 0;   // 下拉列表可见窗口的起始偏移
    static constexpr int kSuggVisible = 8;  // 一次最多显示的候选条数

    InputOption iopt;
    iopt.multiline        = true;
    iopt.cursor_position  = &cursor_pos;
    // 去掉默认的 inverted（白底）样式，保持透明背景
    iopt.transform = [](InputState state) -> Element {
        if (state.is_placeholder) state.element |= dim;
        return state.element;
    };
    // 用户手动输入字符时重置下拉列表选中状态
    iopt.on_change = [&]() { sugg_sel_idx = -1; sugg_scroll = 0; };
    auto inp = Input(&content, "", iopt);

    auto handler = CatchEvent(inp, [&](Event ev) -> bool {
        // Enter → 提交（不插入换行）
        if (ev == Event::Return) {
            std::string t = utils::Trim(content);
            if (!t.empty()) { submitted = true; exit_fn(); }
            return true;  // 消费：不让 Input 插入 '\n'
        }

        // Ctrl+D → 退出 REPL
        if (ev.is_character() && ev.character() == "\x04") {
            running_ = false;
            exit_fn();
            return true;
        }

        // Ctrl+C → 清空或退出
        if (ev == Event::Special("\x03")) {
            if (!content.empty()) {
                content.clear();
                cursor_pos   = 0;
                sugg_sel_idx = -1;
                sugg_scroll  = 0;
                hist_idx     = -1;
            } else {
                running_ = false;
                exit_fn();
            }
            return true;
        }

        // Escape → 清空输入
        if (ev == Event::Escape) {
            content.clear();
            cursor_pos   = 0;
            sugg_sel_idx = -1;
            sugg_scroll  = 0;
            hist_idx     = -1;
            return true;
        }

        // Alt+Enter (ESC+Enter) → 插入换行
        if (ev == Event::Special("\033\r") || ev == Event::Special("\033\n")) {
            content += "\n";
            cursor_pos = static_cast<int>(content.size());
            return true;
        }

        // Tab → 接受当前高亮候选（或第一个）
        if (ev == Event::Tab) {
            auto suggs = get_suggestions();
            if (!suggs.empty()) {
                int idx = (sugg_sel_idx >= 0 && sugg_sel_idx < static_cast<int>(suggs.size()))
                          ? sugg_sel_idx : 0;
                content      = suggs[static_cast<size_t>(idx)];
                cursor_pos   = static_cast<int>(content.size());
                sugg_sel_idx = -1;
                hist_idx     = -1;
                return true;
            }
            return false;
        }

        // ↓ 键 —— 有补全列表时导航列表；否则不做历史（避免与补全冲突）
        if (ev == Event::ArrowDown && content.find('\n') == std::string::npos) {
            auto suggs = get_suggestions();
            if (!suggs.empty()) {
                int n = static_cast<int>(suggs.size());
                sugg_sel_idx = (sugg_sel_idx + 1) % n;
                if (sugg_sel_idx >= sugg_scroll + kSuggVisible)
                    sugg_scroll = sugg_sel_idx - kSuggVisible + 1;
                if (sugg_sel_idx < sugg_scroll)
                    sugg_scroll = sugg_sel_idx;
                return true;
            }
            return false;  // 无候选：不处理，让 Input 组件内部处理
        }

        // ↑ 键 —— 有补全列表时向上导航；否则不做历史
        if (ev == Event::ArrowUp && content.find('\n') == std::string::npos) {
            auto suggs = get_suggestions();
            if (!suggs.empty() && sugg_sel_idx >= 0) {
                --sugg_sel_idx;
                if (sugg_sel_idx >= 0 && sugg_sel_idx < sugg_scroll)
                    sugg_scroll = sugg_sel_idx;
                if (sugg_sel_idx < 0) sugg_scroll = 0;
                return true;
            }
            return false;  // 无候选：不处理
        }

        // ← 在光标行首：历史回溯（单行时）
        if (ev == Event::ArrowLeft &&
            cursor_pos == 0 &&
            content.find('\n') == std::string::npos) {
            if (hist_idx == -1) {
                saved_buf = content;
                hist_idx  = static_cast<int>(s_hist.size());
            }
            if (hist_idx > 0) {
                --hist_idx;
                content    = s_hist[static_cast<size_t>(hist_idx)];
                cursor_pos = static_cast<int>(content.size());
            }
            sugg_sel_idx = -1;
            sugg_scroll  = 0;
            return true;
        }

        // → 在光标行尾且处于历史浏览模式：历史前进
        if (ev == Event::ArrowRight &&
            hist_idx != -1 &&
            cursor_pos == static_cast<int>(content.size()) &&
            content.find('\n') == std::string::npos) {
            ++hist_idx;
            if (hist_idx >= static_cast<int>(s_hist.size())) {
                hist_idx = -1;
                content  = saved_buf;
            } else {
                content  = s_hist[static_cast<size_t>(hist_idx)];
            }
            cursor_pos = static_cast<int>(content.size());
            return true;
        }

        return false;
    });

    // ── 渲染器 ────────────────────────────────────────────────────────────
    const auto& cfg    = ConfigManager::GetInstance().config();
    std::string cwd_nm = context_.GetWorkingDir().filename().string();
    if (cwd_nm.empty()) cwd_nm = "/";

    auto make_badge = [&]() -> Element {
        size_t est = context_.EstimateTokens();
        if (cfg.max_context_tokens > 0 && est > 0) {
            int pct = std::min(100, static_cast<int>(
                          est * 100 / static_cast<size_t>(cfg.max_context_tokens)));
            Color c = (pct < 60) ? Color::GreenLight
                    : (pct < 80) ? Color::Yellow
                                 : Color::Red;
            return hbox({
                text(std::to_string(pct) + "%") | color(c),
                text("  "),
            });
        }
        return text("");
    };

    auto renderer = Renderer(handler, [&] {
        auto suggs = get_suggestions();

        std::vector<Element> box;

        // ❯  + 输入区（高度上限 8 行，防止超出终端导致 ScreenInteractive 跳屏）
        box.push_back(hbox({
            text(" ❯ ") | bold | color(Color::CyanLight),
            handler->Render() | flex | size(HEIGHT, LESS_THAN, 4),
        }));

        // 建议下拉列表（命令列表 / 文件引用补全）
        if (!suggs.empty()) {
            box.push_back(separator());
            bool is_cmd_list = !content.empty() && content[0] == '/';
            int total = static_cast<int>(suggs.size());

            // 保证 sugg_scroll 不越界
            int max_scroll = std::max(0, total - kSuggVisible);
            if (sugg_scroll > max_scroll) sugg_scroll = max_scroll;

            int win_start = sugg_scroll;
            int win_end   = std::min(total, sugg_scroll + kSuggVisible);

            // 上方还有更多时显示 "↑ N more"
            if (win_start > 0) {
                box.push_back(hbox({
                    text("  ↑ " + std::to_string(win_start) + " more") | dim,
                    filler(),
                }));
            }

            for (int i = win_start; i < win_end; ++i) {
                const auto& sugg = suggs[static_cast<size_t>(i)];
                bool selected = (i == sugg_sel_idx);
                std::string display;
                std::string desc;
                if (is_cmd_list) {
                    display = sugg;
                    desc    = get_cmd_desc(sugg);
                } else {
                    size_t ap = sugg.rfind('@');
                    display = (ap != std::string::npos) ? sugg.substr(ap) : sugg;
                }
                auto row = hbox({
                    text("  "),
                    text(display) | (selected ? bold : dim),
                    text(desc.empty() ? "" : "  " + desc) | dim,
                    filler(),
                });
                if (selected) row = row | inverted;
                box.push_back(row);
            }

            // 下方还有更多时显示 "↓ N more"
            int remaining = total - win_end;
            if (remaining > 0) {
                box.push_back(hbox({
                    text("  ↓ " + std::to_string(remaining) + " more") | dim,
                    filler(),
                }));
            }
        }

        // 状态行（cwd · token% · 快捷键提示）
        box.push_back(hbox({
            text(" "),
            make_badge(),
            text(cwd_nm) | dim,
            text("   ") | dim,
            text("Enter") | bold | dim,
            text(" 发送") | dim,
            text("  ·  ") | dim,
            text("Alt+Enter") | bold | dim,
            text(" 换行") | dim,
            text("  ·  ") | dim,
            text("←→") | bold | dim,
            text(" 历史") | dim,
            suggs.empty() ? text("") : hbox({
                text("  ·  ") | dim,
                text("↑↓") | bold | dim,
                text(" 选择") | dim,
                text("  ") | dim,
                text("Tab") | bold | dim,
                text(" 补全") | dim,
            }),
            filler(),
        }));

        // 总高度不超过终端高度的一半，避免 FitComponent 触发全屏清空
        // 宽度固定为终端宽度：防止内容缩短时 min_x < 上次宽度，
        // 触发 FTXUI FitComponent 的 "\033[H\033[J" 清屏逻辑（见 screen_interactive.cpp:939）
        auto termSz  = ftxui::Terminal::Size();
        int  safe_h  = std::max(6, termSz.dimy / 2);
        return vbox(box) | border
            | size(HEIGHT, LESS_THAN, safe_h)
            | size(WIDTH,  EQUAL,     termSz.dimx);
    });

    // 打印一个空行确保光标在新的一行，然后直接启动
    std::cout << std::flush;
    screen.Loop(renderer);

    if (!submitted) return "";
    std::string result = utils::Trim(content);
    save_hist(result);
    return result;
}

// ── 启动 REPL ─────────────────────────────────────────────────────────────

void Repl::Run() {
    PrintWelcome();

    while (running_) {
        std::string input = ReadLine("");
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

    // ── @file 引用解析 ──────────────────────────────────────────────────
    auto refs = ParseAtRefs(input, context_.GetWorkingDir());
    std::string message = input;
    bool has_refs = false;
    for (const auto& ref : refs) {
        if (ref.ok) {
            has_refs = true;
            using namespace ftxui;
            std::string label = ref.path;
            if (ref.start_line > 0 || ref.end_line < ref.total_lines + ref.start_line - 1) {
                label += ":" + std::to_string(ref.start_line)
                       + "-" + std::to_string(ref.end_line);
            }
            auto doc = hbox({
                text(" ⊕ ") | bold | color(Color::CyanLight),
                text(label) | bold,
                text("  (" + std::to_string(ref.total_lines) + " 行)") | dim,
            }) | border;
            PrintFTXUI(doc, 2);
        } else {
            std::cout << utils::color::kYellow << "  ⚠ " << utils::color::kReset
                      << "文件未找到: " << ref.path << "\n";
        }
    }
    if (has_refs) {
        message = InjectAtRefs(input, refs);
        std::cout << "\n";
    }

    RunAgentLoop(message, false);
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
    if (cmd == "skills"  || cmd == "skill")         { HandleSkills(args);    return true; }
    if (cmd == "memory"  || cmd == "mem")           { HandleMemory(args);    return true; }
    if (cmd == "plan"    || cmd == "p")             { HandlePlan(args);      return true; }

    utils::PrintWarning("未知命令: /" + cmd + "。输入 /help 查看帮助。");
    return true;
}

// ── 斜杠命令实现 ──────────────────────────────────────────────────────────

void Repl::HandleHelp() {
    using namespace ftxui;

    // 每行命令：固定宽度左列 + dim 右列描述
    auto row = [](const std::string& cmd, const std::string& desc) -> Element {
        return hbox({
            text("  " + cmd) | bold | size(WIDTH, EQUAL, 28),
            text(desc) | dim,
        });
    };
    // 章节标题
    auto sect = [](const std::string& title) -> Element {
        return hbox({text(title) | bold | color(Color::CyanLight)});
    };

    auto doc = vbox({
        hbox({
            text("◆ Termind 命令") | bold | color(Color::CyanLight),
            text("  直接输入自然语言即可与 AI 对话") | dim,
        }),
        separator(),

        sect("对话与上下文"),
        row("/file <路径>",        "将文件加入 AI 上下文"),
        row("/files",              "列出上下文中的文件"),
        row("/clearfiles",         "清除文件上下文"),
        row("/add <内容>",         "附加文字片段到上下文"),
        row("/clear",              "清除对话历史"),
        row("/tokens",             "显示预估 token 用量"),
        separator(),

        sect("模型与配置"),
        row("/model <名称>",       "切换 AI 模型"),
        row("/config",             "查看当前配置"),
        separator(),

        sect("目录"),
        row("/cd <路径>",          "切换工作目录"),
        row("/pwd",                "显示当前工作目录"),
        separator(),

        sect("项目记忆"),
        row("/memory",             "显示当前 TERMIND.md"),
        row("/memory init",        "在当前目录创建 TERMIND.md 模板"),
        row("/memory edit",        "用 $EDITOR 打开 TERMIND.md"),
        row("/memory reload",      "重新加载 TERMIND.md"),
        separator(),

        sect("Skills"),
        row("/skills",             "列出所有可用 Skills"),
        row("/skills load <name>", "手动加载 Skill"),
        row("/skills reload",      "重新扫描 Skills 目录"),
        separator(),

        sect("规划 & 其他"),
        row("/plan <任务>",        "先输出执行计划，确认后执行  (别名: /p)"),
        row("/help",               "显示此帮助"),
        row("/quit",               "退出"),
    }) | border;

    std::cout << "\n";
    PrintFTXUI(doc, 2);
    std::cout << "\n";
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
              << "  工具调用上限:  " << cfg.max_tool_iterations << "\n"
              << "  上下文压缩阈值: "
              << (cfg.max_context_tokens > 0
                      ? std::to_string(cfg.max_context_tokens) + " tokens"
                      : "禁用") << "\n"
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

    // 重新加载新工作目录的项目记忆
    bool had_memory = !project_memory_path_.empty();
    InitProjectMemory();
    bool has_memory = !project_memory_path_.empty();

    std::string msg = "工作目录: " + new_dir.string();
    if (has_memory) {
        msg += "\n  📋 已加载项目记忆: " + project_memory_path_.string();
    } else if (had_memory) {
        msg += "\n  📋 项目记忆已卸载（当前目录无 TERMIND.md）";
    }
    utils::PrintSuccess(msg);
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
    const auto& cfg = ConfigManager::GetInstance().config();
    size_t est  = context_.EstimateTokens();
    int limit   = cfg.max_context_tokens;

    std::cout << utils::color::kCyan << "上下文统计" << utils::color::kReset << "\n"
              << "  预估 token:    " << est << "\n"
              << "  压缩阈值:      "
              << (limit > 0 ? std::to_string(limit) : "禁用") << "\n"
              << "  已压缩次数:    " << context_.GetCompressCount() << "\n"
              << "  历史消息条数:  " << context_.GetHistorySize() << "\n"
              << "  附加文件数:    " << context_.GetFiles().size() << "\n";

    if (limit > 0) {
        int pct = (limit > 0)
                  ? static_cast<int>(est * 100 / static_cast<size_t>(limit))
                  : 0;
        pct = std::min(pct, 100);
        int bar_filled = pct * 30 / 100;
        std::string bar =
            std::string(pct < 80 ? utils::color::kGreen : utils::color::kYellow) +
            utils::Repeat("█", bar_filled) +
            utils::color::kDim +
            utils::Repeat("░", 30 - bar_filled) +
            utils::color::kReset;
        std::cout << "  用量:          [" << bar << "]  " << pct << "%\n";
    }
    std::cout << "\n";
}

void Repl::HandleSkills(const std::string& args) {
    auto& sm = SkillManager::GetInstance();

    // /skills reload  — 重新扫描目录
    if (args == "reload") {
        InitSkills();
        utils::PrintSuccess("Skills 已重新加载，发现 " +
                            std::to_string(sm.GetSkills().size()) + " 个");
        return;
    }

    // /skills load <name>  — 手动加载一个 skill 到上下文
    if (args.substr(0, 5) == "load ") {
        std::string name = utils::Trim(args.substr(5));
        auto body = sm.GetSkillBody(name);
        if (!body) {
            utils::PrintError("未找到 Skill: " + name);
            return;
        }
        sm.MarkLoaded(name);
        // 注入到对话上下文
        context_.AddUserMessage("[Skill 已手动加载: " + name + "]\n\n" + *body);
        context_.AddAssistantMessage("好的，我已加载 Skill「" + name + "」，将按其指导执行任务。");
        utils::PrintSuccess("已加载 Skill: " + name);
        return;
    }

    // /skills  — 列出所有 Skills
    const auto& skills = sm.GetSkills();
    if (skills.empty()) {
        std::cout << utils::color::kYellow
                  << "没有发现可用的 Skills。\n"
                  << utils::color::kReset
                  << "  放置位置（任一）：\n"
                  << "    ~/.config/termind/skills/<skill-name>/SKILL.md\n"
                  << "    <工作目录>/.termind/skills/<skill-name>/SKILL.md\n"
                  << "  或在 config.json 中配置 skills_dirs\n\n";
        return;
    }

    std::cout << "\n" << utils::color::kBold << utils::color::kCyan
              << "可用 Skills (" << skills.size() << " 个)"
              << utils::color::kReset << "\n\n";

    for (const auto& s : skills) {
        bool loaded = sm.IsLoaded(s.name);
        std::cout << "  " << (loaded
                              ? std::string(utils::color::kGreen) + "✅"
                              : std::string(utils::color::kDim)   + "⬜")
                  << utils::color::kReset << "  "
                  << utils::color::kBold << s.name << utils::color::kReset << "\n"
                  << "       " << utils::color::kDim << s.description
                  << utils::color::kReset << "\n"
                  << "       " << utils::color::kDim << s.dir.string()
                  << utils::color::kReset << "\n\n";
    }

    std::cout << utils::color::kDim
              << "  /skills load <name>  — 手动加载 Skill 到上下文\n"
              << "  /skills reload       — 重新扫描目录\n"
              << utils::color::kReset << "\n";
}

// ── 项目记忆命令 ──────────────────────────────────────────────────────────

void Repl::HandleMemory(const std::string& args) {
    // /memory reload  — 重新读取 TERMIND.md
    if (args == "reload") {
        InitProjectMemory();
        if (project_memory_path_.empty()) {
            utils::PrintWarning("未找到 TERMIND.md（已搜索工作目录及上级直到 Git 根）");
        } else {
            utils::PrintSuccess("已重新加载: " + project_memory_path_.string() +
                                "（" + std::to_string(project_memory_content_.size()) +
                                " 字符）");
        }
        return;
    }

    // /memory edit  — 用 $EDITOR 打开
    if (args == "edit") {
        fs::path target = project_memory_path_.empty()
                              ? context_.GetWorkingDir() / "TERMIND.md"
                              : project_memory_path_;

        // 若文件不存在则写入模板
        if (!fs::exists(target)) {
            if (!utils::WriteFile(target, kMemoryTemplate)) {
                utils::PrintError("创建文件失败: " + target.string());
                return;
            }
            utils::PrintInfo("已创建模板: " + target.string());
        }

        std::string editor = utils::GetEnv("VISUAL");
        if (editor.empty()) editor = utils::GetEnv("EDITOR");
        if (editor.empty()) editor = "vi";

        std::string cmd = editor + " " + utils::EscapeShellArg(target.string());
        int ret = std::system(cmd.c_str());  // NOLINT(cert-env33-c)
        (void)ret;

        // 编辑完毕后自动 reload
        InitProjectMemory();
        utils::PrintSuccess("已重新加载: " + target.string());
        return;
    }

    // /memory init  — 在当前工作目录创建模板（不打开编辑器）
    if (args == "init") {
        fs::path target = context_.GetWorkingDir() / "TERMIND.md";
        if (fs::exists(target)) {
            utils::PrintWarning("文件已存在: " + target.string() +
                                "，使用 /memory edit 编辑");
            return;
        }
        if (!utils::WriteFile(target, kMemoryTemplate)) {
            utils::PrintError("创建失败: " + target.string());
            return;
        }
        InitProjectMemory();
        utils::PrintSuccess("已创建: " + target.string());
        return;
    }

    // /memory  — 显示当前状态
    if (project_memory_path_.empty()) {
        using namespace ftxui;
        auto doc = vbox({
            hbox({text("◆ 项目记忆") | bold | color(Color::CyanLight),
                  text("  未找到 TERMIND.md") | dim}),
            separator(),
            text("从工作目录向上搜索（到 Git 根）均未找到记忆文件。") | dim,
            text("创建一个来给 AI 提供持久化的项目背景：") | dim,
            text(""),
            hbox({text("/memory init") | bold, text("  在当前目录创建模板") | dim}),
            hbox({text("/memory edit") | bold, text("  创建并用 $EDITOR 打开") | dim}),
        }) | border;
        std::cout << "\n";
        PrintFTXUI(doc, 2);
        std::cout << "\n";
        return;
    }

    // 显示已加载的内容
    using namespace ftxui;
    std::string rel  = utils::GetRelativePath(project_memory_path_, context_.GetWorkingDir());
    std::string size = utils::FormatFileSize(project_memory_content_.size());

    auto lines = utils::Split(project_memory_content_, '\n');
    // 去掉尾部空行
    while (!lines.empty() && utils::Trim(lines.back()).empty()) lines.pop_back();
    int show = std::min(static_cast<int>(lines.size()), 40);

    std::vector<Element> rows;
    rows.push_back(hbox({
        text("◆ 项目记忆") | bold | color(Color::CyanLight),
        text("  " + rel + "  (" + size + ")") | dim,
    }));
    rows.push_back(separator());
    for (int i = 0; i < show; ++i)
        rows.push_back(text(lines[static_cast<size_t>(i)]) | dim);
    if (static_cast<int>(lines.size()) > show) {
        rows.push_back(
            text("… 共 " + std::to_string(lines.size()) +
                 " 行，/memory edit 查看全部") | dim
        );
    }
    rows.push_back(separator());
    rows.push_back(
        hbox({text("/memory edit") | bold, text("  编辑"), text("   "),
              text("/memory reload") | bold, text("  重新加载")}) | dim
    );

    auto doc = vbox(rows) | border;
    std::cout << "\n";
    PrintFTXUI(doc, 2);
    std::cout << "\n";
}

// ── 从规划文本中提取编号步骤 ─────────────────────────────────────────────

// ── /plan 命令：先规划，再执行（步骤与进度交由 AI 自主规划）────────────────

void Repl::HandlePlan(const std::string& args) {
    std::string task = utils::Trim(args);
    if (task.empty()) {
        utils::PrintWarning("用法: /plan <任务描述>\n"
                            "  示例: /plan 给 User 类添加邮箱验证功能");
        return;
    }

    utils::PrintHorizontalRule("规划模式");
    std::cout << utils::color::kDim
              << "  Termind 将先分析任务，输出执行计划，再询问是否开始。\n"
              << utils::color::kReset << "\n";

    // 构造规划专用的系统消息（步骤与进度全部交由 AI 自主规划与输出）
    auto messages = context_.GetMessages();
    if (!messages.empty() && messages[0].role == MessageRole::kSystem) {
        messages[0].content +=
            "\n\n---\n## 当前模式：规划（Planning）\n\n"
            "用户要求你先输出执行计划，**不要调用任何工具，不要修改任何代码**。\n"
            "请自由组织计划结构（如：任务理解、需了解的信息、执行步骤、潜在风险、验证方案等），\n"
            "步骤的拆分、编号和进度展示由你自行决定。输出计划后停止，不要执行任何操作。";
    }
    messages.push_back(Message::User("请为以下任务制定执行计划：\n\n" + task));

    // ── 流式输出规划，同时累积完整文本以便加入上下文 ──────────────────────
    std::cout << utils::color::kBrightGreen << "Termind"
              << utils::color::kReset << " "
              << utils::color::kDim << "❯ " << utils::color::kReset;

    std::string plan_text;
    bool first_chunk = true;
    tui::ThinkingPane pane;
    pane.Start(std::string(tui::color::kDim) + "规划中…" + tui::color::kReset);

    ai_client_->ChatStream(
        messages, {},
        [&](const std::string& chunk) {
            if (first_chunk) { pane.Stop(); first_chunk = false; }
            std::cout << chunk;
            std::cout.flush();
            plan_text += chunk;
        });

    pane.Stop();
    std::cout << "\n\n";
    utils::PrintHorizontalRule();

    bool proceed = utils::AskYesNo("按照此计划开始执行？", true);
    std::cout << "\n";

    if (!proceed) {
        utils::PrintInfo("已取消。可以直接输入任务描述重新开始。");
        return;
    }

    // 将规划轮次加入上下文，让 AI 执行时能看到自己的计划
    context_.AddUserMessage("请为以下任务制定执行计划：\n\n" + task);
    context_.AddAssistantMessage(plan_text);
    context_.AddUserMessage(
        "很好，请按照上述计划开始执行。\n\n"
        "**重要**：每一轮回复开头必须先输出一行进度摘要（在调用任何工具之前），"
        "格式如：📋 进度：✅ 步骤1  ⏳ 步骤2（进行中）  ⬜ 步骤3\n"
        "不要把进度放进 <think> 标签。");

    utils::PrintHorizontalRule("开始执行");
    RunAgentLoop("", true);
}

// ── AI 代理循环 ───────────────────────────────────────────────────────────

void Repl::RunAgentLoop(const std::string& user_message,
                         bool skip_add_user) {
    if (!skip_add_user) {
        context_.AddUserMessage(user_message);
    }

    auto& registry = ToolRegistry::GetInstance();
    const auto& cfg = ConfigManager::GetInstance().config();
    auto tools = registry.GetToolDefinitionsJson();

    // ── Read Group：延迟打印方式，避免光标定位出错 ──────────────────────────
    // append_rg 只收集条目，flush_rg 在合适时机统一打印一张卡片。
    // flush_rg 调用时机：① 遇到非读取工具  ② AI 输出文字  ③ 每轮工具循环结束
    struct ReadGroup {
        std::vector<std::pair<std::string,std::string>> entries;
    } rg;

    auto flush_rg = [&]() {
        if (rg.entries.empty()) return;
        using namespace ftxui;
        std::vector<Element> rows;
        rows.push_back(hbox({
            text("Read") | bold | color(Color::CyanLight),
            text("  files") | dim,
        }));
        rows.push_back(separator());
        for (const auto& [l, d] : rg.entries) {
            rows.push_back(hbox({
                text(l + "  ") | color(Color::CyanLight),
                text(d) | dim,
            }));
        }
        PrintFTXUI(vbox(rows) | border, 2);
        rg.entries.clear();
    };

    auto append_rg = [&](const ToolCallRequest& tc2, const ToolResult& res) {
        std::string path = tc2.arguments.value("path", "");
        auto ls = utils::Split(res.output, '\n');
        while (!ls.empty() && ls.back().empty()) ls.pop_back();
        std::string lbl = (tc2.name == "get_file_outline") ? "Outline" : "Read   ";
        std::string desc = path + "  (" + std::to_string(ls.size()) + " 行)";
        rg.entries.emplace_back(lbl, desc);
    };

    int iterations = 0;

    while (iterations++ < cfg.max_tool_iterations) {
        // ── 上下文压缩（在发送前检查）────────────────────────────────────
        if (cfg.max_context_tokens > 0) {
            int dropped = context_.TrimToFit(
                static_cast<size_t>(cfg.max_context_tokens));
            if (dropped > 0) {
                utils::PrintWarning(
                    "上下文已压缩：丢弃 " + std::to_string(dropped) +
                    " 条旧消息（当前 ~" +
                    std::to_string(context_.EstimateTokens()) + " tokens）");
            }
        }

        auto messages = context_.GetMessages();

        // ── 每轮注入进度展示要求 ────────────────────────────────────────────
        if (!messages.empty() && messages[0].role == MessageRole::kSystem) {
            messages[0].content +=
                "\n\n---\n## 任务规划与进度（强制要求）\n"
                "接到多步骤任务时，**第一轮**必须先在正文（不在 <think> 里）输出任务拆解，"
                "然后立即开始执行，不需要等用户确认：\n\n"
                "  📋 任务拆解：\n"
                "  ⬜ 读取 src/server.cpp，找到 handle_request() 入口\n"
                "  ⬜ 在超时分支末尾追加 LOG_WARN(\"timeout\") 并返回 408\n"
                "  ⬜ 运行 cmake --build build 编译，确认零错误\n"
                "  ⬜ curl -v http://localhost:8080/slow 验证返回 408\n\n"
                "**后续每轮**有文字输出时，在开头更新进度（每步一行）：\n\n"
                "  ✅ 读取 src/server.cpp，找到 handle_request() 入口\n"
                "  ⏳ 在超时分支末尾追加 LOG_WARN(\"timeout\") 并返回 408（进行中）\n"
                "  ⬜ 运行 cmake --build build 编译，确认零错误\n"
                "  ⬜ curl -v http://localhost:8080/slow 验证返回 408\n\n"
                "规则：\n"
                "- 每步单独一行，以 ✅/⏳/⬜ 开头\n"
                "- 描述具体：写出文件名、函数名、具体操作，禁止写「理解需求」「实现修改」等泛化词\n"
                "- **绝对不能放进 <think> 标签**，必须在正文输出\n"
                "- 单步骤、问答类任务不需要显示进度";
        }

        // ── 启动思考预览面板 ──────────────────────────────────────────────
        tui::ThinkingPane pane;
        pane.Start(std::string(tui::color::kDim) + "思考中…" + tui::color::kReset);

        ChatResponse response;

        if (cfg.stream) {
            // 状态追踪
            bool   first_chunk    = true;
            bool   text_shown     = false;
            size_t tool_arg_chars = 0;
            std::string current_tool;

            // 流式渲染器（tui::StreamRenderer）：
            //   - <think>...</think> 内容路由到 ThinkingPane，正文完全不输出
            //   - [[DONE:N]] 静默剥除
            std::string  md_line_buf;    // 逐行缓冲，遇 '\n' 后统一剥除 markdown 再打印
            bool         md_in_code = false; // 代码围栏状态（``` 切换）

            tui::StreamRenderer renderer(
                // ── 正文回调 ──────────────────────────────────────────
                [&](const std::string& text) {
                    if (first_chunk) {
                        pane.Stop();
                        flush_rg();  // 文字输出前先刷出上一批读取卡片
                        first_chunk = false;
                    }
                    // 按行缓冲：遇换行才剥除 markdown 并打印，保证整行上下文
                    for (char c : text) {
                        if (c == '\n') {
                            std::cout << utils::StripMarkdownLine(md_line_buf, md_in_code)
                                      << '\n';
                            std::cout.flush();
                            md_line_buf.clear();
                        } else {
                            md_line_buf += c;
                        }
                    }
                    text_shown = true;
                },
                // ── think 内容回调：喂给 ThinkingPane，不输出到正文 ──
                [&](const std::string& chunk) {
                    // 如果面板已被停止（先出现了正文），重启面板
                    if (!pane.IsRunning()) {
                        if (text_shown) { std::cout << "\n"; text_shown = false; }
                        pane.Start(std::string(tui::color::kDim) + "思考中…"
                                   + tui::color::kReset);
                    }
                    pane.FeedRaw(chunk);
                });

            response = ai_client_->ChatStream(
                messages, tools,
                // ── 文字流回调 ────────────────────────────────────────
                [&](const std::string& chunk) {
                    renderer.Feed(chunk);
                },
                // ── 工具参数流回调 ─────────────────────────────────────
                // 情况 A：纯工具调用（无文字），面板一直运行，直接更新标题
                // 情况 B：文字之后才来工具参数，面板已停 —— 换行后重启
                [&](const std::string& tool_name,
                    const std::string& arg_chunk) {
                    if (!pane.IsRunning()) {
                        // 情况 B：文字刚输出完，面板已被停止
                        if (text_shown) {
                            std::cout << "\n";  // 结束文字行
                            text_shown = false;
                        }
                        current_tool   = tool_name;
                        tool_arg_chars = 0;
                        pane.Start(std::string(tui::color::kBrightCyan) +
                                   "● " + tui::color::kReset +
                                   tui::color::kBold + tool_name +
                                   tui::color::kReset + " …");
                    } else if (tool_name != current_tool) {
                        current_tool   = tool_name;
                        tool_arg_chars = 0;
                    }
                    tool_arg_chars += arg_chunk.size();
                    pane.SetHeading(
                        std::string(tui::color::kBrightCyan) + "● " +
                        tui::color::kReset +
                        tui::color::kBold + tool_name + tui::color::kReset +
                        tui::color::kDim + "  " +
                        std::to_string(tool_arg_chars) + " chars" +
                        tui::color::kReset);
                    pane.Feed(arg_chunk);
                });

            // 流结束：冲刷渲染器（关闭未闭合样式）
            renderer.Flush();
            // 刷出末尾未换行的行缓冲（AI 结束时通常无尾换行）
            if (!md_line_buf.empty()) {
                std::cout << utils::StripMarkdownLine(md_line_buf, md_in_code);
                std::cout.flush();
                md_line_buf.clear();
            }
            pane.Stop();
        } else {
            response = ai_client_->Chat(messages, tools);
            // 非流式：同样过滤 <think> 标签和步骤标记
            if (!response.content.empty()) {
                std::string filtered;
                tui::StreamRenderer nr([&](const std::string& t){ filtered += t; });
                nr.Feed(response.content);
                nr.Flush();
                response.content = filtered;
            }
            pane.Stop();
        }

        // ── 请求失败 ──────────────────────────────────────────────────────
        if (!response.success) {
            utils::PrintError("AI 请求失败: " + response.error_message);
            break;
        }

        // ── 最终文字回答（无工具调用）────────────────────────────────────
        if (!response.HasToolCalls()) {
            if (!cfg.stream && !response.content.empty()) {
                // 非流式：逐行剥离 markdown 后打印
                bool in_code = false;
                for (auto& ln : utils::Split(response.content, '\n')) {
                    std::cout << utils::StripMarkdownLine(ln, in_code) << '\n';
                }
            }
            std::cout << "\n";
            context_.AddAssistantMessage(response.content);
            break;
        }

        // ── 处理工具调用 ──────────────────────────────────────────────────
        context_.AddAssistantToolCalls(response.tool_calls, response.content);

        for (const auto& tc : response.tool_calls) {
            ToolResult result = ExecuteToolWithConfirmation(tc);
            context_.AddToolResult(tc.id, result.output);

            bool is_read = (tc.name == "read_file" || tc.name == "get_file_outline");

            if (!is_read) flush_rg();   // 非读取工具 → 先把积累的读取打印出来

            if (is_read) {
                append_rg(tc, result);
            } else if (tc.name != "write_file" && tc.name != "edit_file") {
                RenderToolCard(tc, result);
            }

            if (interrupt_loop_) break;
        }
        flush_rg();   // 工具循环结束，打印本轮剩余的读取条目

        if (interrupt_loop_) {
            interrupt_loop_ = false;
            break;
        }
    }

    if (iterations > cfg.max_tool_iterations) {
        utils::PrintWarning("已达到最大工具调用次数（" +
                             std::to_string(cfg.max_tool_iterations) + "）");
    }
}

// ── 工具调用确认 ──────────────────────────────────────────────────────────

ToolResult Repl::ExecuteToolWithConfirmation(const ToolCallRequest& tc) {
    auto& registry = ToolRegistry::GetInstance();

    // ── 文件写入：展示 diff，自动执行，无需确认 ─────────────────────────
    if (tc.name == "write_file") {
        std::string path_str = tc.arguments.value("path", "");
        std::string content  = tc.arguments.value("content", "");
        PrintBoxTop("Write", path_str);
        ShowWriteFilePreview(path_str, content);
        auto result = registry.Execute(tc.name, tc.arguments);
        if (result.success)
            std::cout << utils::color::kGreen << "  ✓ 已写入" << utils::color::kReset << "\n";
        else
            std::cout << utils::color::kRed << "  ✗ 写入失败: " << result.output.substr(0, 80)
                      << utils::color::kReset << "\n";
        PrintBoxBottom();
        return result;
    }

    // ── 精准编辑：展示 diff，自动执行，无需确认 ─────────────────────────
    if (tc.name == "edit_file") {
        std::string path_str    = tc.arguments.value("path", "");
        std::string old_content = tc.arguments.value("old_content", "");
        std::string new_content = tc.arguments.value("new_content", "");
        PrintBoxTop("Edit", path_str);
        ShowEditFilePreview(path_str, old_content, new_content);
        auto result = registry.Execute(tc.name, tc.arguments);
        if (result.success)
            std::cout << utils::color::kGreen << "  ✓ 已编辑" << utils::color::kReset << "\n";
        else
            std::cout << utils::color::kRed << "  ✗ 编辑失败: " << result.output.substr(0, 80)
                      << utils::color::kReset << "\n";
        PrintBoxBottom();
        return result;
    }

    // ── 非破坏性工具：直接执行 ──────────────────────────────────────────
    if (!registry.RequiresConfirmation(tc.name))
        return registry.Execute(tc.name, tc.arguments);

    // ── run_shell 及其他需要确认的工具：展示后询问 ──────────────────────
    if (tc.name == "run_shell") {
        std::string cmd = tc.arguments.value("command", "");
        std::cout << "\n" << utils::color::kYellow << "  $ "
                  << utils::color::kReset << cmd << "\n";
    } else {
        std::cout << "\n  " << tc.arguments.dump(2) << "\n";
    }

    bool ok = utils::AskYesNo("执行此命令?", true);
    if (!ok) {
        // 询问反馈：空回车=跳过，文字=给 AI 提示，q=中断整个任务
        std::cout << utils::color::kDim
                  << "  ↳ 反馈（直接回车跳过，输入内容重定向 AI，q 中断任务）: "
                  << utils::color::kReset;
        std::cout.flush();
        std::string feedback;
        std::getline(std::cin, feedback);
        // 去除首尾空白
        size_t s = feedback.find_first_not_of(" \t");
        size_t e = feedback.find_last_not_of(" \t");
        feedback = (s == std::string::npos) ? "" : feedback.substr(s, e - s + 1);

        std::cout << "\n";
        if (feedback == "q" || feedback == "Q") {
            interrupt_loop_ = true;
            utils::PrintWarning("任务已中断");
            return {false, "用户已中断任务。"};
        }
        if (!feedback.empty()) {
            return {false,
                    "用户拒绝了此操作，并给出以下反馈，请据此重新规划，"
                    "不要再次尝试刚才的操作：\n" + feedback};
        }
        return {false, "用户已拒绝该操作。"};
    }

    return registry.Execute(tc.name, tc.arguments);
}

// ── 文件写入预览 ──────────────────────────────────────────────────────────

void Repl::ShowWriteFilePreview(const std::string& path_str,
                                  const std::string& new_content) {
    fs::path path = path_str[0] == '/'
                        ? fs::path(path_str)
                        : context_.GetWorkingDir() / path_str;

    if (utils::FileExists(path)) {
        auto old_content = utils::ReadFile(path);
        if (old_content) {
            std::string diff = utils::ComputeDiff(*old_content, new_content, path_str);
            utils::PrintColoredDiff(diff);
        }
    } else {
        // 新文件 → 绿色 + 号预览
        auto lines = utils::Split(new_content, '\n');
        int show   = std::min(30, static_cast<int>(lines.size()));
        for (int i = 0; i < show; ++i) {
            std::cout << utils::color::kGreen << "  + " << lines[i]
                      << utils::color::kReset << "\n";
        }
        if (static_cast<int>(lines.size()) > show) {
            std::cout << utils::color::kDim << "  + … 还有 "
                      << lines.size() - show << " 行"
                      << utils::color::kReset << "\n";
        }
    }
}

// ── edit_file 预览 ────────────────────────────────────────────────────────

void Repl::ShowEditFilePreview(const std::string& path_str,
                                 const std::string& old_content,
                                 const std::string& new_content) {
    std::string diff = utils::ComputeDiff(old_content, new_content, path_str);
    utils::PrintColoredDiff(diff);
}

// ── 工具调用头部显示 ──────────────────────────────────────────────────────

void Repl::PrintToolCallHeader(const ToolCallRequest& tc) const {
    // 每个工具对应的颜色和单字母动作标识
    struct Info { const char* color; const char* label; };
    Info info;
    if      (tc.name == "read_file"        ) info = {utils::color::kBrightCyan,   "Read"  };
    else if (tc.name == "write_file"       ) info = {utils::color::kBrightGreen,  "Write" };
    else if (tc.name == "edit_file"        ) info = {utils::color::kBrightGreen,  "Edit"  };
    else if (tc.name == "list_directory"   ) info = {utils::color::kBrightCyan,   "List"  };
    else if (tc.name == "search_files"     ) info = {utils::color::kBrightYellow, "Search"};
    else if (tc.name == "grep_code"        ) info = {utils::color::kBrightYellow, "Grep"  };
    else if (tc.name == "run_shell"        ) info = {utils::color::kYellow,       "Bash"  };
    else if (tc.name == "get_file_info"    ) info = {utils::color::kBrightCyan,   "Stat"  };
    else if (tc.name == "search_symbol"    ) info = {utils::color::kBrightYellow, "Symbol"};
    else if (tc.name == "get_file_outline" ) info = {utils::color::kBrightCyan,   "Outline"};
    else                                     info = {utils::color::kDim,           "Tool"  };

    // 摘要参数
    std::string brief;
    if      (tc.arguments.contains("command")) brief = tc.arguments["command"].get<std::string>();
    else if (tc.arguments.contains("path")   ) brief = tc.arguments["path"].get<std::string>();
    else if (tc.arguments.contains("pattern")) brief = tc.arguments["pattern"].get<std::string>();
    else if (tc.arguments.contains("query")  ) brief = tc.arguments["query"].get<std::string>();

    // 截断过长的 brief（避免多行）
    int tw = utils::GetTerminalWidth();
    int max_brief = tw - static_cast<int>(tc.name.size()) - 14;
    if (max_brief < 10) max_brief = 10;
    if (static_cast<int>(brief.size()) > max_brief)
        brief = brief.substr(0, static_cast<size_t>(max_brief)) + "…";

    // 格式：  ● Label(brief)
    //    或：  ● Label
    std::cout << "\n"
              << info.color   << "  ● " << utils::color::kReset
              << utils::color::kBold << info.label << utils::color::kReset
              << utils::color::kDim;
    if (!brief.empty())
        std::cout << "(" << brief << ")";
    std::cout << utils::color::kReset << "\n";
}

// ── 欢迎界面 ──────────────────────────────────────────────────────────────

void Repl::PrintWelcome() const {
    using namespace ftxui;
    const auto& cfg = ConfigManager::GetInstance().config();

    // ── 路径处理 ────────────────────────────────────────────────────────
    std::string dir_str = fs::current_path().string();
    if (static_cast<int>(dir_str.size()) > 46) {
        fs::path p = fs::current_path();
        dir_str = "…/" + p.parent_path().filename().string()
                  + "/" + p.filename().string();
    }

    std::string mem_str;
    bool has_memory = !project_memory_path_.empty();
    if (has_memory) {
        mem_str = utils::GetRelativePath(project_memory_path_, context_.GetWorkingDir())
                  + "  (" + utils::FormatFileSize(project_memory_content_.size()) + ")";
    } else {
        mem_str = "未找到 TERMIND.md  (/memory init 创建)";
    }

    // ── 构建 FTXUI 文档 ─────────────────────────────────────────────────
    // 标题区
    auto title_line = hbox({
        text("◆ Termind") | bold | color(Color::CyanLight),
        text("  v0.1") | dim,
    });
    auto sub_line = text("端脑 · 终端代码助手") | dim;

    // 信息行生成器
    auto info = [](const std::string& lbl, const Element& val) {
        return hbox({
            text(lbl) | dim,
            val,
        });
    };

    auto mem_val = has_memory
        ? text(mem_str) | color(Color::Green)
        : text(mem_str) | dim;

    auto doc = vbox({
        title_line,
        sub_line,
        separator(),
        info("模型   ", text(cfg.model) | color(Color::CyanLight)),
        info("目录   ", text(dir_str)   | dim),
        info("记忆   ", mem_val),
    }) | border;

    std::cout << "\n";
    PrintFTXUI(doc, 2);
    std::cout << "\n";

    if (cfg.api_key.empty()) {
        std::cout << utils::color::kYellow << "  ⚠  " << utils::color::kReset
                  << "未检测到 API Key，请设置 TERMIND_API_KEY\n\n";
    }

    std::cout << utils::color::kDim
              << "  /help 查看命令  ·  /quit 退出\n"
              << utils::color::kReset << "\n";
}

// ── 上下文 token 徽章（提示符和 AI 回答头部共用）─────────────────────────
// 格式："42% 34k/80k "（启用压缩）或 "34k "（禁用），颜色随用量变化

std::string Repl::BuildContextBadge() const {
    const auto& cfg = ConfigManager::GetInstance().config();
    size_t est = context_.EstimateTokens();

    // 紧凑 token 格式：>= 1000 用 "XXk"，否则直接显示数字
    auto fmt_tok = [](size_t n) -> std::string {
        if (n >= 1000) {
            size_t k = n / 1000;
            size_t r = (n % 1000) / 100;
            if (r == 0) return std::to_string(k) + "k";
            return std::to_string(k) + "." + std::to_string(r) + "k";
        }
        return std::to_string(n);
    };

    if (cfg.max_context_tokens > 0) {
        int limit = cfg.max_context_tokens;
        int pct   = static_cast<int>(est * 100 / static_cast<size_t>(limit));
        pct = std::min(pct, 100);

        const char* clr;
        if      (pct < 60) clr = utils::color::kBrightGreen;
        else if (pct < 80) clr = utils::color::kYellow;
        else               clr = utils::color::kRed;

        return std::string(clr) +
               std::to_string(pct) + "% " +
               fmt_tok(est) + "/" + fmt_tok(static_cast<size_t>(limit)) +
               utils::color::kReset + " ";
    }

    if (est > 0) {
        return std::string(utils::color::kDim) +
               fmt_tok(est) +
               utils::color::kReset + " ";
    }

    return {};
}

// ── 构建提示符 ────────────────────────────────────────────────────────────
// readline 需要知道提示符的可见宽度以正确换行。
// 所有不可见的 ANSI 转义序列必须用 \001 (RL_PROMPT_START_IGNORE) 和
}  // namespace termind
