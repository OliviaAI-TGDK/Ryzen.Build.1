"""
xem_fib_recursion.py

Replicate the XEM / ZRAM ColdMet structure over a Fibonacci recursion.

Depends on:
- ouijihex144_7x9.py
- xem_zcold_policy.py

Model:
- base XEM ColdMet decision for a key
- N recursive levels
- Fibonacci weighting per level
- replicated node structure per level
- inherited 9-mip retained profile
- recursive signatures / aggregate ratios
"""

from __future__ import annotations

import json
from dataclasses import dataclass, asdict
from typing import Any, Dict, List, Optional

from xem_zcold_policy import XemZramColdMet


def clamp(value: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, value))


def avg(values: List[float]) -> float:
    return sum(values) / len(values) if values else 0.0


def fib_sequence(count: int) -> List[int]:
    if count <= 0:
        return []
    if count == 1:
        return [1]
    seq = [1, 1]
    while len(seq) < count:
        seq.append(seq[-1] + seq[-2])
    return seq[:count]


@dataclass
class FibRecursiveNode:
    level: int
    node_index: int
    fib_value: int
    fib_weight: float
    lane: int
    slot: int
    phase: float
    curvature: float
    inherited_retained_ratio: float
    retained_ratio: float
    reduction_ratio: float
    crosshatch_score: float
    coldness_score: float
    mip_retained: List[float]
    signature: str


@dataclass
class FibRecursiveLevel:
    level: int
    fib_value: int
    fib_weight: float
    node_count: int
    level_retained_ratio: float
    level_reduction_ratio: float
    level_crosshatch_score: float
    level_coldness_score: float
    level_mip_retained: List[float]
    nodes: List[FibRecursiveNode]


class XemFibRecursor:
    def __init__(
        self,
        *,
        recursion_levels: int = 22,
        nodes_per_level: int = 12,
        min_retained_ratio: float = 0.0102,
        max_retained_ratio: float = 0.12,
        level_gain: float = 0.144,
        node_gain: float = 0.021,
        phase_gain: float = 0.0102,
        curvature_gain: float = 0.03125,
        seed: int = 0x0F210144,
    ) -> None:
        self.recursion_levels = recursion_levels
        self.nodes_per_level = nodes_per_level
        self.min_retained_ratio = min_retained_ratio
        self.max_retained_ratio = max_retained_ratio
        self.level_gain = level_gain
        self.node_gain = node_gain
        self.phase_gain = phase_gain
        self.curvature_gain = curvature_gain
        self.seed = seed & 0xFFFFFFFF

        self.policy = XemZramColdMet(
            min_retained_ratio=min_retained_ratio,
            max_retained_ratio=max_retained_ratio,
            seed=seed,
        )

    def recurse(self, key: str) -> Dict[str, Any]:
        base = self.policy.classify(key)
        fib = fib_sequence(self.recursion_levels)
        fib_total = sum(fib) or 1

        levels: List[FibRecursiveLevel] = []
        all_retained: List[float] = []
        all_reduction: List[float] = []
        all_crosshatch: List[float] = []
        all_coldness: List[float] = []

        for level_index, fib_value in enumerate(fib, start=1):
            fib_weight = fib_value / fib_total
            level_nodes = self._build_level_nodes(base, level_index, fib_value, fib_weight)

            level_retained = avg([n.retained_ratio for n in level_nodes])
            level_reduction = avg([n.reduction_ratio for n in level_nodes])
            level_crosshatch = avg([n.crosshatch_score for n in level_nodes])
            level_coldness = avg([n.coldness_score for n in level_nodes])
            level_mips = self._build_9_mip_profile(level_retained)

            levels.append(
                FibRecursiveLevel(
                    level=level_index,
                    fib_value=fib_value,
                    fib_weight=round(fib_weight, 8),
                    node_count=len(level_nodes),
                    level_retained_ratio=round(level_retained, 6),
                    level_reduction_ratio=round(level_reduction, 6),
                    level_crosshatch_score=round(level_crosshatch, 6),
                    level_coldness_score=round(level_coldness, 6),
                    level_mip_retained=[round(v, 6) for v in level_mips],
                    nodes=level_nodes,
                )
            )

            all_retained.append(level_retained)
            all_reduction.append(level_reduction)
            all_crosshatch.append(level_crosshatch)
            all_coldness.append(level_coldness)

        return {
            "module": "xem-fib-recursion",
            "schema": "xem-fib-recursion/v1",
            "source_key": key,
            "base": {
                "final_signature": base.final_signature,
                "final_base": base.final_base,
                "matrix": base.matrix,
                "coldmet": base.coldmet,
                "tier": base.tier,
                "crosshatch_score": base.crosshatch_score,
                "coldness_score": base.coldness_score,
                "retained_ratio": base.retained_ratio,
                "reduction_ratio": base.reduction_ratio,
                "compression_factor": base.compression_factor,
                "mip_retained": base.mip_retained,
            },
            "recursion": {
                "levels": self.recursion_levels,
                "nodes_per_level": self.nodes_per_level,
                "total_nodes": self.recursion_levels * self.nodes_per_level,
                "fibonacci": fib,
            },
            "summary": {
                "average_retained_ratio": round(avg(all_retained), 6),
                "average_reduction_ratio": round(avg(all_reduction), 6),
                "average_crosshatch_score": round(avg(all_crosshatch), 6),
                "average_coldness_score": round(avg(all_coldness), 6),
                "recursive_compression_factor": round(
                    1.0 / max(avg(all_retained), 0.000001), 4
                ),
            },
            "levels": [self._level_to_dict(level) for level in levels],
        }

    def _build_level_nodes(
        self,
        base: Any,
        level_index: int,
        fib_value: int,
        fib_weight: float,
    ) -> List[FibRecursiveNode]:
        nodes: List[FibRecursiveNode] = []

        for node_index in range(self.nodes_per_level):
            lane = node_index % 3
            slot = node_index % 4

            phase = (
                (level_index * self.phase_gain)
                + (node_index * self.phase_gain * 0.5)
                + (fib_weight * 0.144)
            )

            curvature = (
                level_index * self.curvature_gain
                * (1.0 + slot * 0.25)
                * (1.0 + lane * 0.125)
            )

            # Fibonacci weighting shifts retention downward as recursion deepens,
            # while keeping it inside the configured band.
            level_scalar = 1.0 - (fib_weight * self.level_gain)
            node_scalar = 1.0 - ((node_index / max(1, self.nodes_per_level - 1)) * self.node_gain)
            phase_scalar = 1.0 - (phase * 0.01)

            retained = (
                base.retained_ratio
                * level_scalar
                * node_scalar
                * phase_scalar
            )
            retained = clamp(retained, self.min_retained_ratio, self.max_retained_ratio)

            crosshatch = clamp(
                base.crosshatch_score
                * (1.0 + fib_weight * 0.12)
                * (1.0 - slot * 0.015),
                0.0,
                1.0,
            )

            coldness = clamp(
                base.coldness_score
                * (1.0 + fib_weight * 0.18)
                * (1.0 + lane * 0.01),
                0.0,
                1.0,
            )

            mip_retained = self._build_9_mip_profile(retained)

            signature = (
                f"{base.final_signature}"
                f":fib{level_index}"
                f":f{fib_value}"
                f":n{node_index}"
                f":l{lane}"
                f":s{slot}"
            )

            nodes.append(
                FibRecursiveNode(
                    level=level_index,
                    node_index=node_index,
                    fib_value=fib_value,
                    fib_weight=round(fib_weight, 8),
                    lane=lane,
                    slot=slot,
                    phase=round(phase, 6),
                    curvature=round(curvature, 6),
                    inherited_retained_ratio=round(base.retained_ratio, 6),
                    retained_ratio=round(retained, 6),
                    reduction_ratio=round(1.0 - retained, 6),
                    crosshatch_score=round(crosshatch, 6),
                    coldness_score=round(coldness, 6),
                    mip_retained=[round(v, 6) for v in mip_retained],
                    signature=signature,
                )
            )

        return nodes

    def _build_9_mip_profile(self, base_retained: float) -> List[float]:
        weights = [2.40, 1.85, 1.40, 1.05, 0.80, 0.60, 0.45, 0.30, 0.20]
        return [clamp(base_retained * w, 0.001, 1.0) for w in weights]

    @staticmethod
    def _level_to_dict(level: FibRecursiveLevel) -> Dict[str, Any]:
        return {
            "level": level.level,
            "fib_value": level.fib_value,
            "fib_weight": level.fib_weight,
            "node_count": level.node_count,
            "level_retained_ratio": level.level_retained_ratio,
            "level_reduction_ratio": level.level_reduction_ratio,
            "level_crosshatch_score": level.level_crosshatch_score,
            "level_coldness_score": level.level_coldness_score,
            "level_mip_retained": level.level_mip_retained,
            "nodes": [asdict(n) for n in level.nodes],
        }


def print_fib_report(report: Dict[str, Any], max_levels: Optional[int] = 3) -> None:
    print("=== XEM FIBONACCI RECURSION REPORT ===")
    print({
        "module": report["module"],
        "schema": report["schema"],
        "source_key": report["source_key"],
        "levels": report["recursion"]["levels"],
        "nodes_per_level": report["recursion"]["nodes_per_level"],
        "total_nodes": report["recursion"]["total_nodes"],
    })

    print("\n=== BASE ===")
    print(report["base"])

    print("\n=== SUMMARY ===")
    print(report["summary"])

    print("\n=== LEVEL SNAPSHOT ===")
    levels = report["levels"] if max_levels is None else report["levels"][:max_levels]
    for level in levels:
        print({
            "level": level["level"],
            "fib_value": level["fib_value"],
            "fib_weight": level["fib_weight"],
            "level_retained_ratio": level["level_retained_ratio"],
            "level_reduction_ratio": level["level_reduction_ratio"],
            "level_crosshatch_score": level["level_crosshatch_score"],
            "level_coldness_score": level["level_coldness_score"],
            "level_mip_retained": level["level_mip_retained"],
            "first_node_signature": level["nodes"][0]["signature"] if level["nodes"] else None,
        })


if __name__ == "__main__":
    engine = XemFibRecursor(
        recursion_levels=22,
        nodes_per_level=12,
        min_retained_ratio=0.0102,
        max_retained_ratio=0.12,
    )

    report = engine.recurse("DAWNSTAR:page-0001")
    print_fib_report(report, max_levels=3)

    print("\n=== FULL JSON ===")
    print(json.dumps(report, indent=2))
