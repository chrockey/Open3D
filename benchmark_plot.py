import numpy as np
from matplotlib import pyplot as plt
from pathlib import Path
import os
import re
from scipy.stats import gmean

pwd = Path(os.path.dirname(os.path.realpath(__file__)))


def geo_mean(iterable):
    a = np.array(iterable)
    return a.prod()**(1.0 / len(a))


def match_num_thread(line):
    """
    Args:
        line: a line of text.
    Return:
        Returns None if not matched.
        Returns OMP_NUM_THREADS value if matched.
    Ref:
        https://www.tutorialspoint.com/python/python_reg_expressions.htm
    """
    pattern = r"^# OMP_NUM_THREADS: ([0-9]+)$"
    match = re.match(pattern, line)
    if match:
        # print(line)
        # print(int(match.group(1)))
        return int(match.group(1))
    else:
        return None


def match_runtime(line):
    """
    Args:
        line: a line of text.
    Return:
        Returns None if not matched.
        Returns the runtime value (float) if the value is matched.
    Ref:
        https://stackoverflow.com/a/14550569/1255535
    """
    pattern = r"^.* +(\d+(?:\.\d+)?) ms +.*ms.*$"
    match = re.match(pattern, line)
    if match:
        # print(line)
        # print(float(match.group(1)))
        return float(match.group(1))
    else:
        return None


def parse_file(log_file):
    """
    Returns: results, a list of directories, e.g.
        [
            {"num_threads": xxx, "gmean": xxx, "ICP": xxx, "Tensor": xxx},
            {"num_threads": xxx, "gmean": xxx, "ICP": xxx, "Tensor": xxx},
        ]
    """
    results = []
    with open(log_file) as f:
        lines = [line.strip() for line in f.readlines()]

        current_num_thread = None
        current_runtimes = []
        for line in lines:
            # Parse current line
            num_thread = match_num_thread(line)
            runtime = match_runtime(line)

            if num_thread:
                # If we already collected, save
                if current_num_thread:
                    results.append({
                        "num_threads": current_num_thread,
                        "gmean": gmean(current_runtimes)
                    })
                # Reset to fresh
                current_num_thread = num_thread
                runtime = []
            elif runtime:
                current_runtimes.append(runtime)

        # Save the last set of data
        if current_num_thread:
            results.append({
                "num_threads": current_num_thread,
                "gmean": gmean(current_runtimes)
            })
    return results


if __name__ == '__main__':
    fig = plt.figure()

    plt.subplot(211)
    log_file = pwd / "benchmark_4_core.log"
    results = parse_file(log_file)
    xs = [result["num_threads"] for result in results]
    ys = [result["gmean"] for result in results]
    plt.plot(xs, ys, 'b-')
    plt.title("Intel(R) Core(TM) i5-8265U (4 cores/8 threads)")
    plt.xticks(np.arange(min(xs), max(xs) + 1, 1.0))
    plt.xlabel("# of threads")
    plt.ylabel("Runtime geometric mean (ms)")

    plt.subplot(212)
    log_file = pwd / "benchmark_18_core_1_dummy.log"
    results = parse_file(log_file)
    xs = [result["num_threads"] for result in results]
    ys = [result["gmean"] for result in results]
    plt.plot(xs, ys, 'b-')
    plt.title("Intel(R) Core(TM) i9-10980XE (18 cores/36 threads)")
    plt.xticks(np.arange(min(xs), max(xs) + 1, 1.0))
    plt.xlabel("# of threads")
    plt.ylabel("Runtime geometric mean (ms)")

    fig.tight_layout()
    plt.show()
