#!/usr/bin/env python3
"""Regression tests for scripts/gen-test-buckets.py (issue #675, exordy round).

Each test reproduces a review finding against the pre-fix script:
  finding 1/5: the --check comparison dropped the indented bucket member
               lines, so removing or duplicating a case in the layout passed
               the guard while changing what ctest runs.
  finding 2:   regeneration looped forever when the current bucket count
               already had an empty bucket (the loop only grew the count).
  finding 3:   the printed regeneration command dropped --only-file/--exclude,
               so the suggested command failed for the filtered e2ee suites.
  finding 4:   tiny timings were written as scientific notation, which the
               fragment reader cannot parse, poisoning the next --check.
The re-review round added:
  re-review 1: pick_count only grew the count, so a crc32 collision at the
               committed count reported "no valid count" although a smaller
               count packed every bucket non-empty under the ceiling.
  re-review 2: the --check filter dropped every '#'-leading line, so a CMake
               bracket comment (standalone "#[[" closed by "#]]#") could hide
               a member from the parsed set() while the guard still passed.
"""
import importlib.util
import os
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE.parent / "scripts" / "gen-test-buckets.py"

spec = importlib.util.spec_from_file_location("gen_test_buckets", SCRIPT)
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)

FUNCTIONS = [f"caseAlpha{i}" for i in range(12)]
FAKE_BINARY = textwrap.dedent(
    """\
    #!/usr/bin/env python3
    import sys
    if sys.argv[1:] == ["-functions"]:
        for name in NAME_PLACEHOLDER:
            print(f"{name}()")
    """,
)


def fake_binary(dirpath, names):
    path = dirpath / "fake-test"
    path.write_text(
        FAKE_BINARY.replace(
            "NAME_PLACEHOLDER", repr(names)
        )
    )
    path.chmod(0o755)
    return path


def timing_xml(path, pairs):
    entries = "\n".join(
        f'  <TestFunction name="{name}"><Duration msecs="{ms:.3f}"/></TestFunction>'
        for name, ms in pairs
    )
    path.write_text(
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        "<TestCase name=\"FakeTest\">\n" + entries + "\n</TestCase>\n"
    )


class GenTestBucketsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="gen-buckets-test."))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.binary = fake_binary(self.tmp, FUNCTIONS)
        self.out = self.tmp / "layout.cmake"
        self.fragments = self.tmp / "frags"
        timing = self.tmp / "run.xml"
        timing_xml(timing, [(name, 2500.0 + 100.0 * i)
                            for i, name in enumerate(FUNCTIONS)])
        self.gen = [sys.executable, str(SCRIPT)]
        subprocess.run(
            self.gen + ["--binary", str(self.binary), "--out", str(self.out),
                        "--fragments", str(self.fragments),
                        "--timings", str(timing), "--ceiling", "12"],
            check=True, capture_output=True, text=True,
        )

    def check(self, *extra):
        return subprocess.run(
            self.gen + ["--check", "--binary", str(self.binary),
                        "--out", str(self.out), "--fragments", str(self.fragments),
                        "--ceiling", "12"] + list(extra),
            capture_output=True, text=True,
        )

    def test_check_detects_member_line_removed(self):
        """finding 1/5: a layout with one member line deleted must fail --check."""
        lines = self.out.read_text().splitlines()
        victim = next(line for line in lines if line.startswith("    "))
        tampered = [line for line in lines if line != victim]
        self.out.write_text("\n".join(tampered) + "\n")
        result = self.check()
        self.assertNotEqual(result.returncode, 0,
                            f"guard accepted a layout missing {victim!r}")

    def test_check_detects_member_line_duplicated(self):
        """finding 1/5: a layout with one member line duplicated must fail --check."""
        lines = self.out.read_text().splitlines()
        victim = next(line for line in lines if line.startswith("    "))
        index = lines.index(victim)
        lines.insert(index + 1, victim)
        self.out.write_text("\n".join(lines) + "\n")
        result = self.check()
        self.assertNotEqual(result.returncode, 0,
                            f"guard accepted a layout duplicating {victim!r}")

    def test_clean_layout_passes_check(self):
        result = self.check()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_regeneration_terminates_on_empty_current_buckets(self):
        """finding 2: more part fragments than bucketable functions must not hang."""
        # Shrink the bucketable set far below the existing part count.
        keep = FUNCTIONS[:3]
        binary = fake_binary(self.tmp, keep)
        shutil.rmtree(self.fragments)
        self.fragments.mkdir()
        # Stale layout: 5 part fragments for only 3 bucketable functions, so
        # the committed count can only grow into permanently-empty buckets.
        for i, name in enumerate(keep):
            (self.fragments / f"part{i:02d}.txt").write_text(f"{name}=2.6\n")
        (self.fragments / "part03.txt").write_text("")
        (self.fragments / "part04.txt").write_text("")
        result = subprocess.run(
            self.gen + ["--binary", str(binary), "--out", str(self.out),
                        "--fragments", str(self.fragments), "--ceiling", "60"],
            capture_output=True, text=True, timeout=30,
        )
        # Either a clean re-render or an explicit report - never a hang (the
        # subprocess timeout above is the hang detector for the pre-fix loop).
        self.assertEqual(
            result.returncode, 0,
            f"regeneration must report, got rc={result.returncode}: "
            + result.stdout + result.stderr)
        names = re.findall(r"^    (\S+)$", self.out.read_text(), re.M)
        self.assertEqual(sorted(names), sorted(keep))
        parts = [p for p in self.fragments.iterdir() if p.name.startswith("part")]
        self.assertLessEqual(len(parts), len(keep))

    def test_check_message_keeps_function_filters(self):
        """finding 3: the regeneration hint must include --only-file/--exclude."""
        only = self.tmp / "only.txt"
        only.write_text("\n".join(FUNCTIONS[:8]) + "\n")
        excluded = FUNCTIONS[3]
        binary = fake_binary(self.tmp, FUNCTIONS)
        self.out.write_text("set(PREFIX_BUCKETS_COUNT 99)\n")
        result = self.check("--prefix", "PREFIX",
                            "--only-file", str(only), "--exclude", excluded)
        self.assertNotEqual(result.returncode, 0)
        output = result.stdout + result.stderr
        self.assertIn("--only-file", output)
        self.assertIn("--exclude", output)

    def test_check_detects_bracket_commented_member(self):
        """re-review finding: CMake bracket comments may hide a member.

        CMake treats everything between a standalone "#[[" line and "#]]#"
        as a comment, so the member is gone from the parsed set() (verified
        against cmake -P). Both boundary lines start with '#', so the old
        comparison that dropped every '#'-leading line kept the member in
        the comparison and accepted the layout. The guard must compare the
        complete rendering, comments included."""
        lines = self.out.read_text().splitlines()
        victim = next(line for line in lines if line.startswith("    "))
        index = lines.index(victim)
        tampered = lines[:index] + ["#[[", victim, "#]]#"] + lines[index + 1:]
        self.out.write_text("\n".join(tampered) + "\n")
        result = self.check()
        self.assertNotEqual(
            result.returncode, 0,
            f"guard accepted a layout hiding {victim!r} in a bracket comment")

    def test_check_rejects_bracket_token_in_membership_line(self):
        """The membership header is exempt from comparison only when clean.

        Appending "#[[" there would comment out the whole set() body while
        the exempt line hides the token from a naive comparison."""
        lines = self.out.read_text().splitlines()
        self.assertIn("# membership: ", lines[1])
        lines[1] = lines[1] + " #[" + "["  # the bracket-comment opener
        self.out.write_text("\n".join(lines) + "\n")
        result = self.check()
        self.assertNotEqual(result.returncode, 0,
                            "guard ignored a bracket token in the header line")

    def test_pick_count_tries_smaller_counts(self):
        """re-review finding: the bounded search must try smaller counts.

        Three cases whose crc32 mod 3 collides leave bucket 3 empty, and
        the search is clamped at the function count (3), so growing can
        never help. Count 2 packs all three cases non-empty under the
        ceiling, and the old search never tried it and reported no valid
        count (probeCase0/1/5 collide at mod 3, split across mod 2)."""
        per = {name: 1.0 for name in ("probeCase0", "probeCase1", "probeCase5")}
        for name in per:
            self.assertNotEqual(gen.bucket_index(name, 3), 2,
                                "fixture lost its mod-3 collision property")
        count = gen.pick_count(per, 60.0, 5)
        self.assertIsNotNone(count,
                             "count 2 is valid but the search only grew")
        buckets = gen.assign(per, count)
        self.assertTrue(all(buckets), "returned count leaves an empty bucket")
        self.assertLessEqual(max(gen.bucket_secs(buckets, per)), 60.0)

    def test_fragment_values_are_reader_parseable(self):
        """finding 4: sub-millisecond timings must not be written in scientific form."""
        keep = FUNCTIONS[:6]
        binary = fake_binary(self.tmp, keep)
        shutil.rmtree(self.fragments)
        self.fragments.mkdir()
        timing = self.tmp / "tiny.xml"
        timing_xml(timing, [(name, 0.05 * (i + 1)) for i, name in enumerate(keep)])
        out = self.tmp / "tiny.cmake"
        subprocess.run(
            self.gen + ["--binary", str(binary), "--out", str(out),
                        "--fragments", str(self.fragments),
                        "--timings", str(timing), "--ceiling", "60"],
            check=True, capture_output=True, text=True,
        )
        reloaded = gen.load_fragments(str(self.fragments))
        self.assertEqual(sorted(reloaded), sorted(keep))
        for name, seconds in reloaded.items():
            self.assertGreater(seconds, 0.0, f"{name} lost its sub-ms timing")


if __name__ == "__main__":
    unittest.main(verbosity=2)
