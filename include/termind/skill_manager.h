#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace termind {

// ── 单个 Skill 的元数据与内容 ────────────────────────────────────────────
struct SkillMeta {
    std::string name;         // SKILL.md frontmatter: name
    std::string description;  // SKILL.md frontmatter: description
    std::string license;      // SKILL.md frontmatter: license（可选）
    std::filesystem::path skill_md;  // SKILL.md 的绝对路径
    std::filesystem::path dir;       // Skill 根目录
};

// ── SkillManager（单例）────────────────────────────────────────────────────
//
// 职责：
//   1. 扫描 skills 目录，发现所有含 SKILL.md 的子目录并解析元数据
//   2. 为 system prompt 提供简短摘要（name + description）
//   3. 按需返回 Skill 的完整正文或附属文件内容
//   4. 追踪哪些 Skill 已被加载到上下文，避免重复注入
class SkillManager {
public:
    static SkillManager& GetInstance();

    // 扫描目录，递归查找含 SKILL.md 的子目录。可多次调用（累积）。
    void LoadFromDir(const std::filesystem::path& dir);

    // 批量扫描
    void LoadFromDirs(const std::vector<std::filesystem::path>& dirs);

    // 清空所有已发现的 skills（通常不需要调用）
    void Clear();

    // 已发现的所有 Skill 元数据
    const std::vector<SkillMeta>& GetSkills() const { return skills_; }

    bool HasSkills() const { return !skills_.empty(); }

    // 按 name 查找元数据（大小写敏感）
    const SkillMeta* FindSkill(const std::string& name) const;

    // 返回 SKILL.md 正文（去掉 YAML frontmatter）
    std::optional<std::string> GetSkillBody(const std::string& name) const;

    // 返回 Skill 目录下的附属文件内容（路径相对于 Skill 根目录）
    std::optional<std::string> GetSkillFile(const std::string& skill_name,
                                             const std::string& relative_path) const;

    // 供 system prompt 使用的简短列表，如：
    //   - **frontend-design**: Create distinctive, production-grade...
    std::string GetSummaryBlock() const;

    // 已主动加载（load_skill 被调用过）的 skill 名称集合
    bool IsLoaded(const std::string& name) const;
    void MarkLoaded(const std::string& name);
    const std::vector<std::string>& GetLoadedNames() const { return loaded_; }

private:
    SkillManager() = default;

    // 解析单个 SKILL.md 文件，返回元数据（失败返回 nullopt）
    static std::optional<SkillMeta> ParseSkillMd(
        const std::filesystem::path& skill_md_path);

    std::vector<SkillMeta>   skills_;
    std::vector<std::string> loaded_;  // 已通过 load_skill 加载的名称
};

}  // namespace termind
