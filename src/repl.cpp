#include "termind/repl.h"
#include "termind/config.h"
#include "termind/skill_manager.h"
#include "termind/utils.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <set>
#include <sstream>

#include <readline/history.h>
#include <readline/readline.h>
#include <unistd.h>

namespace termind {

namespace fs = std::filesystem;

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
    if (cmd == "skills"  || cmd == "skill")         { HandleSkills(args);    return true; }
    if (cmd == "memory"  || cmd == "mem")           { HandleMemory(args);    return true; }
    if (cmd == "plan"    || cmd == "p")             { HandlePlan(args);      return true; }

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
              << utils::color::kYellow << "项目记忆\n" << utils::color::kReset
              << "  /memory              显示当前 TERMIND.md 内容\n"
              << "  /memory init         在当前目录创建 TERMIND.md 模板\n"
              << "  /memory edit         用 $EDITOR 打开 TERMIND.md\n"
              << "  /memory reload       重新加载 TERMIND.md\n\n"
              << utils::color::kYellow << "Skills\n" << utils::color::kReset
              << "  /skills              列出所有可用 Skills\n"
              << "  /skills load <name>  手动加载 Skill 到上下文\n"
              << "  /skills reload       重新扫描 Skills 目录\n\n"
              << utils::color::kYellow << "规划\n" << utils::color::kReset
              << "  /plan <任务>     先输出执行计划，确认后再执行（别名: /p）\n\n"
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
        std::cout << "\n" << utils::color::kYellow
                  << "📋 未找到项目记忆（TERMIND.md）" << utils::color::kReset << "\n\n"
                  << "  Termind 会从工作目录向上搜索 TERMIND.md，直到 Git 根目录。\n"
                  << "  创建一个来为 AI 提供持久化的项目背景：\n\n"
                  << utils::color::kDim
                  << "    /memory init          在当前目录创建模板\n"
                  << "    /memory edit          创建并用 $EDITOR 打开\n"
                  << utils::color::kReset << "\n";
        return;
    }

    // 显示已加载的内容
    std::cout << "\n" << utils::color::kBold << utils::color::kGreen
              << "📋 项目记忆" << utils::color::kReset << "\n"
              << "  路径: " << utils::color::kDim
              << project_memory_path_.string() << utils::color::kReset << "\n"
              << "  大小: " << utils::color::kDim
              << utils::FormatFileSize(project_memory_content_.size())
              << utils::color::kReset << "\n\n";

    // 显示前 40 行内容预览
    auto lines = utils::Split(project_memory_content_, '\n');
    int preview_lines = std::min(static_cast<int>(lines.size()), 40);
    for (int i = 0; i < preview_lines; ++i) {
        std::cout << utils::color::kDim << "  " << utils::color::kReset
                  << lines[i] << "\n";
    }
    if (static_cast<int>(lines.size()) > preview_lines) {
        std::cout << utils::color::kDim << "  … 共 " << lines.size()
                  << " 行，使用 /memory edit 查看全部" << utils::color::kReset << "\n";
    }

    std::cout << "\n" << utils::color::kDim
              << "  /memory edit    编辑\n"
              << "  /memory reload  重新加载\n"
              << utils::color::kReset << "\n";
}

// ── 从规划文本中提取编号步骤 ─────────────────────────────────────────────

static std::vector<std::string> ParseTasksFromPlanText(const std::string& text) {
    std::vector<std::string> tasks;
    for (const auto& line : utils::Split(text, '\n')) {
        std::string s = utils::Trim(line);
        if (s.empty()) continue;

        // 匹配：数字 + '.' 或 ')' + 内容
        // 如："1. 读取文件"、"2) **分析结构**：..."、"  3. 修改代码"
        size_t i = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
        if (i == 0 || i >= s.size()) continue;
        if (s[i] != '.' && s[i] != ')') continue;
        ++i;
        // 跳过空格和 markdown 粗体标记
        while (i < s.size() && (s[i] == ' ' || s[i] == '*')) ++i;

        std::string desc = s.substr(i);
        // 去除末尾的 ** 和多余空格
        while (!desc.empty() &&
               (desc.back() == '*' || desc.back() == ' ' || desc.back() == ':'))
            desc.pop_back();

        if (desc.size() > 3)  // 过滤太短的（如纯标点）
            tasks.push_back(desc);
    }
    return tasks;
}

// ── /plan 命令：先规划，再执行 ───────────────────────────────────────────

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

    // 构造规划专用的系统消息
    auto messages = context_.GetMessages();
    if (!messages.empty() && messages[0].role == MessageRole::kSystem) {
        messages[0].content +=
            "\n\n---\n## 当前模式：规划（Planning）\n\n"
            "用户要求你先输出执行计划，**不要调用任何工具，不要修改任何代码**。\n"
            "请在\"执行步骤\"部分使用严格的编号列表（1. 2. 3. ...），每步一行。\n"
            "请按以下结构输出计划：\n"
            "1. **任务理解**：用一句话概括要做什么\n"
            "2. **需要了解的信息**：列出需要先阅读哪些文件或搜索哪些符号\n"
            "3. **执行步骤**（编号列表，每步一行）：具体操作，注明使用什么工具\n"
            "4. **潜在风险**：可能遇到的问题\n"
            "5. **验证方案**：如何验证修改正确\n\n"
            "输出计划后停止，不要执行任何操作。";
    }
    messages.push_back(Message::User("请为以下任务制定执行计划：\n\n" + task));

    // ── 流式输出规划，同时累积完整文本 ──────────────────────────────────
    std::cout << utils::color::kBrightGreen << "Termind"
              << utils::color::kReset << " "
              << utils::color::kDim << "❯ " << utils::color::kReset;

    std::string plan_text;
    bool first_chunk = true;
    utils::ThinkingPane pane;
    pane.Start("规划中…");

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

    // ── 解析步骤，构建 TaskPanel ──────────────────────────────────────────
    auto steps = ParseTasksFromPlanText(plan_text);

    utils::TaskPanel panel;
    if (!steps.empty()) {
        panel.SetTasks(steps);
        panel.ActivateFirst();
        std::cout << "\n";
        panel.Render();
        std::cout << "\n";
    }

    // ── 询问是否执行 ──────────────────────────────────────────────────────
    bool proceed = utils::AskYesNo("按照此计划开始执行？", true);
    std::cout << "\n";

    if (!proceed) {
        panel.Clear();
        utils::PrintInfo("已取消。可以直接输入任务描述重新开始。");
        return;
    }

    utils::PrintHorizontalRule("开始执行");
    RunAgentLoop(task, steps.empty() ? nullptr : &panel);

    // ── 执行结束，显示最终面板状态 ────────────────────────────────────────
    if (!steps.empty()) {
        std::cout << "\n";
        panel.Render();
    }
}

// ── AI 代理循环 ───────────────────────────────────────────────────────────

// 流式渲染器：逐字符处理 AI 输出流，完成两件事：
//   1. 识别 <think>...</think> 块，用滚动面板（ThinkingPane）显示，标签本身不输出
//   2. 识别并剥除 [[DONE:N]] 步骤标记，同时更新 TaskPanel
struct StreamRenderer {
    enum class ThinkState { kNormal, kInThink };

    ThinkState           think_state_ = ThinkState::kNormal;
    std::string          tag_buf_;   // 正在识别的 <think> 或 </think>
    std::string          step_buf_;  // 正在识别的 [[DONE:N]]
    utils::TaskPanel*    panel_;
    utils::ThinkingPane* outer_pane_;  // RunAgentLoop 的外层请求进度面板
    utils::ThinkingPane  think_pane_;  // 思考内容滚动面板

    explicit StreamRenderer(utils::TaskPanel* p, utils::ThinkingPane* op)
        : panel_(p), outer_pane_(op) {}

    // 禁止拷贝/移动（ThinkingPane 内含 mutex/thread）
    StreamRenderer(const StreamRenderer&)            = delete;
    StreamRenderer& operator=(const StreamRenderer&) = delete;

    // 喂入一个 chunk，返回可安全打印的正文文本（实时，允许返回空串）
    std::string process(const std::string& chunk) {
        std::string out;
        for (char c : chunk)
            out += process_char(c);
        return out;
    }

    // 流结束时冲刷缓冲，确保面板关闭
    std::string flush() {
        std::string out;
        if (!tag_buf_.empty()) {
            if (think_state_ == ThinkState::kInThink)
                think_pane_.FeedRaw(tag_buf_);
            else
                out += tag_buf_;
            tag_buf_.clear();
        }
        if (!step_buf_.empty()) { out += step_buf_; step_buf_.clear(); }
        if (think_state_ == ThinkState::kInThink) {
            think_pane_.Stop();
            think_state_ = ThinkState::kNormal;
        }
        return out;
    }

private:
    void handle_done(int n) {
        if (panel_ && n >= 1 && n <= static_cast<int>(panel_->Size()))
            panel_->MarkDone(static_cast<size_t>(n - 1));
    }

    std::string process_char(char c) {
        // ── [[DONE:N]] 识别（仅在正文模式下）───────────────────────────
        if (!step_buf_.empty()) {
            step_buf_ += c;
            static const std::string kPfx = "[[DONE:";
            size_t end = step_buf_.find("]]");
            if (end != std::string::npos) {
                std::string ns = step_buf_.substr(kPfx.size(), end - kPfx.size());
                try { handle_done(std::stoi(ns)); } catch (...) {}
                step_buf_.clear();
                return "";
            }
            if (step_buf_.size() > 20) {
                std::string s = step_buf_; step_buf_.clear(); return s;
            }
            if (step_buf_.size() <= kPfx.size() &&
                kPfx.substr(0, step_buf_.size()) != step_buf_) {
                std::string s = step_buf_; step_buf_.clear(); return s;
            }
            return "";
        }

        // ── 标签识别缓冲区非空：继续匹配 ───────────────────────────────
        if (!tag_buf_.empty()) {
            tag_buf_ += c;
            return try_match_tag();
        }

        // ── 思考模式：内容喂给滚动面板 ──────────────────────────────────
        if (think_state_ == ThinkState::kInThink) {
            if (c == '<') { tag_buf_ += c; return ""; }  // 可能是 </think>
            std::string s(1, c);
            think_pane_.FeedRaw(s);
            return "";
        }

        // ── 正文模式 ─────────────────────────────────────────────────────
        if (c == '<') { tag_buf_ += c; return ""; }
        if (c == '[' && panel_) { step_buf_ += c; return ""; }
        return std::string(1, c);
    }

    std::string try_match_tag() {
        static const std::string kOpen  = "<think>";
        static const std::string kClose = "</think>";

        if (tag_buf_ == kOpen) {
            tag_buf_.clear();
            think_state_ = ThinkState::kInThink;
            // AI 以 <think> 开头时外层 pane 尚未被 first_chunk 关闭，需先停掉
            if (outer_pane_ && outer_pane_->IsRunning())
                outer_pane_->Stop();
            think_pane_.Start(std::string(utils::color::kDim) +
                              "思考中…" + utils::color::kReset);
            return "";
        }
        if (tag_buf_ == kClose) {
            tag_buf_.clear();
            think_state_ = ThinkState::kNormal;
            think_pane_.Stop();   // 清除面板，光标归位
            return "";
        }

        if (kOpen.substr(0,  tag_buf_.size()) == tag_buf_) return "";
        if (kClose.substr(0, tag_buf_.size()) == tag_buf_) return "";

        // 非合法标签：按当前模式处理缓冲
        std::string s = tag_buf_; tag_buf_.clear();
        if (think_state_ == ThinkState::kInThink) {
            think_pane_.FeedRaw(s);
            return "";
        }
        return s;
    }
};

void Repl::RunAgentLoop(const std::string& user_message,
                         utils::TaskPanel* panel) {
    context_.AddUserMessage(user_message);

    auto& registry = ToolRegistry::GetInstance();
    const auto& cfg = ConfigManager::GetInstance().config();
    auto tools = registry.GetToolDefinitionsJson();

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

        // ── 记录本轮开始时已完成的任务数（用于检测 [[DONE:N]] 是否触发）
        int done_before_iter = panel ? panel->DoneCount() : 0;

        // （面板渲染移到本轮工具调用完成之后，此处不再重复渲染）

        auto messages = context_.GetMessages();

        // ── 若存在任务面板，注入步骤追踪指令 ─────────────────────────────
        if (panel && !panel->Empty() &&
            !messages.empty() && messages[0].role == MessageRole::kSystem) {
            messages[0].content +=
                "\n\n---\n## 任务追踪指令\n"
                "你正在执行一个预定计划。每完成一个步骤（完成该步骤的所有工具调用后），"
                "在**独立一行**输出：[[DONE:N]]（N 为步骤编号，从 1 开始）。\n"
                "此标记会被系统自动解析并更新进度显示，**不会**展示给用户。\n"
                "当前已完成步骤数：" +
                std::to_string(panel->DoneCount()) + "/" +
                std::to_string(panel->Size()) + "。";
        }

        std::cout << "\n";

        // ── 启动思考预览面板 ──────────────────────────────────────────────
        utils::ThinkingPane pane;
        pane.Start("思考中… (" + std::to_string(iterations) + "/" +
                   std::to_string(cfg.max_tool_iterations) + ")");

        ChatResponse response;

        if (cfg.stream) {
            // 状态追踪
            bool   first_chunk    = true;
            bool   text_shown     = false;
            size_t tool_arg_chars = 0;
            std::string current_tool;

            // 流式渲染器：处理 <think> 标签着色 + [[DONE:N]] 步骤标记剥除
            StreamRenderer renderer(panel, &pane);

            response = ai_client_->ChatStream(
                messages, tools,
                // ── 文字流回调 ────────────────────────────────────────
                [&](const std::string& chunk) {
                    std::string clean = renderer.process(chunk);
                    if (clean.empty()) return;

                    if (first_chunk) {
                        pane.Stop();
                        std::cout << utils::color::kBrightGreen
                                  << "Termind" << utils::color::kReset
                                  << " " << BuildContextBadge()
                                  << utils::color::kDim << "❯ "
                                  << utils::color::kReset;
                        first_chunk = false;
                    }
                    std::cout << clean;
                    std::cout.flush();
                    text_shown = true;
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
                        pane.Start(std::string(utils::color::kYellow) +
                                   "⚡ " + utils::color::kReset +
                                   utils::color::kDim + tool_name +
                                   "  " + utils::color::kReset + "0 字符…");
                    } else if (tool_name != current_tool) {
                        // 同一轮内切换到下一个工具调用
                        current_tool   = tool_name;
                        tool_arg_chars = 0;
                    }
                    tool_arg_chars += arg_chunk.size();
                    pane.SetHeading(
                        std::string(utils::color::kYellow) + "⚡ " +
                        utils::color::kReset +
                        utils::color::kDim + tool_name + "  " +
                        utils::color::kReset +
                        std::to_string(tool_arg_chars) + " 字符…");
                    pane.Feed(arg_chunk);
                });

            // 流结束：冲刷渲染器（关闭未闭合样式、输出残余缓冲）
            {
                std::string tail = renderer.flush();
                if (!tail.empty()) {
                    std::cout << tail;
                    std::cout.flush();
                }
            }
            pane.Stop();
        } else {
            response = ai_client_->Chat(messages, tools);
            // 非流式：同样过滤 <think> 标签和步骤标记
            if (!response.content.empty()) {
                StreamRenderer nr(panel, nullptr);
                response.content = nr.process(response.content) + nr.flush();
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
            if (!cfg.stream) {
                std::cout << utils::color::kBrightGreen
                          << "Termind" << utils::color::kReset
                          << " " << BuildContextBadge()
                          << utils::color::kDim << "❯ "
                          << utils::color::kReset
                          << response.content;
            }
            std::cout << "\n";
            context_.AddAssistantMessage(response.content);

            // 最终回答意味着所有工作完成：推进面板并输出整体状态
            if (panel && !panel->Empty() && !panel->AllDone()) {
                while (!panel->AllDone()) panel->AdvanceActive();
                std::cout << "\n"
                          << utils::color::kDim << "── 全部完成 ──"
                          << utils::color::kReset << "\n";
                panel->Render();
                std::cout << "\n";
            }
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
                // read_file / get_file_outline 仅保留头部，不显示详细内容
                if (tc.name != "read_file" && tc.name != "get_file_outline") {
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
                }
            } else {
                utils::PrintError("工具执行失败: " + result.output);
            }

            // 用户请求中断整个任务：跳出工具循环，再跳出 agent 循环
            if (interrupt_loop_) break;
        }

        std::cout << "\n";

        // ── 本轮工具调用完成：推进任务、输出整体状态，再继续下一步 ──
        if (panel && !panel->Empty()) {
            if (panel->DoneCount() == done_before_iter) {
                panel->AdvanceActive();
            }
            std::cout << utils::color::kDim << "── 步骤 "
                      << panel->DoneCount() << "/" << panel->Size()
                      << " 完成 ──" << utils::color::kReset << "\n";
            panel->Render();
            std::cout << "\n";
        }

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
        ShowWriteFilePreview(path_str, content);
        auto result = registry.Execute(tc.name, tc.arguments);
        if (result.success)
            utils::PrintSuccess("已写入: " + path_str);
        else
            utils::PrintError("写入失败: " + result.output);
        return result;
    }

    // ── 精准编辑：展示 diff，自动执行，无需确认 ─────────────────────────
    if (tc.name == "edit_file") {
        std::string path_str    = tc.arguments.value("path", "");
        std::string old_content = tc.arguments.value("old_content", "");
        std::string new_content = tc.arguments.value("new_content", "");
        ShowEditFilePreview(path_str, old_content, new_content);
        auto result = registry.Execute(tc.name, tc.arguments);
        if (result.success)
            utils::PrintSuccess("已编辑: " + path_str);
        else
            utils::PrintError("编辑失败: " + result.output);
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

    // 显示项目记忆状态
    if (!project_memory_path_.empty()) {
        std::cout << "  " << utils::color::kGreen << "📋 项目记忆"
                  << utils::color::kReset << ": "
                  << utils::color::kDim
                  << utils::GetRelativePath(project_memory_path_,
                                            context_.GetWorkingDir())
                  << utils::color::kReset
                  << "  (" << utils::FormatFileSize(project_memory_content_.size())
                  << ")\n";
    } else {
        std::cout << "  " << utils::color::kDim
                  << "📋 未找到 TERMIND.md，使用 /memory init 创建项目记忆"
                  << utils::color::kReset << "\n";
    }

    std::cout << utils::color::kDim
              << "  输入 /help 查看命令，/quit 退出\n"
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
// \002 (RL_PROMPT_END_IGNORE) 包裹，否则输入满一行时会回到行首覆盖提示符。

std::string Repl::BuildPrompt() const {
    // 将 ANSI 转义序列包裹为 readline 不可见区域
    auto rl = [](const char* code) -> std::string {
        return "\001" + std::string(code) + "\002";
    };

    std::string cwd = context_.GetWorkingDir().filename().string();
    if (cwd.empty()) cwd = "/";

    // BuildContextBadge() 中的 ANSI 码也需包裹；在此直接内联构建
    const auto& cfg = ConfigManager::GetInstance().config();
    size_t est = context_.EstimateTokens();
    auto fmt_tok = [](size_t n) -> std::string {
        if (n >= 1000) {
            size_t k = n / 1000;
            size_t r = (n % 1000) / 100;
            if (r == 0) return std::to_string(k) + "k";
            return std::to_string(k) + "." + std::to_string(r) + "k";
        }
        return std::to_string(n);
    };

    std::string badge;
    if (cfg.max_context_tokens > 0) {
        int limit = cfg.max_context_tokens;
        int pct   = std::min(100, static_cast<int>(
                        est * 100 / static_cast<size_t>(limit)));
        const char* clr = (pct < 60) ? utils::color::kBrightGreen
                        : (pct < 80) ? utils::color::kYellow
                                     : utils::color::kRed;
        badge = rl(clr) +
                std::to_string(pct) + "% " +
                fmt_tok(est) + "/" + fmt_tok(static_cast<size_t>(limit)) +
                rl(utils::color::kReset) + " ";
    } else if (est > 0) {
        badge = rl(utils::color::kDim) + fmt_tok(est) +
                rl(utils::color::kReset) + " ";
    }

    return rl(utils::color::kBold) + rl(utils::color::kBrightCyan) +
           "termind" + rl(utils::color::kReset) + " " +
           badge +
           rl(utils::color::kDim) + cwd + " " + rl(utils::color::kReset) +
           rl(utils::color::kBrightCyan) + "❯ " + rl(utils::color::kReset);
}

}  // namespace termind
