#include "termind/skill_manager.h"
#include "termind/utils.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace termind {

namespace fs = std::filesystem;

// ── 单例 ──────────────────────────────────────────────────────────────────

SkillManager& SkillManager::GetInstance() {
    static SkillManager instance;
    return instance;
}

// ── 解析 SKILL.md ─────────────────────────────────────────────────────────
//
// SKILL.md 格式：
//   ---
//   name: skill-name
//   description: Some description text
//   license: Optional license
//   ---
//
//   # Skill Body
//   ...
//
// 只解析简单的 "key: value" frontmatter（不支持多行值）。

std::optional<SkillMeta> SkillManager::ParseSkillMd(const fs::path& path) {
    std::ifstream f(path);
    if (!f.is_open()) return std::nullopt;

    std::string line;

    // 跳过空行，找到第一个 "---"
    bool found_open = false;
    while (std::getline(f, line)) {
        line = utils::Trim(line);
        if (line == "---") { found_open = true; break; }
        if (!line.empty()) break;  // 没有 frontmatter
    }

    if (!found_open) return std::nullopt;

    // 解析 key: value 直到下一个 "---"
    SkillMeta meta;
    meta.skill_md = fs::absolute(path);
    meta.dir      = path.parent_path();

    while (std::getline(f, line)) {
        std::string trimmed = utils::Trim(line);
        if (trimmed == "---") break;  // frontmatter 结束

        // 找第一个 ":"
        size_t colon = trimmed.find(':');
        if (colon == std::string::npos) continue;

        std::string key = utils::Trim(trimmed.substr(0, colon));
        std::string val = utils::Trim(trimmed.substr(colon + 1));

        if (key == "name")        meta.name        = val;
        else if (key == "description") meta.description = val;
        else if (key == "license")     meta.license     = val;
    }

    if (meta.name.empty()) return std::nullopt;

    return meta;
}

// ── 扫描目录 ──────────────────────────────────────────────────────────────

void SkillManager::LoadFromDir(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;

    for (const auto& entry : fs::directory_iterator(
             dir, fs::directory_options::skip_permission_denied, ec)) {
        if (!entry.is_directory()) continue;

        fs::path skill_md = entry.path() / "SKILL.md";
        if (!fs::exists(skill_md)) continue;

        auto meta = ParseSkillMd(skill_md);
        if (!meta) continue;

        // 避免重复（同 name 已存在则跳过）
        bool dup = std::any_of(skills_.begin(), skills_.end(),
                               [&](const SkillMeta& s) {
                                   return s.name == meta->name;
                               });
        if (!dup) {
            skills_.push_back(std::move(*meta));
        }
    }
}

void SkillManager::LoadFromDirs(const std::vector<fs::path>& dirs) {
    for (const auto& d : dirs) LoadFromDir(d);
}

void SkillManager::Clear() {
    skills_.clear();
    loaded_.clear();
}

// ── 查找 ──────────────────────────────────────────────────────────────────

const SkillMeta* SkillManager::FindSkill(const std::string& name) const {
    for (const auto& s : skills_) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

// ── 返回 SKILL.md 正文（去掉 frontmatter）────────────────────────────────

std::optional<std::string> SkillManager::GetSkillBody(
    const std::string& name) const {
    const SkillMeta* meta = FindSkill(name);
    if (!meta) return std::nullopt;

    std::ifstream f(meta->skill_md);
    if (!f.is_open()) return std::nullopt;

    std::string line;
    bool in_frontmatter = false;
    bool past_frontmatter = false;
    int dash_count = 0;

    std::ostringstream body;

    while (std::getline(f, line)) {
        std::string trimmed = utils::Trim(line);

        if (!past_frontmatter) {
            if (trimmed == "---") {
                ++dash_count;
                if (dash_count == 1) { in_frontmatter = true; continue; }
                if (dash_count == 2) { past_frontmatter = true; in_frontmatter = false; continue; }
            }
            if (!in_frontmatter && dash_count == 0 && !trimmed.empty()) {
                // 没有 frontmatter，直接输出
                past_frontmatter = true;
                body << line << "\n";
            }
            continue;
        }

        body << line << "\n";
    }

    return body.str();
}

// ── 返回附属文件内容 ──────────────────────────────────────────────────────

std::optional<std::string> SkillManager::GetSkillFile(
    const std::string& skill_name,
    const std::string& relative_path) const {
    const SkillMeta* meta = FindSkill(skill_name);
    if (!meta) return std::nullopt;

    fs::path full_path = meta->dir / relative_path;
    // 安全性：确保路径不超出 skill 目录
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(full_path, ec);
    if (ec) return std::nullopt;
    fs::path skill_dir = fs::weakly_canonical(meta->dir, ec);
    if (ec) return std::nullopt;

    // 检查 canonical 是否以 skill_dir 为前缀
    auto mismatch_pair = std::mismatch(skill_dir.begin(), skill_dir.end(),
                                        canonical.begin());
    if (mismatch_pair.first != skill_dir.end()) {
        return std::nullopt;  // path traversal 攻击
    }

    return utils::ReadFile(canonical);
}

// ── System prompt 摘要块 ──────────────────────────────────────────────────

std::string SkillManager::GetSummaryBlock() const {
    if (skills_.empty()) return "";

    std::ostringstream ss;
    ss << "## 可用 Skills\n"
       << "以下 Skills 已加载，可按需调用 `load_skill` 工具获取完整指导：\n\n";
    for (const auto& s : skills_) {
        ss << "- **" << s.name << "**: " << s.description << "\n";
    }
    ss << "\n当用户的请求与某个 Skill 相关时，先调用 `load_skill` 加载其完整内容，"
          "再执行任务。\n";
    return ss.str();
}

// ── 已加载追踪 ────────────────────────────────────────────────────────────

bool SkillManager::IsLoaded(const std::string& name) const {
    return std::find(loaded_.begin(), loaded_.end(), name) != loaded_.end();
}

void SkillManager::MarkLoaded(const std::string& name) {
    if (!IsLoaded(name)) loaded_.push_back(name);
}

}  // namespace termind
