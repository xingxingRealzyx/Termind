#pragma once

// ── Termind 通用工具 ──────────────────────────────────────────────────────
// 字符串、文件、终端、用户交互、环境变量工具。
// TUI 组件（ThinkingPane, TaskPanel, StreamRenderer）请使用 tui.h。

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace termind {
namespace utils {

// ── ANSI 颜色（保留以兼容现有调用方，同 tui::color）───────────────────
namespace color {
    constexpr const char* kReset        = "\033[0m";
    constexpr const char* kBold         = "\033[1m";
    constexpr const char* kDim          = "\033[2m";
    constexpr const char* kItalic       = "\033[3m";
    constexpr const char* kRed          = "\033[31m";
    constexpr const char* kGreen        = "\033[32m";
    constexpr const char* kYellow       = "\033[33m";
    constexpr const char* kBlue         = "\033[34m";
    constexpr const char* kMagenta      = "\033[35m";
    constexpr const char* kCyan         = "\033[36m";
    constexpr const char* kWhite        = "\033[37m";
    constexpr const char* kGray         = "\033[90m";
    constexpr const char* kBrightGreen  = "\033[92m";
    constexpr const char* kBrightYellow = "\033[93m";
    constexpr const char* kBrightCyan   = "\033[96m";
}

// ── 字符串工具 ────────────────────────────────────────────────────────────
std::string Trim(const std::string& s);
std::string TrimLeft(const std::string& s);
std::string TrimRight(const std::string& s);
std::vector<std::string> Split(const std::string& s, char delim);
std::string Join(const std::vector<std::string>& parts, const std::string& sep);
bool StartsWith(const std::string& s, const std::string& prefix);
bool EndsWith(const std::string& s, const std::string& suffix);
std::string ToLower(const std::string& s);
std::string EscapeShellArg(const std::string& s);
std::string Utf8SafeTruncate(const std::string& s, size_t max_bytes);

// 将字符串中所有非法 UTF-8 字节替换为 '?'，保证结果可安全序列化为 JSON。
// 用于处理 shell / 文件 等可能产生二进制输出的场合。
std::string SanitizeUtf8(const std::string& s);

// 剥除一行文本中的 Markdown 语法，返回适合终端直接打印的纯文本。
// in_code_block：调用方维护的代码块状态（``` 围栏切换）。
std::string StripMarkdownLine(const std::string& line, bool& in_code_block);

// ── 文件工具 ──────────────────────────────────────────────────────────────
std::optional<std::string> ReadFile(const std::filesystem::path& path);
bool WriteFile(const std::filesystem::path& path, const std::string& content);
bool FileExists(const std::filesystem::path& path);
std::string GetRelativePath(const std::filesystem::path& path,
                             const std::filesystem::path& base);
std::string FormatFileSize(std::uintmax_t bytes);

// ── 终端工具 ──────────────────────────────────────────────────────────────
int GetTerminalWidth();
int GetTerminalHeight();
bool IsAtty();
std::string Repeat(const std::string& s, int n);
void PrintHorizontalRule(const std::string& title = "");
void PrintInfo(const std::string& msg);
void PrintWarning(const std::string& msg);
void PrintError(const std::string& msg);
void PrintSuccess(const std::string& msg);
void PrintDim(const std::string& msg);
void PrintColoredDiff(const std::string& diff_text);

// ── 差异比较（调用系统 diff）────────────────────────────────────────────
std::string ComputeDiff(const std::string& old_content,
                         const std::string& new_content,
                         const std::string& filename);

// ── 用户交互 ──────────────────────────────────────────────────────────────
char AskYesNoEdit(const std::string& prompt);
bool AskYesNo(const std::string& prompt, bool default_yes = false);
std::string AskString(const std::string& prompt,
                       const std::string& default_val = "");

// ── 环境变量 ──────────────────────────────────────────────────────────────
std::string GetEnv(const std::string& name,
                   const std::string& default_val = "");
std::string ExpandHome(const std::string& path);

}  // namespace utils
}  // namespace termind
