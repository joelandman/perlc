#!/usr/bin/env python3
"""tools/gap_report.py — Phase-0 runtime-gap inventory reporter.

Aggregates the JSONL sidecars emitted by perlc's runtime gap detector
(see src/runtime.c, gated on the PERLC_GAPS env var) into a per-target
checklist and a corpus-wide triage queue.

Usage:
    python3 tools/gap_report.py tests/realworld/gaps/*.jsonl [--tsv gaps.tsv]

Each sidecar file is one compiled script's recorded gaps (one JSON object
per line).  Reports:
  * corpus summary (targets scanned, scripts with gaps, distinct gaps)
  * "implement now" — broadly-used gaps whose depth is cheap (sub/module)
  * full corpus rollup, sorted by breadth then frequency
  * breakdown by category
  * per-target checklist
  * (optionally) a machine-readable gaps.tsv for cross-commit diffing.

Pure standard library.  Categories come from the runtime's best-effort
stamp; depth is a default derived from the category and is meant to be
refined by hand during triage.
"""

import json
import os
import sys
from collections import defaultdict

# category -> default triage depth (a coarse estimate of how far down the
# rabbit hole the gap goes; the developer refines this during triage).
DEPTH_BY_CAT = {
    "CORE_SUB":          "sub",
    "CORE_MOD_SUB":      "module",
    "MOD_CONSTRUCT":     "construct",
    "MISSING_RUNTIME_C": "runtime_c",
    "FEATURE":           "feature",
    "CROSS_MODULE":      "subtree",
    "XS_BINARY":         "binary",
}

# depths for which emulation is the obvious default (a single cheap body).
EMULATABLE_DEPTH = {"sub", "module"}


def parse_call_site(cs):
    """'file:line' -> (file, line).  Splits on the last ':'."""
    if not cs:
        return "-", "-"
    idx = cs.rfind(":")
    if idx < 0:
        return cs, "-"
    return cs[:idx], cs[idx + 1:]


def read_sidecar(path):
    """Yield gap dicts from a JSONL sidecar file."""
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue
            target = obj.get("target") or os.path.splitext(os.path.basename(path))[0]
            sub = obj.get("sub", "?")
            yield {
                "target": target,
                "sub": sub,
                "qual": obj.get("qual", sub),
                "cat": obj.get("cat", "CORE_MOD_SUB"),
                "call_site": obj.get("call_site", f"{target}:" + str(obj.get("site_line", 0))),
                "hits": int(obj.get("hits", 1) or 1),
            }


def load(sidecars):
    """Load all sidecars into a list of gaps and per-target ordering."""
    gaps = []
    for path in sidecars:
        for g in read_sidecar(path):
            g["depth"] = DEPTH_BY_CAT.get(g["cat"], "module")
            g["site"], g["line"] = parse_call_site(g["call_site"])
            gaps.append(g)
    return gaps


def triage(gap):
    """Stamp a triage hint from depth + how widely the gap is used."""
    if gap["depth"] in ("subtree", "binary", "runtime_c", "feature"):
        if gap["depth"] == "feature":
            return "DEFER (known limitation)"
        return "DELEGATE / BUNDLE (bigger scope)"
    if gap["unique_targets"] >= 2 or gap["hits"] >= 5:
        return "IMPLEMENT NOW"
    return "implement when it gets hot"


def report(sidecars, tsv_path=None):
    gaps = load(sidecars)
    if not gaps:
        print("(no gaps recorded — nothing ran into a runtime gap)")
        return 0

    # unique targets per (sub, cat) — the breadth signal for triage.
    targets_by_gap = defaultdict(set)
    for g in gaps:
        targets_by_gap[(g["sub"], g["cat"])].add(g["target"])
    for g in gaps:
        g["unique_targets"] = len(targets_by_gap[(g["sub"], g["cat"])])

    scripts_gapped = sorted({g["target"] for g in gaps})

    # Canonicalise to one record per (sub, category), summarised across
    # targets: unique_targets (breadth), total hits, and a representative
    # call_site.  This is the single source the report sections iterate, so
    # a gap that fires in several scripts is listed once, not once per script.
    canon = {}
    for g in gaps:
        key = (g["sub"], g["cat"])
        rec = canon.get(key)
        if rec is None:
            rec = {
                "sub": g["sub"], "cat": g["cat"], "depth": g["depth"],
                "hits": g["hits"], "targets": {g["target"]},
                "call_site": g["call_site"],
            }
            canon[key] = rec
        else:
            rec["hits"] += g["hits"]
            rec["targets"].add(g["target"])
    canon = list(canon.values())
    for r in canon:
        r["unique_targets"] = len(r["targets"])
    distinct = {(r["sub"], r["cat"]) for r in canon}

    out = []
    out.append("perlc runtime-gap inventory — corpus rollup")
    out.append("=" * 60)
    out.append(f"targets scanned (with gaps) : {len(scripts_gapped)}")
    out.append(f"distinct gaps (sub+category): {len(distinct)}")
    out.append(f"total gap hits recorded     : {sum(r['hits'] for r in canon)}")
    out.append("")

    # ---- implement-now quadrant ----------------------------------------
    impl = [r for r in canon if r["depth"] in EMULATABLE_DEPTH and
            (r["unique_targets"] >= 2 or r["hits"] >= 5)]
    impl = sorted(impl, key=lambda r: (-r["unique_targets"], -r["hits"]))
    out.append("Implement now (cheap depth, broadly used):")
    if not impl:
        out.append("  (none — no shared cheap gap yet; look at the rollup below)")
    else:
        out.append(f"  {'TGT':>4}  {'HITS':>5}  {'DEPTH':>9}  {'CATEGORY':<16}  SUB")
        for r in impl:
            out.append(f"  {r['unique_targets']:>4}  {r['hits']:>5}  "
                       f"{r['depth']:>9}  {r['cat']:<16}  {r['sub']}")
    out.append("")

    # ---- by category ---------------------------------------------------
    by_cat = defaultdict(int)
    for r in canon:
        by_cat[r["cat"]] += 1
    out.append("By category:")
    for cat, n in sorted(by_cat.items(), key=lambda kv: -kv[1]):
        out.append(f"  {cat:<18} {n}")
    out.append("")

    # ---- full corpus rollup --------------------------------------------
    rollup = sorted(canon, key=lambda r: (-r["unique_targets"], -r["hits"], r["sub"]))
    out.append("Corpus rollup (distinct gaps, by breadth then frequency):")
    out.append(f"  {'TGT':>4}  {'HITS':>5}  {'DEPTH':>9}  {'CATEGORY':<16}  TRIAGE  SUB")
    for r in rollup:
        out.append(f"  {r['unique_targets']:>4}  {r['hits']:>5}  "
                   f"{r['depth']:>9}  {r['cat']:<16}  {triage(r):<24}  {r['sub']}")
    out.append("")

    # ---- per-target checklist ------------------------------------------
    out.append("Per-target checklists:")
    by_target = defaultdict(list)
    for g in gaps:
        by_target[g["target"]].append(g)
    for target in scripts_gapped:
        tg = sorted(by_target[target], key=lambda g: (-g["hits"], g["sub"]))
        out.append(f"\n  {target}  ({len({(g['sub'], g['cat']) for g in tg})} gaps)")
        for g in tg:
            out.append(f"    [{g['cat']}] {g['sub']}")
            out.append(f"        call at {g['call_site']}  "
                       f"hits={g['hits']}  depth={g['depth']}  -> {triage(g)}")

    # ---- machine-readable TSV ------------------------------------------
    if tsv_path:
        cols = ["target", "sub", "qual", "cat", "depth", "call_site",
                "hits", "unique_targets", "triage"]
        with open(tsv_path, "w", encoding="utf-8") as f:
            f.write("\t".join(cols) + "\n")
            seen = set()
            for g in gaps:
                key = (g["sub"], g["cat"])
                if key in seen:
                    continue
                seen.add(key)
                line = "\t".join(str(c if c is not None else "") for c in [
                    g["target"], g["sub"], g["qual"], g["cat"], g["depth"],
                    g["call_site"], g["hits"], g["unique_targets"],
                    triage(g)])
                f.write(line + "\n")
        print(f"\n(tsv written to {tsv_path})")

    print("\n".join(out))
    return 0


def main(argv):
    sidecars = [a for a in argv[1:] if not a.startswith("--tsv")]
    tsv_path = None
    for i, a in enumerate(argv):
        if a == "--tsv" and i + 1 < len(argv):
            tsv_path = argv[i + 1]
    if not sidecars:
        print(__doc__)
        return 2
    return report(sidecars, tsv_path)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
