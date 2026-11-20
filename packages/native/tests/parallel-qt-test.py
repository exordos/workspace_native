"""Run every QtTest function once, balancing data rows across independent processes."""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable")
    parser.add_argument("--jobs", type=int, required=True)
    parser.add_argument("--separate", nargs="+", default=[], help="functions covered by a separate CTest group")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")

    tags = subprocess.check_output([args.executable, "-datatags"], text=True).splitlines()
    functions = Counter(row.split(maxsplit=2)[1] for row in tags)
    for name in args.separate:
        if name not in functions:
            raise SystemExit(f"Separately registered QtTest function does not exist: {name}")
        del functions[name]
    if not functions:
        raise SystemExit("QtTest did not report any runnable functions")

    groups = [[] for _ in range(min(args.jobs, len(functions)))]
    sizes = [0] * len(groups)
    for function, count in functions.most_common():
        index = sizes.index(min(sizes))
        groups[index].append(function)
        sizes[index] += count

    def run(group):
        return subprocess.run([args.executable, *group], stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, text=True)

    print(f"Running {sum(functions.values())} rows in {len(groups)} processes", flush=True)
    failed = False
    with ThreadPoolExecutor(max_workers=len(groups)) as pool:
        for result in pool.map(run, groups):
            if result.returncode:
                sys.stdout.write(result.stdout)
                failed = True
            else:
                print(next((line for line in result.stdout.splitlines() if line.startswith("Totals:")),
                           "QtTest process passed"))
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
