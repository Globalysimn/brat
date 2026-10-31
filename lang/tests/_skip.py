"""The status a test exits with when it could not run.

77 instead of 0, because 0 is the answer "I checked and it holds". `run_all.py`
counts this code apart from `ok` and from `FAILED`, so a suite that silently
skipped half of itself is visible in one line.
"""

SKIP = 77


def skip(why):
    """Print the reason and exit as a skip, never as a pass."""
    import sys

    print("SKIPPED: " + why)
    sys.exit(SKIP)
