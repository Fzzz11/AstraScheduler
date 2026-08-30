"""AST-001 发布规则门禁：仓库文档/配置测试。

Source ticket: .scratch/astra-scheduler-runtime/issues/01-release-rule-gates.md
Primary rules: R-004, R-005, R-094, R-112.

这些测试审计规格、已发布的 Ticket 与发布规划入口，使"批准 Spec 才可拆票/实现"
策略和 Phase 0 -> v1.0 里程碑交付矩阵由 CI 入口强制校验，而不是依赖聊天上下文。

本地运行（R-112：必须在 WSL Linux 用户空间执行）：

    wsl bash -lc "cd /mnt/d/code/cppStudy/AstraScheduler && python3 -X utf8 tools/check_release_gates.py"

Python 兼容性：3.8+（WSL 基线为 python3.8 / 3.8.10；CI 固定 3.8 保持一致）。
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SPEC_PATH = REPO_ROOT / ".scratch" / "astra-scheduler-runtime" / "spec.md"
ISSUES_DIR = REPO_ROOT / ".scratch" / "astra-scheduler-runtime" / "issues"
MILESTONES_PATH = REPO_ROOT / "docs" / "release" / "milestones.md"
CI_WORKFLOW_PATH = REPO_ROOT / ".github" / "workflows" / "ci.yml"
AGENTS_PATH = REPO_ROOT / "AGENTS.md"
DEVELOPMENT_PATH = REPO_ROOT / "docs" / "development.md"
AST001_PATH = ISSUES_DIR / "01-release-rule-gates.md"

# R-094 里程碑序列，按交付顺序排列。Phase 0 为无 tag 的脚手架。
ALLOWED_MILESTONES = (
    "Phase 0",
    "v0.1.0",
    "v0.2.0",
    "v0.3.0",
    "v0.4.0",
    "v0.5.0",
    "v0.6.0",
    "v0.7.0",
    "v0.8.0",
    "v0.9.0",
    "v1.0.0",
    "v1.1.0",
    "v1.2.0",
)

RULE_HEADER_RE = re.compile(r"^### (R-\d{3}) — .*$", re.MULTILINE)
RULE_BLOCK_RE = re.compile(r"^### (R-\d{3}) — .*?(?=^### |\Z)", re.MULTILINE | re.DOTALL)
APPLIES_TO_RE = re.compile(r"^Applies to: (.+?)\s*$", re.MULTILINE)
MILESTONE_FIELD_RE = re.compile(r"^Milestone: (.+?)\s*$", re.MULTILINE)
PRIMARY_RULES_RE = re.compile(r"^- (.+?) \[primary\]", re.MULTILINE)
MATRIX_ROW_RE = re.compile(r"^\|\s*(.+?)\s*\|", re.MULTILINE)


def read_required(path: Path) -> str:
    if not path.is_file():
        raise FileNotFoundError(f"required file is missing: {path}")
    return path.read_text(encoding="utf-8")


def parse_rule_blocks(spec_text: str) -> dict[str, str]:
    """将 spec 按规则 id 切分为规则块。"""
    return {
        match.group(1): match.group(0)
        for match in RULE_BLOCK_RE.finditer(spec_text)
    }


def section_body(testcase: unittest.TestCase, markdown: str, heading: str) -> str:
    """返回 '## <heading>' 节的内容（节缺失时失败）。"""
    marker = f"## {heading}"
    testcase.assertIn(marker, markdown, f"document is missing the '## {heading}' section")
    start = markdown.index(marker) + len(marker)
    next_section = markdown.find("\n## ", start)
    return markdown[start:] if next_section == -1 else markdown[start:next_section]


def text_before(testcase: unittest.TestCase, markdown: str, heading: str) -> str:
    """返回 '## <heading>' 节之前的内容（节缺失时失败）。"""
    marker = f"## {heading}"
    testcase.assertIn(marker, markdown, f"document is missing the '## {heading}' section")
    return markdown.split(marker, 1)[0]


def subsection_body(testcase: unittest.TestCase, markdown: str, heading: str) -> str:
    """返回 '### <heading>' 子节的内容。"""
    marker = f"### {heading}"
    testcase.assertIn(marker, markdown, f"document is missing the '### {heading}' subsection")
    start = markdown.index(marker) + len(marker)
    next_subsection = markdown.find("\n### ", start)
    next_section = markdown.find("\n## ", start)
    candidates = [end for end in (next_subsection, next_section) if end != -1]
    end = min(candidates) if candidates else len(markdown)
    return markdown[start:end]


class R004SpecScopeTests(unittest.TestCase):
    """R-004：spec 覆盖跨版本 Runtime，且每条规则声明其适用范围。"""

    @classmethod
    def setUpClass(cls):
        cls.spec_text = read_required(SPEC_PATH)

    def test_R004_spec_status_is_approved(self):
        """门禁：只有 approved 状态的 spec 才能支撑拆票/实现工作。"""
        header = self.spec_text.split("## ", 1)[0]
        self.assertRegex(header, r"(?m)^Status: approved\b")

    def test_R004_every_rule_has_nonempty_applies_to(self):
        blocks = parse_rule_blocks(self.spec_text)
        rule_headers = RULE_HEADER_RE.findall(self.spec_text)
        self.assertTrue(rule_headers, "spec contains no rule definitions")
        self.assertEqual(
            len(blocks), len(rule_headers), "rule block parsing lost some rules"
        )
        missing = [
            rule_id
            for rule_id, block in sorted(blocks.items())
            if not [m for m in APPLIES_TO_RE.findall(block) if m.strip()]
        ]
        self.assertEqual(
            missing,
            [],
            "rules without a non-empty 'Applies to' scope: " + ", ".join(missing),
        )

    def test_R004_spec_states_cross_version_scope(self):
        """整体目标不得被收窄为单一版本范围。"""
        problem_statement = section_body(self, self.spec_text, "Problem Statement")
        goals = section_body(self, self.spec_text, "Goals")
        preamble = text_before(self, self.spec_text, "Problem Statement")
        self.assertIn("v0.1.0", goals + problem_statement)
        self.assertIn("v1.0", goals + problem_statement)
        self.assertIn("纵向里程碑", goals)
        self.assertIn("跨版本", preamble + goals)


class R005TicketVersioningTests(unittest.TestCase):
    """R-005：实现工作拆分为多个 Ticket，且每个 Ticket 记录目标版本。"""

    @classmethod
    def setUpClass(cls):
        cls.spec_text = read_required(SPEC_PATH)
        cls.ticket_paths = sorted(ISSUES_DIR.glob("*.md"))
        cls.tickets = {
            path.name: path.read_text(encoding="utf-8") for path in cls.ticket_paths
        }

    def test_R005_issue_directory_exists_with_tickets(self):
        self.assertTrue(ISSUES_DIR.is_dir(), f"missing issues directory: {ISSUES_DIR}")
        self.assertGreaterEqual(
            len(self.tickets), 1, "no published tickets found in the tracker"
        )

    def test_R005_every_ticket_records_exactly_one_target_milestone(self):
        offenders = {}
        for name, text in self.tickets.items():
            milestones = [
                m.strip() for m in MILESTONE_FIELD_RE.findall(text) if m.strip()
            ]
            if len(milestones) != 1 or milestones[0] not in ALLOWED_MILESTONES:
                offenders[name] = milestones
        self.assertEqual(
            offenders,
            {},
            "tickets without exactly one allowed Milestone value: "
            + repr(offenders),
        )

    def test_R005_no_single_ticket_spans_the_whole_project(self):
        blocks = parse_rule_blocks(self.spec_text)
        active_rules = {
            rule_id
            for rule_id, block in blocks.items()
            if re.search(r"^Status: active\b", block, re.MULTILINE)
        }
        self.assertGreaterEqual(len(active_rules), 1)
        for name, text in sorted(self.tickets.items()):
            primary = [
                rule.strip() for rule in PRIMARY_RULES_RE.findall(text)
            ]
            for rule in primary:
                self.assertIn(
                    rule,
                    blocks,
                    f"{name} references rule {rule} that is not defined in the spec",
                )
            covered = {r for r in primary if r in active_rules}
            self.assertLess(
                len(covered),
                len(active_rules),
                f"{name} claims primary ownership of every active rule; "
                "the project must not collapse into one implementation ticket",
            )


class R094MilestoneGateTests(unittest.TestCase):
    """R-094：Phase 0 至 v1.0 以可独立构建的里程碑 tag 交付。"""

    @classmethod
    def setUpClass(cls):
        cls.milestones_text = read_required(MILESTONES_PATH)

    def test_R094_milestone_matrix_lists_all_tags_in_order(self):
        matrix_rows = [
            row.strip() for row in MATRIX_ROW_RE.findall(self.milestones_text)
        ]
        tags_in_document_order = [
            row for row in matrix_rows if row in ALLOWED_MILESTONES
        ]
        self.assertEqual(
            tags_in_document_order,
            list(ALLOWED_MILESTONES),
            "milestone matrix must list every tag exactly once (Phase 0 through "
            "v1.2.0), in delivery order",
        )

    def test_R094_every_milestone_row_carries_scope_and_release_evidence(self):
        rows = [
            [cell.strip() for cell in line.strip().strip("|").split("|")]
            for line in self.milestones_text.splitlines()
            if line.lstrip().startswith("|")
        ]
        rows_by_tag = {
            cells[0]: cells for cells in rows if cells and cells[0] in ALLOWED_MILESTONES
        }
        for tag in ALLOWED_MILESTONES:
            self.assertIn(tag, rows_by_tag, f"matrix is missing a row for {tag}")
            cells = rows_by_tag[tag]
            self.assertGreaterEqual(
                len(cells),
                3,
                f"milestone row for {tag} must carry scope and release evidence",
            )
            self.assertTrue(cells[1], f"milestone {tag} has an empty scope cell")
            self.assertTrue(cells[2], f"milestone {tag} has an empty evidence cell")

    def test_R094_milestone_matrix_fixes_per_tag_definition_of_done(self):
        dod = section_body(
            self, self.milestones_text, "Definition of Done（每个 tag 的统一 DoD）"
        )
        for keyword in (
            "approved-rule",
            "Tier-1",
            "并发",
            "docs",
            "package",
            "schema",
            "benchmark",
            "独立构建",
        ):
            self.assertIn(
                keyword,
                dod,
                f"milestone matrix Definition of Done must mention '{keyword}' "
                "(R-094: approved-rule tests, Tier-1 build, concurrency evidence, "
                "docs/package/schema/benchmark gates, independently buildable tag)",
            )

    def test_R094_milestone_matrix_provides_rule_traceability_entry_points(self):
        for link_fragment in (
            "spec.md",
            "decision-log.md",
            "issues/",
        ):
            self.assertIn(
                link_fragment,
                self.milestones_text,
                "milestone matrix must link the rule traceability entry point "
                f"'{link_fragment}'",
            )

    def test_R094_ci_entry_point_runs_the_gate_checks(self):
        workflow = read_required(CI_WORKFLOW_PATH)
        self.assertIn(
            "check_release_gates.py",
            workflow,
            "CI entry point must run the release rule gate tests",
        )
        self.assertIn("python", workflow)


class R112WslDevelopmentGateTests(unittest.TestCase):
    """R-112：本机开发与验证命令在 WSL Linux 内执行。"""

    @classmethod
    def setUpClass(cls):
        cls.agents_text = read_required(AGENTS_PATH)
        cls.development_text = read_required(DEVELOPMENT_PATH)
        cls.milestones_text = read_required(MILESTONES_PATH)
        cls.ast001_text = read_required(AST001_PATH)

    def test_R112_project_instructions_fix_linux_only_wsl_entry(self):
        for token in (
            "仅限 64-bit Linux",
            "/mnt/d/code/cppStudy/AstraScheduler",
            "wsl bash -lc",
            "不得在 Windows native 与 WSL 之间复用",
        ):
            self.assertIn(token, self.agents_text)

    def test_R112_development_guide_fixes_wsl_commands_and_cache_isolation(self):
        for token in (
            "Tier-1：Linux x86_64",
            "wsl bash -lc",
            "build/wsl-gcc-debug",
            "不得在 Windows native 与 WSL 之间复用",
        ):
            self.assertIn(token, self.development_text)

    def test_R112_milestone_document_uses_wsl_for_local_verification(self):
        local_gate = subsection_body(
            self, self.milestones_text, "Local WSL development gate"
        )
        self.assertIn("wsl bash -lc", local_gate)
        self.assertIn("/mnt/d/code/cppStudy/AstraScheduler", local_gate)
        self.assertNotRegex(local_gate, r"(?m)^python(?:3)?\s")

    def test_R112_completed_AST001_owns_rule_and_records_wsl_evidence(self):
        self.assertRegex(self.ast001_text, r"(?m)^- R-112 \[primary\]")
        verification = subsection_body(
            self, self.ast001_text, "Verification commands"
        )
        command_lines = [
            line for line in verification.splitlines() if line.startswith("- `")
        ]
        self.assertTrue(command_lines, "AST-001 records no verification commands")
        for line in command_lines:
            self.assertIn("wsl bash -lc", line)


if __name__ == "__main__":
    unittest.main(verbosity=2)
