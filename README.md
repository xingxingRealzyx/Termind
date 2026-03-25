# Termind · 端脑

> 一个运行在终端里的智能代码助手，使用现代 C++17 编写。  
> 灵感来源于 Claude Code，支持自动工具调用、文件读写确认与流式输出。

```
  ████████╗███████╗██████╗ ███╗   ███╗██╗███╗   ██╗██████╗
     ██╔══╝██╔════╝██╔══██╗████╗ ████║██║████╗  ██║██╔══██╗
     ██║   █████╗  ██████╔╝██╔████╔██║██║██╔██╗ ██║██║  ██║
     ██║   ██╔══╝  ██╔══██╗██║╚██╔╝██║██║██║╚██╗██║██║  ██║
     ██║   ███████╗██║  ██║██║ ╚═╝ ██║██║██║ ╚████║██████╔╝
     ╚═╝   ╚══════╝╚═╝  ╚═╝╚═╝     ╚═╝╚═╝╚═╝  ╚═══╝╚═════╝
                     端脑 · 终端代码助手 v0.1.0 by xingxing
```

---

## 目录

- [特性](#特性)
- [快速开始](#快速开始)
- [TUI 界面](#tui-界面)
- [架构](#架构)
- [内置工具](#内置工具)
- [Agent Skills（技能包）](#agent-skills技能包)
- [REPL 命令](#repl-命令)
- [配置](#配置)
- [构建](#构建)
- [兼容模型](#兼容模型)
- [路线图与计划](#路线图与计划)

---

## 特性

- 🖥️ **现代 TUI** — 基于 FTXUI 的交互式输入框，支持多行编辑、命令补全下拉、`@文件` 引用
- 🤖 **自动工具调用循环** — AI 自主选择工具，迭代直到任务完成（最多 50 轮）
- 📖 **项目记忆** — 自动加载 `TERMIND.md`，AI 可维护项目结构、约定与待办
- 📂 **大文件分层阅读** — `get_file_outline` 获取结构摘要，`read_file` 按行切片，节省上下文
- 🔍 **智能符号搜索** — `search_symbol` 按语言模式查找函数/类/变量定义
- ✏️ **精准文件编辑** — `edit_file` 只替换指定片段，附带 diff 预览
- ✅ **选择性确认** — `write_file`/`edit_file` 展示 diff 后自动执行；`run_shell` 需用户确认
- 🌊 **流式输出** — SSE 实时打印，`<think>` 内容在带框思考面板中滚动，正文自动剥除 Markdown 符号
- 🔌 **OpenAI 兼容** — 支持 GPT-4o、Claude、Ollama、MiniMax 等任意兼容 endpoint
- 🧩 **Agent Skills** — 兼容 Cursor 风格 `SKILL.md` 技能包：全局/项目目录自动发现，系统提示词注入摘要，按需 `load_skill` 拉取全文

---

## 快速开始

```bash
# 1. 设置 API Key
export TERMIND_API_KEY=sk-...
export TERMIND_MODEL=gpt-4o          # 可选，默认 gpt-4o

# 2. 构建并安装
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
cmake --install . --prefix ~/.local  # 安装到 ~/.local/bin/termind

# 3. 在任意目录启动
cd /your/project
termind
```

使用自定义 endpoint（如 Ollama 或 Claude）：

```bash
export TERMIND_API_BASE_URL=https://api.anthropic.com/v1
export TERMIND_MODEL=claude-3-7-sonnet-20250219
termind
```

---

## TUI 界面

### 交互式输入框

Termind 使用 [FTXUI](https://github.com/ArthurSonzogni/FTXUI) 实现类 GUI 风格的终端输入框：

| 操作 | 快捷键 |
|------|--------|
| 发送消息 | `Enter` |
| 插入换行（多行输入）| `Alt+Enter` |
| 清空输入 | `Escape` / `Ctrl+C` |
| 退出 | `Ctrl+D` |
| 历史回溯 | `←`（光标在行首时） |
| 历史前进 | `→`（光标在行尾且在历史模式时） |
| 接受补全 | `Tab` |
| 在补全列表中导航 | `↑` / `↓` |

### 命令补全

输入 `/` 时自动弹出命令下拉列表，显示名称与说明，超出 8 条时可滚动。

### 文件引用 `@`

在消息中使用 `@` 插入文件内容到上下文：

```
# 完整文件
@src/server.cpp

# 指定行范围
@src/server.cpp:42-80

# 多个文件（可连续写，无需空格分隔）
@include/server.h@src/server.cpp:1-50

# Tab 补全路径
输入 @ 后列出当前目录文件，Tab 接受第一个匹配
```

### 思考面板

`<think>` 标签内的内容不在主输出中显示，而是在带边框的思考面板中实时滚动，面板标题栏显示旋转进度指示器。

### 工具输出卡片

- **读取文件**：连续的 `read_file`/`get_file_outline` 调用合并为一张 `Read files` 卡片
- **写入/编辑**：展示 diff 预览后执行，结果显示在带框输出中
- **Shell 命令**：输出在带标题框的滚动区域中实时显示

---

## 架构

```
┌─────────────────────────────────────────────────────────────────┐
│                        termind 进程                              │
│                                                                 │
│  ┌──────────────────┐  用户输入   ┌───────────────────────────┐ │
│  │   FTXUI Input    │ ─────────→  │           Repl            │ │
│  │  • 多行编辑       │             │  • 命令分发               │ │
│  │  • 命令补全       │ ←─────────  │  • RunAgentLoop           │ │
│  │  • @文件引用      │  提示符     │  • 工具确认 / diff 预览   │ │
│  └──────────────────┘             └───────────┬───────────────┘ │
│                                               │                  │
│              ┌────────────────────────────────┼────────────┐     │
│              ↓                                ↓            ↓     │
│  ┌───────────────────┐   ┌──────────────────┐  ┌─────────────┐  │
│  │   ContextManager  │   │    AiClient      │  │ToolRegistry │  │
│  │ • 系统提示词       │   │ • Chat()         │  │ • Register  │  │
│  │ • 项目记忆         │   │ • ChatStream()   │  │ • Execute   │  │
│  │ • 对话历史         │   │ • SSE 流式解析   │  │ • 工具集    │  │
│  └───────────────────┘   └────────┬─────────┘  └──────┬──────┘  │
│                                   │                    │         │
│                          ┌────────↓────────┐    ┌──────↓──────┐  │
│                          │  ConfigManager  │    │    Tools    │  │
│                          │ • API Key/URL   │    │ read_file   │  │
│                          │ • 模型/温度     │    │ write_file  │  │
│                          └─────────────────┘    │ edit_file   │  │
│                                                 │ run_shell   │  │
│  ┌────────────────────────┐                     │ search_*    │  │
│  │       tui.cpp          │                     └─────────────┘  │
│  │ • ThinkingPane（思考框）│                                       │
│  │ • StreamRenderer        │                                       │
│  │ • FTXUI 卡片渲染        │                                       │
│  └────────────────────────┘                                       │
└─────────────────────────────────────────────────────────────────┘
                                   │ libcurl
                                   ↓
                      ┌────────────────────────┐
                      │   AI API (OpenAI 兼容)  │
                      │  POST /chat/completions │
                      │  stream: true (SSE)     │
                      └────────────────────────┘
```

### 模块职责

| 模块 | 文件 | 职责 |
|------|------|------|
| `Repl` | `repl.cpp` | REPL 主循环、FTXUI 输入、命令分发、工具确认 |
| `AiClient` | `ai_client.cpp` | libcurl HTTP 请求、SSE 流式解析、工具调用重建 |
| `ContextManager` | `context_manager.cpp` | 维护对话历史、项目记忆、构建 AI 消息列表 |
| `ToolRegistry` | `tool_registry.cpp` | 工具注册/查询/执行，内置工具 + Skills 相关工具 |
| `SkillManager` | `skill_manager.cpp` | 扫描 `SKILL.md`、解析元数据、按需返回正文与附属文件 |
| `ConfigManager` | `config.cpp` | 单例配置，支持文件 + 环境变量双来源 |
| `tui` | `tui.cpp` | ThinkingPane、StreamRenderer、FTXUI 卡片渲染 |
| `utils` | `utils.cpp` | 颜色、diff、文件读写、UTF-8 处理、Markdown 剥离 |

---

## 内置工具

AI 会根据任务自动选择下列工具，循环迭代直到完成：

| 工具 | 是否需确认 | 功能 |
|------|:---:|------|
| `read_file` | ❌ | 读取文件，支持 `start_line`/`end_line` 切片 |
| `get_file_outline` | ❌ | 获取文件结构摘要（类/函数/行号），大文件必先调用 |
| `write_file` | 自动 | 覆盖写入，展示 diff 后自动执行 |
| `edit_file` | 自动 | **精准替换**文件中某段内容，展示 diff 后自动执行 |
| `search_symbol` | ❌ | 按语言模式查找函数/类/变量定义 |
| `list_directory` | ❌ | 列出目录内容，支持递归 |
| `search_files` | ❌ | 按文件名 glob 模式搜索（`*.cpp`、`test_*` 等） |
| `grep_code` | ❌ | 在代码中搜索正则，返回匹配行 + 上下文 |
| `run_shell` | ✅ | 在工作目录执行 shell 命令，**需用户确认** |
| `get_file_info` | ❌ | 获取文件大小、类型、修改时间等元数据 |
| `update_project_memory` | ❌ | 更新 `TERMIND.md` 项目记忆 |
| `list_skills` | ❌ | 列出已发现的 Skills（名称、描述、路径、是否已加载） |
| `load_skill` | ❌ | 将指定 Skill 的 `SKILL.md` 正文注入对话上下文（供模型按指导执行） |
| `load_skill_file` | ❌ | 读取 Skill 目录下附属文件（如 `reference/`、`scripts/`），路径相对 Skill 根目录 |

> 读取工具（`read_file` / `get_file_outline`）的连续调用会在终端合并为一张卡片显示。

---

## Agent Skills（技能包）

Termind 支持 **Agent Skills** 约定：每个技能是一个独立目录，根目录下放置 **`SKILL.md`**，内含 YAML frontmatter 与正文。与 [Cursor Agent Skills](https://docs.cursor.com/context/skills) 等生态类似，可把可复用的工作流、风格规范、检查清单交给模型按需加载。

### 目录与扫描顺序

启动时会依次扫描（后者不覆盖同名：先登记的 `name` 优先）：

1. `config.json` 中的 **`skills_dirs`**（可配置多个额外目录）
2. **`~/.config/termind/skills/`** — 用户全局技能
3. **`<工作目录>/.termind/skills/`** — 当前项目专用技能

每个**直接子目录**若包含 `SKILL.md` 即视为一个 Skill；同名 Skill 只保留第一次发现的那一份。

### SKILL.md 格式

文件开头为 **YAML frontmatter**（仅支持简单 `key: value` 单行值，不支持多行值）：

```yaml
---
name: my-skill
description: 一句话说明该技能的用途，会出现在列表与系统摘要中
license: MIT   # 可选
---

# 正文（Markdown）

详细步骤、约束、示例……
```

必填字段：**`name`**（唯一标识）、**`description`**。正文在 `load_skill` 或 `/skills load` 时注入上下文；frontmatter 不会重复注入。

### 系统提示词中的行为

若存在已发现的 Skills，Termind 会在系统消息中追加 **简短列表**（名称 + 描述），并提示模型：当用户任务与某 Skill 相关时，先调用 **`list_skills`** / **`load_skill`** 获取完整指导再执行。这样不会在每条请求里塞入全部 Skill 全文，节省上下文。

### 附属文件

Skill 目录下可放 `reference/`、`scripts/` 等文件；模型通过 **`load_skill_file`** 按相对路径读取（路径会校验在 Skill 目录内，防止越界）。

### REPL 与工具的配合

| 方式 | 说明 |
|------|------|
| 自然语言 | 模型可自动 `list_skills` → `load_skill` |
| `/skills` | 终端列出所有 Skill 及是否已加载 |
| `/skills load <name>` | 手动把某 Skill 全文加入对话（等价于一次强制的 `load_skill`） |
| `/skills reload` | 重新扫描目录（例如你刚拷贝了新的 Skill 文件夹） |

---

## REPL 命令

```
对话
  直接输入问题或指令，AI 会自动调用工具完成任务

规划
  /plan <任务>      先输出执行计划，确认后再执行

文件操作
  /file  <路径>     将文件加入 AI 上下文
  /files            列出当前上下文中的所有文件
  /clearfiles       清除文件上下文
  /add   <内容>     直接附加文字片段到上下文

会话管理
  /clear            清除对话历史（保留文件上下文）
  /tokens           显示预估的 token 用量

配置
  /model <名称>     切换 AI 模型（立即生效）
  /config           查看当前运行配置

目录
  /cd    <路径>     切换工作目录（工具的相对路径基准也随之更新）
  /pwd              显示当前工作目录

项目记忆
  /memory              显示当前 TERMIND.md 内容
  /memory init         在当前目录创建 TERMIND.md 模板
  /memory edit         用 $EDITOR 打开 TERMIND.md
  /memory reload       重新加载 TERMIND.md

Skills
  /skills              列出所有可用 Skills
  /skills load <name>  手动加载 Skill 到上下文
  /skills reload       重新扫描 Skills 目录

其他
  /help             显示帮助
  /quit             退出
```

---

## 配置

### 配置文件

路径：`~/.config/termind/config.json`

```json
{
    "api_key":             "sk-...",
    "api_base_url":        "https://api.openai.com/v1",
    "model":               "gpt-4o",
    "max_tokens":          8192,
    "max_context_tokens":  200000,
    "temperature":         0.7,
    "stream":              true,
    "max_tool_iterations": 50,
    "system_prompt":       "",
    "skills_dirs":         []
}
```

`skills_dirs` 为字符串数组，可指定额外扫描路径（支持 `~` 展开）；留空则仅使用默认的 `~/.config/termind/skills` 与项目 `.termind/skills`。

### 环境变量（优先级高于配置文件）

| 变量 | 说明 |
|------|------|
| `TERMIND_API_KEY` | API 密钥（也接受 `OPENAI_API_KEY`） |
| `TERMIND_API_BASE_URL` | API 基础地址（也接受 `OPENAI_API_BASE`） |
| `TERMIND_MODEL` | 默认模型 |
| `EDITOR` | `e` 选项打开的编辑器（默认 `vi`） |

### 命令行参数

```
termind [选项]

  -c, --config <路径>   指定配置文件
  -m, --model  <名称>   覆盖模型
  -d, --dir    <路径>   设置工作目录
  --no-stream           禁用流式输出
  -h, --help            帮助
  -v, --version         版本
```

---

## 构建

### 依赖

| 依赖 | 版本 | 说明 |
|------|------|------|
| CMake | ≥ 3.20 | 构建系统 |
| GCC / Clang | 支持 C++17 | 编译器 |
| libcurl | 任意 | HTTP 请求 |
| readline | 任意 | 历史文件兼容 |
| nlohmann/json | 3.11.3 | JSON（CMake 自动下载） |
| FTXUI | 6.1.9 | TUI 框架（CMake 自动下载） |

Ubuntu / Debian 安装依赖：

```bash
sudo apt install build-essential cmake libcurl4-openssl-dev libreadline-dev
```

### 编译

```bash
mkdir build && cd build

# Release（推荐）
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# 安装到用户目录（无需 sudo）
cmake --install . --prefix ~/.local
# 确保 ~/.local/bin 在 PATH 中，之后可在任意目录运行 termind

# 安装到系统（可选，需要 sudo）
sudo cmake --install .
```

---

## 兼容模型

| 服务 | API Base URL | 推荐模型 |
|------|-------------|---------|
| OpenAI | `https://api.openai.com/v1` | `gpt-4o` |
| Anthropic | `https://api.anthropic.com/v1` | `claude-3-7-sonnet-20250219` |
| Ollama（本地）| `http://localhost:11434/v1` | `qwen2.5-coder:32b` |
| DeepSeek | `https://api.deepseek.com/v1` | `deepseek-chat` |
| 月之暗面 | `https://api.moonshot.cn/v1` | `moonshot-v1-32k` |
| MiniMax | `https://api.minimax.chat/v1` | `MiniMax-M2.7-highspeed` |

---

## 路线图与计划

以下为方向性规划，按优先级与可行性大致排序，欢迎 Issue / PR。

| 方向 | 说明 |
|------|------|
| **TUI 与组件化** | 将思考面板、工具卡片、输入框等进一步抽象为可复用组件，便于主题与布局迭代 |
| **上下文与成本** | 更智能的摘要与压缩策略、按工具类型控制注入长度、可观测的 token 统计 |
| **Skills 生态** | 内置示例 Skill、文档化与社区模板；可选从远端拉取 Skill 包（需安全校验） |
| **可扩展性** | MCP（Model Context Protocol）或插件式外部工具，对接 LSP、数据库、浏览器等 |
| **工程化** | 单元测试与 CI、静态分析、发行版与安装包（如 deb / AppImage） |
| **体验** | 更稳健的终端兼容（含 SSH）、可配置快捷键主题、多会话 / 工作区（长期） |

---

## 许可

MIT License © 2026 xingxing
