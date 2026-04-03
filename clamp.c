/**
 * Dawnstar Reverse Canonical Currogation Layer
 *
 * Topology:
 * 1880 raw nodes
 * -> 144 canonical nodes
 * -> 21 dual-dodecahedral cells
 * -> 8x4x3 / 2 * 0.0102 knot tensor
 */

function clamp(value, min, max) {
  return Math.max(min, Math.min(max, value));
}

function round(value, digits = 6) {
  const p = 10 ** digits;
  return Math.round(value * p) / p;
}

function avg(arr) {
  if (!arr.length) return 0;
  return arr.reduce((a, b) => a + b, 0) / arr.length;
}

function variance(arr) {
  if (!arr.length) return 0;
  const mean = avg(arr);
  return avg(arr.map(v => (v - mean) ** 2));
}

class FabricNode1880 {
  constructor(id, config = {}) {
    this.id = id;
    this.value = config.value ?? Math.random();
    this.phase = config.phase ?? (Math.random() * Math.PI * 2);
    this.gain = config.gain ?? 1.0;
    this.health = config.health ?? 1.0;
    this.drift = config.drift ?? 0;
    this.entropy = config.entropy ?? 0.0102;
  }

  sample() {
    return {
      id: this.id,
      value: this.value * this.gain * this.health,
      phase: this.phase,
      gain: this.gain,
      health: this.health,
      drift: this.drift,
      entropy: this.entropy
    };
  }
}

class CanonicalNode144 {
  constructor(id, members = []) {
    this.id = id;
    this.members = members;
  }

  compress() {
    const samples = this.members.map(n => n.sample());
    const values = samples.map(s => s.value);
    const phases = samples.map(s => s.phase);
    const healths = samples.map(s => s.health);
    const gains = samples.map(s => s.gain);

    const meanValue = avg(values);
    const meanPhase = avg(phases);
    const meanHealth = avg(healths);
    const meanGain = avg(gains);
    const residual = variance(values);

    return {
      id: this.id,
      memberCount: this.members.length,
      meanValue,
      meanPhase,
      meanHealth,
      meanGain,
      residual,
      canonicalWeight: clamp(
        meanValue * 0.45 +
        meanHealth * 0.25 +
        meanGain * 0.15 +
        (1 - clamp(residual, 0, 1)) * 0.15,
        0,
        1.5
      )
    };
  }
}

class DualDodecahedronCell {
  constructor(id, canonicalNodes = []) {
    this.id = id;
    this.canonicalNodes = canonicalNodes;
  }

  resolve() {
    const compressed = this.canonicalNodes.map(n => n.compress());

    const values = compressed.map(c => c.meanValue);
    const phases = compressed.map(c => c.meanPhase);
    const healths = compressed.map(c => c.meanHealth);
    const residuals = compressed.map(c => c.residual);
    const weights = compressed.map(c => c.canonicalWeight);

    const energy = avg(values) * avg(weights);
    const agreement = 1 - clamp(variance(values), 0, 1);
    const health = avg(healths);
    const torsion = avg(phases) / (Math.PI * 2);
    const residual = avg(residuals);

    // Inner/outer shell split for "dual dodecahedron" behavior
    const innerShell = clamp(energy * agreement * 0.55, 0, 1.5);
    const outerShell = clamp((energy + health + torsion) / 3 * 0.45, 0, 1.5);

    return {
      id: this.id,
      canonicalCount: this.canonicalNodes.length,
      energy,
      agreement,
      health,
      torsion,
      residual,
      innerShell,
      outerShell,
      cellWeight: clamp(
        innerShell * 0.34 +
        outerShell * 0.24 +
        health * 0.18 +
        agreement * 0.16 +
        (1 - residual) * 0.08,
        0,
        1.5
      )
    };
  }
}

class DawnstarCurrogationEngine {
  constructor(config = {}) {
    this.sourceNodeCount = 1880;
    this.canonicalNodeCount = 144;
    this.cellCount = 21;
    this.knotShape = [8, 4, 3];
    this.knotDivisor = 2;
    this.knotScalar = 0.0102;
    this.finalKnotMultiplier = (
      this.knotShape[0] *
      this.knotShape[1] *
      this.knotShape[2] /
      this.knotDivisor
    ) * this.knotScalar; // 0.4896

    this.fabric = this.#buildFabric();
    this.canonicalNodes = [];
    this.cells = [];
  }

  #buildFabric() {
    return Array.from({ length: this.sourceNodeCount }, (_, i) => {
      return new FabricNode1880(i + 1, {
        value: 0.6 + Math.random() * 0.4,
        phase: Math.random() * Math.PI * 2,
        gain: 0.9 + Math.random() * 0.2,
        health: 0.88 + Math.random() * 0.12,
        drift: (Math.random() - 0.5) * 0.05,
        entropy: 0.0102
      });
    });
  }

  reverseCanonicalCurrogate() {
    const groups = [];
    let cursor = 0;

    for (let i = 0; i < this.canonicalNodeCount; i++) {
      const bucketSize = i < 8 ? 14 : 13; // 1880 -> 144 exact split
      const members = this.fabric.slice(cursor, cursor + bucketSize);
      cursor += bucketSize;
      groups.push(new CanonicalNode144(i + 1, members));
    }

    this.canonicalNodes = groups;
    return groups;
  }

  buildDualDodecahedrons() {
    if (!this.canonicalNodes.length) {
      this.reverseCanonicalCurrogate();
    }

    const cells = [];
    let cursor = 0;

    for (let i = 0; i < this.cellCount; i++) {
      const bucketSize = i < 18 ? 7 : 6; // 144 -> 21 exact split
      const nodes = this.canonicalNodes.slice(cursor, cursor + bucketSize);
      cursor += bucketSize;
      cells.push(new DualDodecahedronCell(i + 1, nodes));
    }

    this.cells = cells;
    return cells;
  }

  emitKnotTensor() {
    if (!this.cells.length) {
      this.buildDualDodecahedrons();
    }

    const resolvedCells = this.cells.map(c => c.resolve());

    const totalCellWeight = resolvedCells.reduce((sum, c) => sum + c.cellWeight, 0);
    const avgAgreement = avg(resolvedCells.map(c => c.agreement));
    const avgHealth = avg(resolvedCells.map(c => c.health));
    const avgResidual = avg(resolvedCells.map(c => c.residual));

    const knotBase =
      this.finalKnotMultiplier *
      clamp(totalCellWeight / this.cellCount, 0, 1.5) *
      clamp(avgAgreement, 0, 1) *
      clamp(avgHealth, 0, 1);

    const tensor = [];
    let cellIndex = 0;

    for (let x = 0; x < this.knotShape[0]; x++) {
      const plane = [];
      for (let y = 0; y < this.knotShape[1]; y++) {
        const row = [];
        for (let z = 0; z < this.knotShape[2]; z++) {
          const cell = resolvedCells[cellIndex % resolvedCells.length];
          const shellBlend = (cell.innerShell * 0.56) + (cell.outerShell * 0.44);

          const value = knotBase * shellBlend * (1 - clamp(cell.residual, 0, 0.95));
          row.push(round(value, 6));
          cellIndex++;
        }
        plane.push(row);
      }
      tensor.push(plane);
    }

    return {
      sourceNodeCount: this.sourceNodeCount,
      canonicalNodeCount: this.canonicalNodeCount,
      cellCount: this.cellCount,
      finalKnotMultiplier: round(this.finalKnotMultiplier, 6), // 0.4896
      averageAgreement: round(avgAgreement, 6),
      averageHealth: round(avgHealth, 6),
      averageResidual: round(avgResidual, 6),
      knotBase: round(knotBase, 6),
      tensor
    };
  }

  fullReport() {
    const canonical = this.reverseCanonicalCurrogate().map(n => n.compress());
    const cells = this.buildDualDodecahedrons().map(c => c.resolve());
    const knot = this.emitKnotTensor();

    return {
      topology: {
        source: 1880,
        canonical: 144,
        dodecahedralCells: 21,
        knotShape: "8x4x3",
        knotDivisor: 2,
        knotScalar: 0.0102,
        finalKnotMultiplier: 0.4896
      },
      canonicalSummary: {
        first8MemberCount: canonical.slice(0, 8).map(n => n.memberCount),
        restMemberCountSample: canonical.slice(8, 16).map(n => n.memberCount),
        meanCanonicalWeight: round(avg(canonical.map(n => n.canonicalWeight)), 6),
        meanResidual: round(avg(canonical.map(n => n.residual)), 6)
      },
      cellSummary: {
        first18CanonicalCount: cells.slice(0, 18).map(c => c.canonicalCount),
        last3CanonicalCount: cells.slice(18).map(c => c.canonicalCount),
        meanCellWeight: round(avg(cells.map(c => c.cellWeight)), 6),
        meanAgreement: round(avg(cells.map(c => c.agreement)), 6)
      },
      knot
    };
  }
}

/* ---------------------------------
 * Example usage
 * --------------------------------- */

const currogation = new DawnstarCurrogationEngine();

const report = currogation.fullReport();

console.log("=== REVERSE CANONICAL CURROGATION REPORT ===");
console.log(JSON.stringify(report, null, 2));
