#!/usr/bin/env python3
"""
PAGE_SIZES optimizer for the leaves trie database.

Reads one or more raw allocator-size histograms produced by
`bench_page_sizes --hist_csv=PATH` (rows: ``tag,size,count``) and uses dynamic
programming to find the bucket set that minimizes total internal fragmentation
across all observed allocations.

Usage:

    # Default: aggregate every tag, solve for k = 4..10 buckets.
    python3 tools/page_sizes_solver.py /tmp/leaves_page_hist.csv

    # Restrict to a subset of tags (substring match on each).
    python3 tools/page_sizes_solver.py hist.csv --include random_binary uuid

    # Force a specific bucket count.
    python3 tools/page_sizes_solver.py hist.csv --k 8

    # Use minimax (worst-case per-tag waste) instead of weighted sum.
    python3 tools/page_sizes_solver.py hist.csv --objective minimax

The largest bucket is pinned to MAX_PAGE (default 4096, matching
PAGE_CONTAINER_SIZE in _MemoryMapTraits) and all bucket sizes are rounded up
to ALIGN bytes (default 8).
"""

from __future__ import annotations

import argparse
import csv
import sys
from collections import defaultdict
from dataclasses import dataclass
from typing import Dict, List, Sequence, Tuple


MAX_PAGE_DEFAULT = 4096
ALIGN_DEFAULT = 8
DEFAULT_MIN_BUCKET = 24  # roughly sizeof(PageHeader) + 2-branch trie node


# ---------------------------------------------------------------------------
# Histogram loading
# ---------------------------------------------------------------------------

@dataclass
class TagHist:
    tag: str
    hist: Dict[int, int]  # size -> count

    @property
    def total(self) -> int:
        return sum(self.hist.values())


def load_csv(path: str) -> List[TagHist]:
    """Parse a CSV produced by `_page_hist_dump`."""
    by_tag: Dict[str, Dict[int, int]] = defaultdict(lambda: defaultdict(int))
    with open(path, newline="") as f:
        for row in csv.reader(f):
            if not row or row[0].startswith("#"):
                continue
            if len(row) < 3:
                continue
            tag, size_s, count_s = row[0], row[1], row[2]
            try:
                size = int(size_s)
                count = int(count_s)
            except ValueError:
                continue
            if size <= 0 or count <= 0:
                continue
            by_tag[tag][size] += count
    return [TagHist(t, dict(h)) for t, h in by_tag.items()]


def filter_tags(tags: List[TagHist], include: Sequence[str]) -> List[TagHist]:
    if not include:
        return tags
    return [t for t in tags if any(s in t.tag for s in include)]


def merge_hists(tags: List[TagHist]) -> Dict[int, int]:
    out: Dict[int, int] = defaultdict(int)
    for t in tags:
        for s, c in t.hist.items():
            out[s] += c
    return dict(out)


# ---------------------------------------------------------------------------
# DP: optimal k-bucket selection
# ---------------------------------------------------------------------------

def round_up(x: int, align: int) -> int:
    return ((x + align - 1) // align) * align


def candidate_buckets(hist: Dict[int, int], align: int, max_page: int,
                      min_bucket: int) -> List[int]:
    """
    Construct the discrete set of candidate bucket sizes. Every distinct
    observed request size (rounded up to ALIGN) is a candidate, plus the
    forced endpoints (min_bucket and max_page).
    """
    cands = set()
    cands.add(round_up(min_bucket, align))
    cands.add(max_page)
    for s in hist:
        if s > max_page:
            continue
        cands.add(max(round_up(s, align), round_up(min_bucket, align)))
    return sorted(c for c in cands if c <= max_page)


def solve(hist: Dict[int, int], k: int, align: int, max_page: int,
          min_bucket: int) -> Tuple[int, List[int]]:
    """
    Find k bucket sizes minimizing total waste:

        cost = sum_s hist[s] * (bucket_ceil(s) - s)

    subject to: largest bucket == max_page, all bucket sizes <= max_page,
    all sizes 8-byte aligned, and the smallest bucket >= min_bucket.

    Returns (total_waste, buckets_in_ascending_order). Requests above the
    largest bucket are ignored (treated as not coverable).
    """
    cands = candidate_buckets(hist, align, max_page, min_bucket)
    n = len(cands)
    if k < 1 or k > n:
        raise ValueError(f"k={k} out of range for {n} candidates")

    # Items to cover: requests with size <= max_page, rounded up to ALIGN
    # boundary as the "effective size" (since buckets are aligned, a request
    # of size s consumes bucket_ceil(s) bytes regardless).
    items: List[Tuple[int, int]] = []  # (size, count)
    for s, c in hist.items():
        if s > max_page:
            continue
        items.append((s, c))
    items.sort()

    # waste_in(lo, hi) = sum_{lo < s <= hi} count(s) * (hi - s)
    # Precompute prefix sums for O(1) range queries.
    # Index items by their position; for each candidate bucket b, find the
    # range of items s in (prev_b, b].
    sizes = [s for s, _ in items]
    counts = [c for _, c in items]
    psum_count = [0]
    psum_s_count = [0]  # sum_{i<j} s_i * c_i
    for s, c in items:
        psum_count.append(psum_count[-1] + c)
        psum_s_count.append(psum_s_count[-1] + s * c)

    from bisect import bisect_right, bisect_left

    def range_waste(lo_excl: int, hi_incl: int) -> int:
        """Total waste if all items s in (lo_excl, hi_incl] go to bucket hi_incl."""
        i0 = bisect_right(sizes, lo_excl)
        i1 = bisect_right(sizes, hi_incl)
        if i0 >= i1:
            return 0
        sum_c = psum_count[i1] - psum_count[i0]
        sum_sc = psum_s_count[i1] - psum_s_count[i0]
        return hi_incl * sum_c - sum_sc

    INF = float("inf")
    # dp[j][i] = min waste using j buckets where the j-th (largest used so
    # far) bucket is cands[i], covering all items with s <= cands[i].
    dp = [[INF] * n for _ in range(k + 1)]
    parent = [[-1] * n for _ in range(k + 1)]

    # Base case: 1 bucket = cands[i] covers (0, cands[i]].
    for i in range(n):
        if cands[i] < round_up(min_bucket, align):
            continue
        dp[1][i] = range_waste(0, cands[i])

    for j in range(2, k + 1):
        for i in range(j - 1, n):
            best = INF
            best_p = -1
            for ip in range(j - 2, i):
                if dp[j - 1][ip] == INF:
                    continue
                w = dp[j - 1][ip] + range_waste(cands[ip], cands[i])
                if w < best:
                    best = w
                    best_p = ip
            dp[j][i] = best
            parent[j][i] = best_p

    # Final bucket must be max_page.
    end_idx = cands.index(max_page)
    if dp[k][end_idx] == INF:
        raise RuntimeError("No feasible bucket configuration")

    # Reconstruct.
    buckets: List[int] = []
    j, i = k, end_idx
    while j > 0:
        buckets.append(cands[i])
        i = parent[j][i]
        j -= 1
    buckets.reverse()
    return int(dp[k][end_idx]), buckets


def total_requested_bytes(hist: Dict[int, int], max_page: int) -> int:
    return sum(s * c for s, c in hist.items() if s <= max_page)


def total_requests(hist: Dict[int, int], max_page: int) -> int:
    return sum(c for s, c in hist.items() if s <= max_page)


# ---------------------------------------------------------------------------
# Evaluation: report waste per tag for a given bucket set
# ---------------------------------------------------------------------------

def evaluate(buckets: List[int], hist: Dict[int, int]) -> Tuple[int, int]:
    """Return (waste_bytes, allocated_bytes) of `hist` against `buckets`."""
    from bisect import bisect_left
    waste = 0
    alloc = 0
    for s, c in hist.items():
        idx = bisect_left(buckets, s)
        if idx >= len(buckets):
            # Not coverable; skip.
            continue
        b = buckets[idx]
        waste += c * (b - s)
        alloc += c * b
    return waste, alloc


def current_page_sizes() -> List[int]:
    """The PAGE_SIZES currently committed (same across mmap/file/mem)."""
    # Hard-coded snapshot for reference reporting. Update if traits change.
    return [40, 56, 64, 128, 176, 560, 2096, 4096]


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def fmt_pct(num: int, den: int) -> str:
    return f"{100.0 * num / den:5.2f}%" if den else "  n/a "


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", help="histogram CSV produced by bench_page_sizes")
    ap.add_argument("--k", type=int, default=0,
                    help="exact bucket count to solve for (default: sweep 4..10)")
    ap.add_argument("--k-min", type=int, default=4)
    ap.add_argument("--k-max", type=int, default=10)
    ap.add_argument("--align", type=int, default=ALIGN_DEFAULT)
    ap.add_argument("--max-page", type=int, default=MAX_PAGE_DEFAULT)
    ap.add_argument("--min-bucket", type=int, default=DEFAULT_MIN_BUCKET)
    ap.add_argument("--include", nargs="*", default=[],
                    help="only use tags whose name contains one of these substrings")
    ap.add_argument("--objective", choices=("weighted", "minimax"),
                    default="weighted")
    ap.add_argument("--current", default="",
                    help="comma-separated list of current PAGE_SIZES for the "
                         "reference baseline (default: mmap snapshot)")
    args = ap.parse_args()

    tags = load_csv(args.csv)
    if not tags:
        print(f"No histogram rows found in {args.csv}", file=sys.stderr)
        return 1

    tags = filter_tags(tags, args.include)
    if not tags:
        print("No tags matched filter", file=sys.stderr)
        return 1

    print(f"Loaded {len(tags)} tag(s) from {args.csv}:")
    for t in sorted(tags, key=lambda x: x.tag):
        print(f"  {t.tag:40s}  {t.total:>12,} allocations  "
              f"{len(t.hist):>4} distinct sizes")
    print()

    if args.objective == "weighted":
        agg = merge_hists(tags)
    else:
        # For minimax we still need a candidate basis; aggregate sets it up.
        agg = merge_hists(tags)

    ks: List[int] = [args.k] if args.k > 0 else list(range(args.k_min, args.k_max + 1))

    cur = (sorted(set(int(x) for x in args.current.split(",") if x.strip()))
           if args.current else current_page_sizes())
    print(f"Reference: current PAGE_SIZES = {cur}")
    cur_waste, cur_alloc = evaluate(cur, agg)
    print(f"  aggregate waste = {cur_waste:>12,} bytes "
          f"({fmt_pct(cur_waste, cur_alloc)} of allocated)\n")

    print("Per-tag waste with current PAGE_SIZES:")
    for t in sorted(tags, key=lambda x: x.tag):
        w, a = evaluate(cur, t.hist)
        print(f"  {t.tag:40s}  waste={w:>12,}  alloc={a:>14,}  "
              f"({fmt_pct(w, a)})")
    print()

    best_overall = None
    print(f"{'k':>3}  {'agg_waste':>14}  {'agg_pct':>8}  "
          f"{'worst_tag_pct':>14}  buckets")
    print("-" * 90)
    for k in ks:
        try:
            waste, buckets = solve(agg, k, args.align, args.max_page,
                                   args.min_bucket)
        except (ValueError, RuntimeError) as e:
            print(f"{k:>3}  {'-':>14}  {'-':>8}  {'-':>14}  ({e})")
            continue
        _, total_alloc = evaluate(buckets, agg)
        # Per-tag worst-case waste percentage.
        worst = 0.0
        for t in tags:
            w, a = evaluate(buckets, t.hist)
            if a:
                worst = max(worst, 100.0 * w / a)
        print(f"{k:>3}  {waste:>14,}  {fmt_pct(waste, total_alloc):>8}  "
              f"{worst:>13.2f}%  {buckets}")

        score = worst if args.objective == "minimax" else waste / max(total_alloc, 1)
        if best_overall is None or score < best_overall[0]:
            best_overall = (score, k, buckets, waste, total_alloc, worst)

    if best_overall is None:
        return 2

    _, bk, bbuckets, bwaste, balloc, bworst = best_overall
    print()
    print(f"=== Recommended bucket count: k = {bk} "
          f"(objective={args.objective}) ===")
    print(f"  aggregate waste : {bwaste:,} bytes ({fmt_pct(bwaste, balloc)} of allocated)")
    print(f"  worst-tag waste : {bworst:.2f}%")
    print()
    print("Per-tag waste with recommended PAGE_SIZES:")
    for t in sorted(tags, key=lambda x: x.tag):
        w, a = evaluate(bbuckets, t.hist)
        cw, ca = evaluate(cur, t.hist)
        delta = (100.0 * w / a) - (100.0 * cw / ca) if a and ca else 0.0
        sign = "+" if delta >= 0 else ""
        print(f"  {t.tag:40s}  new={fmt_pct(w, a)}  "
              f"cur={fmt_pct(cw, ca)}  (delta {sign}{delta:.2f} pp)")
    print()
    print("Drop into include/leaves/intern/storage/_mmap.hpp:")
    print()
    print("  static constexpr uint16_t PAGE_SIZES[] = {")
    for b in bbuckets:
        print(f"      {b},")
    print("  };")
    print()
    print("Note: bucket sizes include sizeof(PageHeader). Sizes are 8-byte\n"
          "aligned and the largest is pinned at PAGE_CONTAINER_SIZE.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
