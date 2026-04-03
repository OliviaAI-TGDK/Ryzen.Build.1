"""
xem_zcold_policy.py

ZRAM ColdMet over a .XEM crosshatched matrix using OuijiHex144_7x9.

Purpose:
- Convert page / tensor / shard identifiers into deterministic cold-tier policy
- Emit 9-mip retained ratios
- Produce a JSON manifest that dsvramd can ingest
- Keep "ZRAM" as the policy label, not the kernel module name

Usage:
    python xem_zcold_policy.py --text "DAWNSTAR:page-0001"
    python xem_zcold_policy.py --file tags.txt
"""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass, asdict
from typing import Any, Dict, List, Tuple

from ouijihex144_7x9 import OuijiHex144_7x9


def clamp(value: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, value))


def avg(values: List[float]) -> float:
    return sum(values) / len(values) if values else 0.0


def flatten(xs: List[Any]) -> List[Any]:
    out: List[Any] = []
    for x in xs:
        if isinstance(x, list):
            out.extend(flatten(x))
        else:
            out.append(x)
    return out


@dataclass
class XemColdDecision:
    key: str
    final_signature: str
    final_base: int
    vertical_mean: float
    horizontal_mean: float
    lateral_mean: float
    parallel_mean: float
    offbranch_mean: float
    crosshatch_score: float
    coldness_score: float
    retained_ratio: float
    reduction_ratio: float
    compression_factor: float
    mip_retained: List[float]
    tier: str
    matrix: str = "xem-crosshatched"
    coldmet: str = "ZRAM"


class XemZramColdMet:
    def __init__(
        self,
        min_retained_ratio: float = 0.0102,
        max_retained_ratio: float = 0.12,
        passes: int = 144,
        rows: int = 7,
        cols: int = 9,
        branch_count: int = 3,
        seed: int = 0x0F210144,
    ) -> None:
        self.min_retained_ratio = min_retained_ratio
        self.max_retained_ratio = max_retained_ratio
        self.engine = OuijiHex144_7x9(
            passes=passes,
            rows=rows,
            cols=cols,
            branch_count=branch_count,
            seed=seed,
        )

    def classify(self, key: str) -> XemColdDecision:
        report = self.engine.encode(key)
        item = report["summary"][0] if len(report["summary"]) == 1 else self._fold_summary(report)

        vertical = item["final_vertical"]
        horizontal = item["final_horizontal"]

        # Pull more detail from the final pass
        last_pass = report["pass_log"][-1]["result"][0] if len(report["pass_log"][-1]["result"]) == 1 else report["pass_log"][-1]["result"][0]
        lateral = last_pass["dimensions"]["lateral"]
        parallel = last_pass["dimensions"]["parallel"]
        offbranch = last_pass["dimensions"]["offbranch"]

        lateral_values = (
            flatten(list(lateral.values())) if isinstance(lateral, dict) else flatten(lateral)
        )
        parallel_values = flatten(parallel)
        offbranch_values = flatten(offbranch)

        vertical_mean = avg(vertical)
        horizontal_mean = avg(horizontal)
        lateral_mean = avg([float(v) for v in lateral_values])
        parallel_mean = avg([float(v) for v in parallel_values])
        offbranch_mean = avg([float(v) for v in offbranch_values])

        crosshatch_score = self._crosshatch_score(
            vertical_mean,
            horizontal_mean,
            lateral_mean,
            parallel_mean,
            offbranch_mean,
        )

        coldness_score = self._coldness_score(
            final_base=item["final_base"],
            crosshatch_score=crosshatch_score,
            offbranch_count=item["offbranch_count"],
            vertical_mean=vertical_mean,
            horizontal_mean=horizontal_mean,
        )

        retained_ratio = self._retained_ratio_from_coldness(coldness_score)
        reduction_ratio = 1.0 - retained_ratio
        compression_factor = 1.0 / retained_ratio
        mip_retained = self._build_9_mip_profile(retained_ratio)

        return XemColdDecision(
            key=key,
            final_signature=item["final_signature"],
            final_base=item["final_base"],
            vertical_mean=round(vertical_mean, 6),
            horizontal_mean=round(horizontal_mean, 6),
            lateral_mean=round(lateral_mean, 6),
            parallel_mean=round(parallel_mean, 6),
            offbranch_mean=round(offbranch_mean, 6),
            crosshatch_score=round(crosshatch_score, 6),
            coldness_score=round(coldness_score, 6),
            retained_ratio=round(retained_ratio, 6),
            reduction_ratio=round(reduction_ratio, 6),
            compression_factor=round(compression_factor, 4),
            mip_retained=[round(v, 6) for v in mip_retained],
            tier=self._tier_name(coldness_score),
        )

    def classify_many(self, keys: List[str]) -> Dict[str, Any]:
        decisions = [self.classify(k) for k in keys]

        return {
            "coldmet": "ZRAM",
            "matrix": ".XEM crosshatched",
            "schema": "xem-zcold/v1",
            "count": len(decisions),
            "summary": {
                "average_coldness": round(avg([d.coldness_score for d in decisions]), 6),
                "average_retained_ratio": round(avg([d.retained_ratio for d in decisions]), 6),
                "average_reduction_ratio": round(avg([d.reduction_ratio for d in decisions]), 6),
                "average_crosshatch_score": round(avg([d.crosshatch_score for d in decisions]), 6),
            },
            "pages": [asdict(d) for d in decisions],
        }

    def _fold_summary(self, report: Dict[str, Any]) -> Dict[str, Any]:
        # deterministic fold if key expands to multiple characters
        summary = report["summary"]
        return {
            "character": "*",
            "index": 0,
            "final_base": sum(item["final_base"] for item in summary) & 0xFFFFFFFF,
            "final_signature": "|".join(item["final_signature"] for item in summary),
            "final_vertical": [
                round(avg([item["final_vertical"][i] for item in summary]), 6)
                for i in range(len(summary[0]["final_vertical"]))
            ],
            "final_horizontal": [
                round(avg([item["final_horizontal"][i] for item in summary]), 6)
                for i in range(len(summary[0]["final_horizontal"]))
            ],
            "offbranch_count": avg([item["offbranch_count"] for item in summary]),
        }

    def _crosshatch_score(
        self,
        vertical_mean: float,
        horizontal_mean: float,
        lateral_mean: float,
        parallel_mean: float,
        offbranch_mean: float,
    ) -> float:
        # .XEM crosshatch: reward balanced vertical/horizontal
        vh_balance = 1.0 - abs(vertical_mean - horizontal_mean)
        diag_mix = (lateral_mean * 0.55) + (parallel_mean * 0.45)
        branch_mix = offbranch_mean

        score = (
            vh_balance * 0.32 +
            vertical_mean * 0.14 +
            horizontal_mean * 0.14 +
            diag_mix * 0.22 +
            branch_mix * 0.18
        )
        return clamp(score, 0.0, 1.0)

    def _coldness_score(
        self,
        final_base: int,
        crosshatch_score: float,
        offbranch_count: float,
        vertical_mean: float,
        horizontal_mean: float,
    ) -> float:
        base_mod = ((final_base % 1440) / 1440.0)
        symmetry = 1.0 - abs(vertical_mean - horizontal_mean)

        score = (
            crosshatch_score * 0.42 +
            symmetry * 0.20 +
            base_mod * 0.18 +
            clamp(offbranch_count / 3.0, 0.0, 1.0) * 0.20
        )

        return clamp(score, 0.0, 1.0)

    def _retained_ratio_from_coldness(self, coldness_score: float) -> float:
        # colder => more compressible => lower retained ratio
        retained = self.max_retained_ratio - (
            (self.max_retained_ratio - self.min_retained_ratio) * coldness_score
        )
        return clamp(retained, self.min_retained_ratio, self.max_retained_ratio)

    def _build_9_mip_profile(self, base_retained: float) -> List[float]:
        weights = [2.40, 1.85, 1.40, 1.05, 0.80, 0.60, 0.45, 0.30, 0.20]
        return [clamp(base_retained * w, 0.001, 1.0) for w in weights]

    def _tier_name(self, coldness_score: float) -> str:
        if coldness_score >= 0.86:
            return "deep-cold"
        if coldness_score >= 0.68:
            return "cold"
        if coldness_score >= 0.45:
            return "warm"
        return "hot"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--text", type=str, help="Single key/tag to classify")
    parser.add_argument("--file", type=str, help="File with one key/tag per line")
    parser.add_argument("--min-retained", type=float, default=0.0102)
    parser.add_argument("--max-retained", type=float, default=0.12)
    parser.add_argument("--pretty", action="store_true")
    args = parser.parse_args()

    engine = XemZramColdMet(
        min_retained_ratio=args.min_retained,
        max_retained_ratio=args.max_retained,
    )

    keys: List[str] = []
    if args.text:
        keys.append(args.text)

    if args.file:
        with open(args.file, "r", encoding="utf-8") as f:
            keys.extend([line.strip() for line in f if line.strip()])

    if not keys:
        keys = [
            "DAWNSTAR:page-0001",
            "DAWNSTAR:page-0002",
            "DAWNSTAR:page-0003",
        ]

    result = engine.classify_many(keys)
    print(json.dumps(result, indent=2 if args.pretty else None))


if __name__ == "__main__":
    main()
