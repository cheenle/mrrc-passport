#!/usr/bin/env python3
"""SDD guardian harness for the MRRC terminal firmware.

Two knowledge sources, both in this directory, both resolved live:

  constraints.json — enforcement rules distilled from the incidents this
                     firmware already survived (block / warn / info).
  index.json       — routing index into the live docs tree. It stores refs
                     only, never copies: `doc:docs/transport-tuning.md#6. Transmit`
                     is sliced out of the current file at call time, so a doc
                     edit can never leave this harness quoting a stale paragraph.

Commands
  prime                          One-screen digest of the block rules.
  brief [PATHS...] [--task T]    Constraints + the relevant doc sections.
  doc <REF|TERM>                 Print one doc section, rule, or search hits.
  check [PATHS...] [--staged]    Scan content; exit 2 if a block rule fires.
  hook                           PreToolUse mode: event JSON on stdin for a
                                 pending Write/Edit; exit 2 + reason on block.
  selftest                       Run the harness's own tests.

Exit codes: 0 clean, 2 violation(s), 1 harness misconfiguration.
Stdlib only — the hook path must stay under a couple of seconds.
"""

import fnmatch
import json
import os
import re
import subprocess
import sys
from pathlib import Path

HARNESS_DIR = Path(__file__).resolve().parent
REGISTRY_PATH = HARNESS_DIR / "constraints.json"
INDEX_PATH = HARNESS_DIR / "index.json"

BRIEF_REF_LINE_CAP = 40
BRIEF_TOTAL_LINE_CAP = 260

TEXT_SUFFIXES = {
    ".c", ".h", ".cc", ".cpp", ".py", ".sh", ".json", ".md", ".yml", ".yaml",
    ".txt", ".def", ".local", ".in", ".csv",
}
SKIP_DIRS = {
    ".git", "build", "build-mac", "managed_components", "__pycache__", ".venv",
    "venv", "node_modules", ".cache", "ccache", ".ccache",
}


# ── project / registry ───────────────────────────────────────────────────────

def find_project_root():
    env = os.environ.get("SDD_GUARDIAN_ROOT")
    if env:
        return Path(env).resolve()
    # harness lives at <root>/.agents/skills/sdd-guardian/harness
    cand = HARNESS_DIR.parents[3]
    if (cand / "AGENTS.md").is_file():
        return cand
    cwd = Path.cwd().resolve()
    for base in (cwd, *cwd.parents):
        if (base / ".agents/skills/sdd-guardian/harness/constraints.json").is_file():
            return base
    return cand


PROJECT_ROOT = find_project_root()


def load_json(path):
    with open(str(path), encoding="utf-8") as fh:
        return json.load(fh)


def load_registry():
    return load_json(REGISTRY_PATH)


def load_index():
    return load_json(INDEX_PATH)


def norm(path):
    """Project-relative POSIX path, so scope globs are stable."""
    p = str(path).replace("\\", "/")
    try:
        rel = Path(p).resolve().relative_to(PROJECT_ROOT)
        return str(rel).replace("\\", "/")
    except (ValueError, OSError):
        pass
    while p.startswith("./"):
        p = p[2:]
    return p


def glob_match(path, pattern):
    if fnmatch.fnmatch(path, pattern):
        return True
    if "**/" in pattern and fnmatch.fnmatch(path, pattern.replace("**/", "")):
        return True
    return False


def in_scope(path, rule):
    if any(glob_match(path, pat) for pat in rule.get("exclude_scope", [])):
        return False
    scope = rule.get("scope") or ["**"]
    return any(glob_match(path, pat) for pat in scope)


# ── scanning ─────────────────────────────────────────────────────────────────

COMMENT_RE = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)


def strip_comments(text):
    return COMMENT_RE.sub(lambda m: "\n" * m.group(0).count("\n"), text)


def line_of(text, index):
    return text.count("\n", 0, index) + 1


def rule_hits(rule, path, text):
    """Return [(line, message)] for one rule against one file's content."""
    kind = rule.get("kind", "pattern")
    body = text
    if rule.get("ignore_comments", True) and kind in ("pattern", "numeric"):
        body = strip_comments(text)

    if kind == "pattern":
        hits = []
        for pat in rule.get("patterns") or []:
            for m in re.finditer(pat, body, re.M):
                hits.append((line_of(body, m.start()), m.group(0).strip()[:60]))
        return hits

    if kind == "order":
        first_re = re.compile(rule["first"], re.M)
        then_re = re.compile(rule["then"], re.M)
        skip_re = re.compile(rule["first_exclude"], re.M) if rule.get("first_exclude") else None
        firsts = [m.start() for m in first_re.finditer(body)
                  if not (skip_re and skip_re.search(body[max(0, m.start() - 40):m.end()]))]
        if not firsts:
            return [(1, "no '%s' at all" % rule["first"])]
        first_at = min(firsts)
        hits = [(line_of(body, m.start()), m.group(0).strip()[:60])
                for m in then_re.finditer(body) if m.start() < first_at]
        return hits

    if kind == "numeric":
        hits = []
        bound = rule["max"]
        for m in re.finditer(rule["call_pattern"], body, re.S):
            value = int(m.group(1))
            if value > bound:
                hits.append((line_of(body, m.start()), "%s > %d" % (value, bound)))
        return hits

    return []


def iter_target_files(paths):
    if paths:
        for raw in paths:
            p = Path(raw)
            if p.is_dir():
                for sub in p.rglob("*"):
                    if sub.is_file() and is_scannable(sub):
                        yield sub
            elif p.is_file():
                yield p
        return

    try:
        out = subprocess.run(["git", "ls-files"], cwd=str(PROJECT_ROOT),
                             capture_output=True, text=True)
        if out.returncode == 0:
            for line in out.stdout.splitlines():
                if not line.strip():
                    continue
                p = PROJECT_ROOT / line
                if p.is_file() and is_scannable(p):
                    yield p
            return
    except OSError:
        pass

    for sub in PROJECT_ROOT.rglob("*"):
        if sub.is_file() and is_scannable(sub):
            yield sub


def is_scannable(path):
    parts = set(Path(path).parts)
    if parts & SKIP_DIRS:
        return False
    p = Path(path)
    if p.suffix in TEXT_SUFFIXES or p.name.startswith("sdkconfig"):
        return True
    return False


def staged_blobs():
    """[(path, text)] for what is about to be committed."""
    try:
        names = subprocess.run(["git", "diff", "--cached", "--name-only", "--diff-filter=ACM"],
                               cwd=str(PROJECT_ROOT), capture_output=True, text=True)
    except OSError:
        return []
    if names.returncode != 0:
        return []
    blobs = []
    for name in names.stdout.splitlines():
        if not name.strip() or not is_scannable(PROJECT_ROOT / name):
            continue
        show = subprocess.run(["git", "show", ":%s" % name], cwd=str(PROJECT_ROOT),
                              capture_output=True)
        if show.returncode == 0:
            blobs.append((PROJECT_ROOT / name, show.stdout.decode("utf-8", "replace")))
    return blobs


def collect_hits(pairs, registry):
    """pairs = [(path, text)] -> [(path, line, rule, detail)]"""
    found = []
    for path, text in pairs:
        rel = norm(path)
        for rule in registry["rules"]:
            if not rule.get("patterns") and rule.get("kind", "pattern") == "pattern":
                continue
            if not in_scope(rel, rule):
                continue
            for line, detail in rule_hits(rule, rel, text):
                found.append((rel, line, rule, detail))
    found.sort(key=lambda h: (h[0], h[1]))
    return found


def format_hit(hit):
    rel, line, rule, detail = hit
    return "%s:%d: [%s] %s — %s" % (rel, line, rule["severity"], rule["id"], detail)


# ── docs slicing ─────────────────────────────────────────────────────────────

HEADING_RE = re.compile(r"^(#{1,6})\s+(.*)$")


def resolve_ref(ref):
    """'doc:<path>#<heading substring>' -> (path, heading, body lines) or error str."""
    if not ref.startswith("doc:"):
        return "unsupported ref grammar: %s" % ref
    spec = ref[4:]
    if "#" not in spec:
        return "ref needs '#': %s" % ref
    path, needle = spec.split("#", 1)
    fp = PROJECT_ROOT / path
    if not fp.is_file():
        return "missing file: %s" % path
    lines = fp.read_text(encoding="utf-8").splitlines()
    start = None
    for i, line in enumerate(lines):
        m = HEADING_RE.match(line)
        if m and needle.lower() in m.group(2).lower():
            start = i
            break
    if start is None:
        return "missing heading '%s' in %s" % (needle, path)
    level = len(HEADING_RE.match(lines[start]).group(1))
    end = len(lines)
    for j in range(start + 1, len(lines)):
        m = HEADING_RE.match(lines[j])
        if m and len(m.group(1)) <= level:
            end = j
            break
    body = lines[start:min(end, start + BRIEF_REF_LINE_CAP)]
    return (path, lines[start].lstrip("# ").strip(), body)


def topic_matches(topic, paths, task):
    for p in paths:
        rel = norm(p)
        for pat in topic.get("files", []):
            if glob_match(rel, pat):
                return True
    if task:
        low = task.lower()
        for kw in topic.get("keywords", []):
            if kw in low:
                return True
    return False


# ── commands ─────────────────────────────────────────────────────────────────

def cmd_prime(_args):
    reg = load_registry()
    print("MRRC terminal — constraints that are not negotiable")
    print()
    for rule in reg["rules"]:
        if rule["severity"] != "block":
            continue
        print("  ✗ %-32s %s" % (rule["id"], rule["title"]))
    print()
    print("  Read first: AGENTS.md §1; docs/design-v1.md §3 (four hard constraints)")
    print("  Full brief: python3 .agents/skills/sdd-guardian/harness/sdd_context.py brief <files>")
    return 0


def cmd_brief(args):
    task = None
    paths = []
    i = 0
    while i < len(args):
        if args[i] == "--task":
            task = args[i + 1]
            i += 2
            continue
        paths.append(args[i])
        i += 1

    reg = load_registry()
    idx = load_index()

    print("══ CONSTRAINTS ══")
    shown = 0
    for rule in reg["rules"]:
        if paths and not any(in_scope(norm(p), rule) for p in paths):
            continue
        print("  %s [%s] %s" % ("✗" if rule["severity"] == "block" else
                                "!" if rule["severity"] == "warn" else "i",
                                rule["severity"], rule["id"]))
        print("      %s" % rule["title"])
        if rule["severity"] != "info":
            print("      %s" % rule["message"])
        print("      ref: %s" % rule["sdd_ref"])
        shown += 1
    if not shown:
        print("  (nothing specific — see AGENTS.md §1)")

    print()
    print("══ KNOWLEDGE ══")
    refs = list(idx.get("always", []))
    matched = [t for t in idx["topics"] if topic_matches(t, paths, task)]
    if not matched:
        matched_refs = idx.get("fallback", [])
        matched = []
    else:
        matched_refs = []
        for t in matched:
            print("  ── %s" % t["title"])
            matched_refs.extend(t["refs"])
    for ref in refs + matched_refs:
        got = resolve_ref(ref)
        if isinstance(got, str):
            print("  ! %s (%s)" % (ref, got))
            continue
        path, heading, body = got
        print()
        print("┌─ %s  (%s)" % (heading, path))
        for line in body[:BRIEF_REF_LINE_CAP]:
            print("│ %s" % line)
        print("└────")
    return 0


def cmd_doc(args):
    if not args:
        print("usage: doc <doc:path#heading | rule-id | term>", file=sys.stderr)
        return 1
    term = args[0]
    if term.startswith("doc:"):
        got = resolve_ref(term)
        if isinstance(got, str):
            print(got, file=sys.stderr)
            return 1
        path, heading, body = got
        print("# %s  (%s)" % (heading, path))
        print()
        print("\n".join(body))
        return 0

    reg = load_registry()
    for rule in reg["rules"]:
        if rule["id"] == term:
            print("## %s — %s" % (rule["id"], rule["title"]))
            print()
            print("severity: %s   kind: %s" % (rule["severity"], rule.get("kind", "pattern")))
            print("scope:    %s" % ", ".join(rule.get("scope", [])))
            print("ref:      %s" % rule["sdd_ref"])
            print()
            print(rule["message"])
            return 0

    low = term.lower()
    hits = 0
    for rule in reg["rules"]:
        if low in rule["id"].lower() or low in rule["title"].lower():
            print("rule  %-32s %s" % (rule["id"], rule["title"]))
            hits += 1
    idx = load_index()
    for topic in idx["topics"]:
        for ref in topic.get("refs", []):
            if low in ref.lower():
                print("doc   %s" % ref)
                hits += 1
    if not hits:
        print("no match for %r" % term, file=sys.stderr)
        return 1
    return 0


def cmd_check(args):
    staged = "--staged" in args
    as_json = "--json" in args
    quiet = "--quiet" in args
    paths = [a for a in args if not a.startswith("--")]
    reg = load_registry()

    if staged:
        pairs = staged_blobs()
    else:
        pairs = []
        for p in iter_target_files(paths):
            try:
                pairs.append((p, p.read_text(encoding="utf-8", errors="replace")))
            except OSError:
                continue

    hits = collect_hits(pairs, reg)
    blocks = [h for h in hits if h[2]["severity"] == "block"]
    warns = [h for h in hits if h[2]["severity"] == "warn"]

    if as_json:
        print(json.dumps([{"path": rel, "line": line, "rule": rule["id"],
                           "severity": rule["severity"], "detail": detail}
                          for rel, line, rule, detail in hits], indent=2))
    else:
        for hit in hits:
            print(format_hit(hit))
            if hit[2]["severity"] == "block":
                print("    %s" % hit[2]["message"])
        if not quiet:
            print()
            if blocks:
                print("SDD-GUARDIAN: %d block, %d warn — %s"
                      % (len(blocks), len(warns),
                         ", ".join(sorted({h[2]["id"] for h in blocks}))))
            else:
                print("SDD-GUARDIAN: clean — no constraint violations "
                      "(%d file(s) scanned%s)."
                      % (len(pairs), ", staged only" if staged else ""))
    return 2 if blocks else 0


def cmd_hook(_args):
    try:
        event = json.load(sys.stdin)
    except (ValueError, OSError):
        return 0
    tool = event.get("tool_name") or event.get("toolName") or ""
    if tool not in ("Write", "Edit", "MultiEdit", "write", "edit"):
        return 0
    payload = event.get("tool_input") or event.get("toolInput") or {}
    fp = payload.get("file_path") or payload.get("path")
    if not fp:
        return 0
    text = payload.get("content")
    if text is None:
        chunks = [payload.get("new_string") or ""]
        for edit in payload.get("edits") or []:
            chunks.append(edit.get("newText") or edit.get("new_string") or "")
        text = "\n".join(chunks)
    reg = load_registry()
    hits = collect_hits([(Path(fp), text)], reg)
    blocks = [h for h in hits if h[2]["severity"] == "block"]
    for hit in hits:
        print(format_hit(hit), file=sys.stderr)
    if blocks:
        print("\nBlocked by sdd-guardian. %s" % blocks[0][2]["message"], file=sys.stderr)
        return 2
    return 0


CATALOG_PATH = Path("references/constraint-catalog.md")
CATALOG_HEADER = """# Constraint catalog

Generated from `constraints.json` — do not edit by hand. `selftest` fails when
this file differs from the registry, so the catalog cannot drift.

Regenerate: `python3 .agents/skills/sdd-guardian/harness/sdd_context.py catalog`

"""


CATALOG_LABELS = {
    "en": (CATALOG_HEADER,
           (("block", "Block — an incident, already paid for once"),
            ("warn", "Warn — a real cost a human may still choose"),
            ("info", "Info — read before you touch that area")),
           "| id | rule | scope | reference |",
           "<p align=\"right\">\n  <strong>English</strong> · <a href=\"constraint-catalog.zh_CN.md\">简体中文</a>\n</p>\n"),
    "zh": ("""# 约束目录

由 `constraints.json` 生成 —— 不要手改。`selftest` 会在这个文件与注册表不一致时失败，
所以目录不会漂移。

重新生成：`python3 .agents/skills/sdd-guardian/harness/sdd_context.py catalog --write`

""",
           (("block", "Block —— 已经付过一次代价的事故"),
            ("warn", "Warn —— 有真实代价，但仍可人工权衡"),
            ("info", "Info —— 动那块区域之前先读")),
           "| id | 规则 | 作用范围 | 依据 |",
           "<p align=\"right\">\n  <a href=\"constraint-catalog.md\">English</a> · <strong>简体中文</strong>\n</p>\n"),
}


def render_catalog(lang="en"):
    reg = load_registry()
    header, severities, table_head, switch = CATALOG_LABELS[lang]
    lines = [switch, "", header]
    for severity, title in severities:
        rules = [r for r in reg["rules"] if r["severity"] == severity]
        if not rules:
            continue
        lines.append("## %s\n" % title)
        lines.append(table_head)
        lines.append("| --- | --- | --- | --- |")
        for rule in rules:
            lines.append("| `%s` | %s | %s | %s |" % (
                rule["id"], rule["title"],
                ", ".join("`%s`" % s for s in rule.get("scope", [])),
                rule["sdd_ref"]))
        lines.append("")
    return "\n".join(lines)


def cmd_catalog(args):
    if "--write" in args:
        for lang, name in (("en", "constraint-catalog.md"),
                           ("zh", "constraint-catalog.zh_CN.md")):
            target = PROJECT_ROOT / ".agents/skills/sdd-guardian/references" / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(render_catalog(lang), encoding="utf-8")
            print("wrote %s" % target)
        return 0
    sys.stdout.write(render_catalog("en"))
    return 0


def cmd_selftest(_args):
    sys.path.insert(0, str(HARNESS_DIR))
    import selftest
    import unittest
    suite = unittest.defaultTestLoader.loadTestsFromModule(selftest)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 2


COMMANDS = {
    "prime": cmd_prime,
    "brief": cmd_brief,
    "doc": cmd_doc,
    "context": cmd_brief,
    "check": cmd_check,
    "hook": cmd_hook,
    "catalog": cmd_catalog,
    "selftest": cmd_selftest,
}


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help"):
        print(__doc__)
        return 0 if len(argv) >= 2 else 1
    cmd = COMMANDS.get(argv[1])
    if cmd is None:
        print("unknown command: %s" % argv[1], file=sys.stderr)
        return 1
    return cmd(argv[2:])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
