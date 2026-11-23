import argparse
import subprocess
import tempfile
from pathlib import Path


INSPECT_FILES = {
    "main.c": '#include "window.h"\n\nint main(void) {\n    return retry_delay(4);\n}\n',
    "window.h": "int retry_delay(unsigned int attempt);\n",
    "window.c": '#include "window.h"\n\nint retry_delay(unsigned int attempt) {\n'
    "    return attempt >= 3 ? 30 : 5;\n}\n",
}

FAILURE = "FAIL: count with a trailing unit must be rejected"
FIXTURE_FILES = {
    "inspect": INSPECT_FILES,
    "verbose-fix": {
        "counts.h": "int parse_count(const char *text);\n",
        "counts.c": '#include "counts.h"\n#include <stdlib.h>\n\n'
        "int parse_count(const char *text) {\n    int count = atoi(text);\n"
        "    return count >= 0 && count <= 9999 ? count : -1;\n}\n",
        "test_counts.c": '#include "counts.h"\n\nint main(void) {\n'
        '    return parse_count("12jobs") != -1;\n}\n',
        "test.sh": 'set -eu\ndir=$(mktemp -d)\ntrap \'rm -rf "$dir"\' EXIT\n'
        'cc -std=c11 -o "$dir/test" counts.c test_counts.c\n'
        'awk \'BEGIN { for (i = 1; i <= 1600; i++) printf "setup %04d: checked worker configuration and input buffer\\n", i }\'\n'
        'status=0\n"$dir/test" || status=$?\n'
        f'if [ "$status" -ne 0 ]; then printf "%s\\n" "{FAILURE}"; fi\n'
        'awk \'BEGIN { for (i = 1; i <= 1600; i++) printf "cleanup %04d: released worker configuration and input buffer\\n", i }\'\n'
        'exit "$status"\n',
    },
    "multi-file": {
        "render.h": "void render(const char *text);\n",
        "render.c": '#include "render.h"\n#include <stdio.h>\n\n'
        "void render(const char *text) {\n    puts(text);\n}\n",
        "main.c": '#include "render.h"\n\nint main(int argc, char **argv) {\n'
        "    if (argc != 2) return 2;\n    render(argv[1]);\n    return 0;\n}\n",
    },
}

PROMPTS = {
    "inspect": "Inspect this code without changing any files. Report three lines, one for each of these fields: "
    "function=<name> for the function that computes the delay, "
    "threshold=<number> for the first attempt that selects the longer delay, "
    "and delay=<number> for the delay that main returns. Return only those three lines.",
    "verbose-fix": "Run `sh test.sh` to find the failure and fix `counts.c`. "
    "`parse_count` must accept only nonempty strings of decimal digits that represent values from 0 to 9999. "
    "Return -1 for other input. Keep the public signature and test files unchanged. "
    "Quote the observed failure line in your final reply.",
    "multi-file": "Add `--upper TEXT` to the renderer CLI. Convert ASCII lowercase letters to uppercase "
    "and leave other bytes unchanged. Add `void render_upper(const char *text)` to render.h and implement it "
    "in render.c; main.c must call it for the new form. Keep the existing one-argument form and exit status "
    "for wrong argument counts. Both forms print a final newline. Verify the change.",
}

COUNTS_CHECK = r'''
#include "counts.h"

int main(void) {
    const char *good[] = {"0", "0042", "9999", "00000000000000000000012"};
    const int values[] = {0, 42, 9999, 12};
    const char *bad[] = {
        "", "12jobs", " 5", "+5", "-1", "12 ", "5\n", "5\t", "5\r", "5\v", "5\f",
        "10000", "99999999999999999999"
    };
    for (unsigned i = 0; i < sizeof good / sizeof *good; i++)
        if (parse_count(good[i]) != values[i]) return 1;
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++)
        if (parse_count(bad[i]) != -1) return 1;
    return 0;
}
'''

RENDER_CHECK = r'''
#include "render.h"

int main(void) {
    void (*print)(const char *) = render_upper;
    print("Hello Zz! 42 é");
    return 0;
}
'''


RENDER_PROBE = r'''
#include "render.h"
#include <stdio.h>

void render(const char *text) { puts(text); }
void render_upper(const char *text) {
    fputs("upper dispatch: ", stdout);
    puts(text);
}
'''


def build(workspace, output, *sources, source_text=None):
    args = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I.", "-o", str(output), *sources]
    if source_text is not None:
        args.extend(["-x", "c", "-"])
    subprocess.run(args, cwd=workspace, input=source_text, text=True, check=True, timeout=20)


def matches(binary, args, expected_status, expected_stdout):
    result = subprocess.run([binary, *args], capture_output=True, timeout=5)
    return (result.returncode, result.stdout) == (expected_status, expected_stdout.encode())


def unchanged(workspace, task, names):
    return all((workspace / name).read_bytes() == FIXTURE_FILES[task][name].encode() for name in names)


def check(task, workspace, reply):
    if task == "inspect":
        actual = sorted(reply.strip().splitlines())
        expected = ["delay=30", "function=retry_delay", "threshold=3"]
        return (
            actual == expected
            and sorted(p.name for p in workspace.iterdir()) == sorted(INSPECT_FILES)
            and unchanged(workspace, task, INSPECT_FILES)
        )

    with tempfile.TemporaryDirectory() as directory:
        binary = Path(directory) / "check"
        if task == "verbose-fix":
            if FAILURE not in reply or not unchanged(workspace, task, ("counts.h", "test_counts.c", "test.sh")):
                return False
            build(workspace, binary, "counts.c", source_text=COUNTS_CHECK)
            return matches(binary, [], 0, "")

        build(workspace, binary, "render.c", source_text=RENDER_CHECK)
        if not matches(binary, [], 0, "HELLO ZZ! 42 é\n"):
            return False
        build(workspace, binary, "main.c", source_text=RENDER_PROBE)
        if not matches(binary, ["--upper", "x"], 0, "upper dispatch: x\n"):
            return False
        build(workspace, binary, "main.c", "render.c")
        return all(matches(binary, *case) for case in [
            (["Hello Zz! 42 é"], 0, "Hello Zz! 42 é\n"),
            (["--upper", "Hello Zz! 42 é"], 0, "HELLO ZZ! 42 é\n"),
            (["--upper", ""], 0, "\n"),
            ([], 2, ""),
            (["--wrong", "x"], 2, ""),
            (["--upper", "x", "y"], 2, ""),
        ])


def main():
    parser = argparse.ArgumentParser(description="Prepare and check the #82 agent benchmark tasks.")
    commands = parser.add_subparsers(dest="command", required=True)
    for command in ("prepare", "check"):
        subparser = commands.add_parser(command)
        subparser.add_argument("task", choices=FIXTURE_FILES)
        subparser.add_argument("workspace", type=Path)
        if command == "check":
            subparser.add_argument("reply", type=Path)
    args = parser.parse_args()

    try:
        if args.command == "prepare":
            args.workspace.mkdir()
            for name, content in FIXTURE_FILES[args.task].items():
                (args.workspace / name).write_bytes(content.encode())
            print(PROMPTS[args.task])
            return 0

        ok = check(args.task, args.workspace, args.reply.read_text(encoding="utf-8"))
    except (OSError, subprocess.SubprocessError) as error:
        parser.exit(1, f"{args.task}: {error}\n")
    print(f"{args.task}: {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
