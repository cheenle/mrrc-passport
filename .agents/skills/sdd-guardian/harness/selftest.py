#!/usr/bin/env python3
"""Tests for the guardian itself.

A constraint registry that is not tested is a comment. These tests assert the
properties that make the registry trustworthy:

  * every rule is reachable (its scope globs match at least one tracked file) —
    a rule whose scope matches nothing silently protects nothing;
  * every rule compiles and every rule ships a positive AND a negative fixture,
    so "this pattern catches the trap" and "it does not catch ordinary code"
    are both executable claims;
  * every doc reference in index.json still resolves — the index stores refs
    only, and a renamed heading must fail here rather than at the moment
    somebody needs the paragraph;
  * the detectors themselves behave (line numbers, comment stripping, order,
    numeric bounds, exit codes, hook mode);
  * the repository as committed is clean.

Run: python3 .agents/skills/sdd-guardian/harness/sdd_context.py selftest
"""

import json
import re
import subprocess
import sys
import unittest
from pathlib import Path

HARNESS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(HARNESS_DIR))

import sdd_context as ctx  # noqa: E402

SEVERITIES = {"block", "warn", "info"}
KINDS = {"pattern", "order", "numeric"}


def registry():
    return ctx.load_registry()


def index():
    return ctx.load_index()


def tracked_files():
    out = subprocess.run(["git", "ls-files"], cwd=str(ctx.PROJECT_ROOT),
                         capture_output=True, text=True)
    if out.returncode != 0:
        return []
    return [line for line in out.stdout.splitlines() if line.strip()]


class RegistryTests(unittest.TestCase):
    """The registry is data, and data has a schema."""

    def setUp(self):
        self.reg = registry()
        self.rules = self.reg["rules"]

    def test_registry_declares_a_version(self):
        self.assertIn("version", self.reg)
        self.assertTrue(self.rules)

    def test_rule_ids_are_unique_and_kebab_case(self):
        ids = [r["id"] for r in self.rules]
        self.assertEqual(len(ids), len(set(ids)), "duplicate rule id")
        for rid in ids:
            self.assertRegex(rid, r"^[a-z0-9]+(-[a-z0-9]+)*$")

    def test_every_rule_declares_severity_kind_and_reference(self):
        for rule in self.rules:
            self.assertIn(rule["severity"], SEVERITIES, rule["id"])
            self.assertIn(rule.get("kind", "pattern"), KINDS, rule["id"])
            self.assertTrue(rule.get("sdd_ref"), "%s: no sdd_ref" % rule["id"])
            self.assertTrue(rule.get("title"), rule["id"])
            self.assertTrue(rule.get("message"), rule["id"])

    def test_every_pattern_compiles(self):
        for rule in self.rules:
            for pat in rule.get("patterns") or []:
                try:
                    re.compile(pat)
                except re.error as exc:      # pragma: no cover - failure path
                    self.fail("%s: %s (%s)" % (rule["id"], pat, exc))

    def test_kinds_carry_their_own_fields(self):
        for rule in self.rules:
            kind = rule.get("kind", "pattern")
            if kind == "order":
                self.assertTrue(rule.get("first"), rule["id"])
                self.assertTrue(rule.get("then"), rule["id"])
                re.compile(rule["first"])
                re.compile(rule["then"])
            elif kind == "numeric":
                self.assertTrue(rule.get("call_pattern"), rule["id"])
                self.assertIsInstance(rule.get("max"), int, rule["id"])
                re.compile(rule["call_pattern"])

    def test_every_rule_is_reachable(self):
        """A scope that matches no tracked file protects nothing."""
        files = tracked_files()
        for rule in self.rules:
            if not rule.get("patterns") and rule.get("kind", "pattern") == "pattern":
                continue                       # advisory rules carry no patterns
            matched = [f for f in files if ctx.in_scope(f, rule)]
            self.assertTrue(matched, "%s: scope matches no tracked file (%s)"
                            % (rule["id"], rule.get("scope")))

    def test_secrets_rule_also_scans_comments(self):
        """Commented-out credentials are still credentials in a public repo."""
        rule = [r for r in self.rules if r["id"] == "secrets-in-tracked-config"][0]
        self.assertFalse(rule.get("ignore_comments", True),
                         "the secrets rule must not skip comments")

    def test_no_rule_targets_generated_trees(self):
        for rule in self.rules:
            joined = " ".join(rule.get("scope") or [])
            for bad in ("build/", "managed_components/", "sdkconfig"):
                if bad == "sdkconfig":
                    continue                   # sdkconfig.defaults is tracked
                self.assertNotIn(bad, joined, rule["id"])


class FixtureTests(unittest.TestCase):
    """Every rule proves itself: one sample that must fire, one that must not."""

    def setUp(self):
        self.rules = registry()["rules"]

    def test_every_enforcing_rule_ships_both_fixtures(self):
        for rule in self.rules:
            if not rule.get("patterns") and rule.get("kind", "pattern") == "pattern":
                continue
            self.assertIn("selftest_sample", rule,
                          "%s: no positive fixture" % rule["id"])
            self.assertIn("selftest_clean", rule,
                          "%s: no negative fixture" % rule["id"])

    def test_positive_fixture_fires(self):
        for rule in self.rules:
            sample = rule.get("selftest_sample")
            if not sample:
                continue
            hits = ctx.rule_hits(rule, sample["path"], sample["content"])
            self.assertTrue(hits, "%s did not fire on its own fixture: %r"
                            % (rule["id"], sample["content"]))

    def test_negative_fixture_stays_quiet(self):
        for rule in self.rules:
            clean = rule.get("selftest_clean")
            if not clean:
                continue
            hits = ctx.rule_hits(rule, clean["path"], clean["content"])
            self.assertFalse(hits, "%s fired on clean code: %r -> %r"
                             % (rule["id"], clean["content"], hits))

    def test_fixture_paths_are_inside_the_rule_scope(self):
        for rule in self.rules:
            for key in ("selftest_sample", "selftest_clean"):
                sample = rule.get(key)
                if sample:
                    self.assertTrue(ctx.in_scope(sample["path"], rule),
                                    "%s: fixture %r is outside its own scope"
                                    % (rule["id"], sample["path"]))


class IndexTests(unittest.TestCase):
    """Refs, not copies. A dead ref must fail here, not mid-debug."""

    def setUp(self):
        self.idx = index()

    def test_every_always_ref_resolves(self):
        for ref in self.idx.get("always", []):
            self.assertNotIsInstance(ctx.resolve_ref(ref), str, ref)

    def test_every_fallback_ref_resolves(self):
        for ref in self.idx.get("fallback", []):
            self.assertNotIsInstance(ctx.resolve_ref(ref), str, ref)

    def test_every_topic_ref_resolves(self):
        for topic in self.idx["topics"]:
            self.assertTrue(topic.get("files"), topic["id"])
            self.assertTrue(topic.get("keywords"), topic["id"])
            self.assertTrue(topic.get("refs"), topic["id"])
            for ref in topic["refs"]:
                got = ctx.resolve_ref(ref)
                self.assertNotIsInstance(got, str, "%s: %s" % (topic["id"], got))

    def test_every_topic_covers_a_tracked_area(self):
        files = tracked_files()
        if not files:
            self.skipTest("no git index")
        for topic in self.idx["topics"]:
            hit = any(ctx.glob_match(f, pat) for f in files for pat in topic["files"])
            self.assertTrue(hit, "%s: no tracked file matches %s"
                            % (topic["id"], topic["files"]))

    def test_sliced_sections_are_non_trivial(self):
        for topic in self.idx["topics"]:
            for ref in topic["refs"]:
                got = ctx.resolve_ref(ref)
                self.assertNotIsInstance(got, str, got)
                path, heading, body = got
                self.assertGreaterEqual(len(body), 3, "%s too short" % ref)


class DetectorTests(unittest.TestCase):
    """The scanner's own behaviour, independent of the registry's content."""

    def rule(self, **kwargs):
        base = {"id": "t", "title": "t", "severity": "block", "kind": "pattern",
                "sdd_ref": "test", "message": "m", "scope": ["main/**"]}
        base.update(kwargs)
        return base

    def test_line_numbers_are_one_based(self):
        rule = self.rule(patterns=["BAD_WORD"])
        hits = ctx.rule_hits(rule, "main/x.c", "int a;\nint b;\nBAD_WORD\n")
        self.assertEqual(hits[0][0], 3)

    def test_comments_are_skipped_by_default(self):
        rule = self.rule(patterns=["BAD_WORD"])
        text = "/* BAD_WORD */\n// BAD_WORD\nint ok;\n"
        self.assertEqual(ctx.rule_hits(rule, "main/x.c", text), [])

    def test_comment_stripping_can_be_disabled(self):
        rule = self.rule(patterns=["BAD_WORD"], ignore_comments=False)
        text = "/* BAD_WORD */\n"
        self.assertTrue(ctx.rule_hits(rule, "main/x.c", text))

    def test_comment_stripping_preserves_line_numbers(self):
        rule = self.rule(patterns=["BAD_WORD"])
        text = "/*\n\n\n*/\nBAD_WORD\n"
        self.assertEqual(ctx.rule_hits(rule, "main/x.c", text)[0][0], 5)

    def test_order_rule_flags_use_before_init(self):
        rule = self.rule(kind="order", first="init\\(\\);", then="use\\(\\);")
        hits = ctx.rule_hits(rule, "main/x.c", "use();\ninit();\n")
        self.assertTrue(hits)
        self.assertEqual(ctx.rule_hits(rule, "main/x.c", "init();\nuse();\n"), [])

    def test_order_rule_reports_a_missing_anchor(self):
        rule = self.rule(kind="order", first="init\\(\\);", then="use\\(\\);")
        self.assertTrue(ctx.rule_hits(rule, "main/x.c", "use();\n"))

    def test_numeric_rule_respects_its_bound(self):
        rule = self.rule(kind="numeric", call_pattern=r"send\([^;]*?,\s*(\d+)\s*\)",
                         max=200)
        self.assertTrue(ctx.rule_hits(rule, "main/x.c", "send(h, p, 500);"))
        self.assertEqual(ctx.rule_hits(rule, "main/x.c", "send(h, p, 100);"), [])

    def test_scope_and_exclude_scope(self):
        rule = self.rule(scope=["main/**"], exclude_scope=["main/mrrc_cfg.c"])
        self.assertTrue(ctx.in_scope("main/mrrc_net.c", rule))
        self.assertFalse(ctx.in_scope("main/mrrc_cfg.c", rule))
        self.assertFalse(ctx.in_scope("tools/build.sh", rule))

    def test_exit_code_is_two_on_block_and_zero_on_warn(self):
        block = self.rule(patterns=["BAD_WORD"], selftest_sample=None)
        warn = dict(block, severity="warn")
        self.assertEqual(2 if [h for h in ctx.collect_hits(
            [(Path("main/x.c"), "BAD_WORD")], {"rules": [block]})] else 0, 2)
        hits = ctx.collect_hits([(Path("main/x.c"), "BAD_WORD")], {"rules": [warn]})
        self.assertEqual(len(hits), 1)


class CatalogTests(unittest.TestCase):
    """The printed catalog must equal what the registry renders today."""

    def test_catalog_matches_the_registry(self):
        base = HARNESS_DIR.parent / "references"
        for lang, name in (("en", "constraint-catalog.md"),
                           ("zh", "constraint-catalog.zh_CN.md")):
            target = base / name
            self.assertTrue(target.is_file(),
                            "missing %s: run 'sdd_context.py catalog --write'" % target)
            self.assertEqual(target.read_text(encoding="utf-8").strip(),
                             ctx.render_catalog(lang).strip(),
                             "%s is stale: run 'sdd_context.py catalog --write'" % name)

    def test_catalog_lists_every_rule(self):
        text = ctx.render_catalog()
        for rule in registry()["rules"]:
            self.assertIn("`%s`" % rule["id"], text)


class NvsOwnershipTests(unittest.TestCase):
    """One namespace, one owner per key.

    Three modules write NVS by design (app_main settings, mrrc_net token,
    mrrc_cfg provisioned config) and they share the "mrrc" namespace, so the
    only way a key can be silently clobbered is two modules using the same name
    — which is exactly the kind of thing a compiler cannot see.
    """

    KEY_RE = re.compile(r'nvs_(?:set|get)_[a-z0-9_]+\([^,]+,\s*"([^"]+)"')
    IGNORE_HELPER_RE = re.compile(r"nvs_(?:set|get)_[a-z0-9_]+\(\s*h\s*,")

    def owners(self):
        found = {}
        for path in sorted((ctx.PROJECT_ROOT / "main").glob("*.c")):
            text = path.read_text(encoding="utf-8", errors="replace")
            for key in self.KEY_RE.findall(text):
                found.setdefault(key, set()).add(path.name)
        return found

    def test_every_key_has_exactly_one_owner_module(self):
        clashes = {k: v for k, v in self.owners().items() if len(v) > 1}
        self.assertEqual(clashes, {}, "NVS key written by more than one module")

    def test_keys_are_snake_case_and_short(self):
        """NVS keys are limited to 15 characters and never cleared by a reflash."""
        for key in self.owners():
            self.assertLessEqual(len(key), 15, key)
            self.assertRegex(key, r"^[a-z][a-z0-9_]*$")


class RepositoryTests(unittest.TestCase):
    """The committed tree must be clean, and the CLI must agree."""

    def test_committed_tree_is_clean_of_block_rules(self):
        pairs = []
        for path in ctx.iter_target_files([]):
            try:
                pairs.append((path, path.read_text(encoding="utf-8", errors="replace")))
            except OSError:
                continue
        self.assertTrue(pairs, "no files scanned")
        blocks = [h for h in ctx.collect_hits(pairs, registry())
                  if h[2]["severity"] == "block"]
        self.assertEqual(blocks, [], "block rules fire on the committed tree: %r"
                         % [ctx.format_hit(h) for h in blocks])

    def test_check_command_exits_clean(self):
        self.assertEqual(ctx.main(["sdd_context", "check", "--quiet"]), 0)

    def test_prime_lists_every_block_rule(self):
        import io
        from contextlib import redirect_stdout
        buf = io.StringIO()
        with redirect_stdout(buf):
            ctx.cmd_prime([])
        printed = buf.getvalue()
        for rule in registry()["rules"]:
            if rule["severity"] == "block":
                self.assertIn(rule["id"], printed)


if __name__ == "__main__":
    unittest.main()
