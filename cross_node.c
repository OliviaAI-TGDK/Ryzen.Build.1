struct DuoCrossNode {
    uint32_t levelIndex = 0;
    uint32_t nodeIndex = 0;
    uint32_t laneIndex = 0;
    uint32_t slotIndex = 0; // 0..3 inside the | - | - motif

    std::string axis;       // vertical | horizontal
    std::string role;       // base-a | base-b | refract-ab | refract-ba

    SimplexDirection a = SimplexDirection::INOUT;
    SimplexDirection b = SimplexDirection::INOUT;
    SimplexDirection resolved = SimplexDirection::INOUT;

    double quotient = 1.0;
    double refraction = 0.0102;
    double gain = 1.0;
    double curvature = 0.0;
    double weight = 1.0;
};

struct DuoCrossLevel {
    uint32_t levelIndex = 0;
    std::array<DuoCrossNode, 12> nodes{};
    double levelEnergy = 0.0;
    double quotientMean = 1.0;
    double refractionMean = 0.0102;
    double gainMean = 1.0;
};

struct DuoQuadratalizerConfig {
    uint32_t levelCount = 22;
    double baseRefraction = 0.0102;
    double forkGain = 1.0;
    double refractGain = 1.08;
    double curvatureStep = 0.03125;
    double levelGainStep = 0.0125;
    double knotScalar = 0.4896;
};

class DuoQuadratalizer22 {
public:
    explicit DuoQuadratalizer22(DuoQuadratalizerConfig cfg = {}) : cfg_(cfg) {
        if (cfg_.levelCount == 0) cfg_.levelCount = 22;
    }

    std::vector<DuoCrossLevel> build() const {
        std::vector<DuoCrossLevel> levels;
        levels.reserve(cfg_.levelCount);

        for (uint32_t level = 0; level < cfg_.levelCount; ++level) {
            DuoCrossLevel out;
            out.levelIndex = level;

            const double levelT = static_cast<double>(level) /
                                  static_cast<double>(std::max(1u, cfg_.levelCount - 1));

            double energyAccum = 0.0;
            double quotientAccum = 0.0;
            double refractionAccum = 0.0;
            double gainAccum = 0.0;

            uint32_t nodeCursor = 0;

            for (uint32_t lane = 0; lane < 3; ++lane) {
                const SimplexDirection a = kA[lane];
                const SimplexDirection b = kB[lane];

                // 4-slot motif:
                // |
                // -
                // |
                // -
                //
                // slot 0 = vertical base-a
                // slot 1 = horizontal base-b
                // slot 2 = vertical refract-ab
                // slot 3 = horizontal refract-ba

                out.nodes[nodeCursor] = makeNode(level, nodeCursor, lane, 0, "vertical",   "base-a",    a, b, levelT);
                ++nodeCursor;
                out.nodes[nodeCursor] = makeNode(level, nodeCursor, lane, 1, "horizontal", "base-b",    b, a, levelT);
                ++nodeCursor;
                out.nodes[nodeCursor] = makeNode(level, nodeCursor, lane, 2, "vertical",   "refract-ab", a, b, levelT);
                ++nodeCursor;
                out.nodes[nodeCursor] = makeNode(level, nodeCursor, lane, 3, "horizontal", "refract-ba", b, a, levelT);
                ++nodeCursor;
            }

            for (const auto& n : out.nodes) {
                energyAccum += n.weight * n.gain * cfg_.knotScalar;
                quotientAccum += n.quotient;
                refractionAccum += n.refraction;
                gainAccum += n.gain;
            }

            out.levelEnergy = round_to(energyAccum / 12.0, 6);
            out.quotientMean = round_to(quotientAccum / 12.0, 6);
            out.refractionMean = round_to(refractionAccum / 12.0, 6);
            out.gainMean = round_to(gainAccum / 12.0, 6);

            levels.push_back(out);
        }

        return levels;
    }

    std::string renderJSON(const std::vector<DuoCrossLevel>& levels) const {
        std::ostringstream os;
        os << std::fixed << std::setprecision(6);

        os << "{\n";
        os << "  \"duo_quadratalizer\": {\n";
        os << "    \"levels\": " << cfg_.levelCount << ",\n";
        os << "    \"nodes_per_level\": 12,\n";
        os << "    \"total_nodes\": " << (cfg_.levelCount * 12) << ",\n";
        os << "    \"a\": [\"in/out\", \"in/out\", \"out\"],\n";
        os << "    \"b\": [\"in/out\", \"in/out\", \"in\"],\n";
        os << "    \"cross_motif\": \"| - | -\",\n";
        os << "    \"base_refraction\": " << cfg_.baseRefraction << ",\n";
        os << "    \"fork_gain\": " << cfg_.forkGain << ",\n";
        os << "    \"refract_gain\": " << cfg_.refractGain << ",\n";
        os << "    \"curvature_step\": " << cfg_.curvatureStep << ",\n";
        os << "    \"level_gain_step\": " << cfg_.levelGainStep << ",\n";
        os << "    \"knot_scalar\": " << cfg_.knotScalar << "\n";
        os << "  },\n";
        os << "  \"levels\": [\n";

        for (std::size_t i = 0; i < levels.size(); ++i) {
            const auto& level = levels[i];

            os << "    {\n";
            os << "      \"level_index\": " << level.levelIndex << ",\n";
            os << "      \"level_energy\": " << level.levelEnergy << ",\n";
            os << "      \"quotient_mean\": " << level.quotientMean << ",\n";
            os << "      \"refraction_mean\": " << level.refractionMean << ",\n";
            os << "      \"gain_mean\": " << level.gainMean << ",\n";
            os << "      \"nodes\": [\n";

            for (std::size_t n = 0; n < level.nodes.size(); ++n) {
                const auto& node = level.nodes[n];
                os << "        {\n";
                os << "          \"level_index\": " << node.levelIndex << ",\n";
                os << "          \"node_index\": " << node.nodeIndex << ",\n";
                os << "          \"lane_index\": " << node.laneIndex << ",\n";
                os << "          \"slot_index\": " << node.slotIndex << ",\n";
                os << "          \"axis\": \"" << node.axis << "\",\n";
                os << "          \"role\": \"" << node.role << "\",\n";
                os << "          \"a\": \"" << simplexDirName(node.a) << "\",\n";
                os << "          \"b\": \"" << simplexDirName(node.b) << "\",\n";
                os << "          \"resolved\": \"" << simplexDirName(node.resolved) << "\",\n";
                os << "          \"quotient\": " << node.quotient << ",\n";
                os << "          \"refraction\": " << node.refraction << ",\n";
                os << "          \"gain\": " << node.gain << ",\n";
                os << "          \"curvature\": " << node.curvature << ",\n";
                os << "          \"weight\": " << node.weight << "\n";
                os << "        }" << (n + 1 < level.nodes.size() ? "," : "") << "\n";
            }

            os << "      ]\n";
            os << "    }" << (i + 1 < levels.size() ? "," : "") << "\n";
        }

        os << "  ]\n";
        os << "}\n";
        return os.str();
    }

private:
    static constexpr std::array<SimplexDirection, 3> kA = {
        SimplexDirection::INOUT,
        SimplexDirection::INOUT,
        SimplexDirection::OUT
    };

    static constexpr std::array<SimplexDirection, 3> kB = {
        SimplexDirection::INOUT,
        SimplexDirection::INOUT,
        SimplexDirection::IN
    };

    DuoQuadratalizerConfig cfg_{};

    static int dirValue(SimplexDirection d) {
        switch (d) {
            case SimplexDirection::IN: return -1;
            case SimplexDirection::OUT: return 1;
            case SimplexDirection::INOUT: return 0;
            default: return 0;
        }
    }

    static SimplexDirection resolvePair(SimplexDirection a, SimplexDirection b, bool refracted) {
        if (!refracted) return a;

        if (a == b) return a;
        if (a == SimplexDirection::INOUT || b == SimplexDirection::INOUT) {
            return SimplexDirection::INOUT;
        }

        // IN crossed with OUT resolves to INOUT
        return SimplexDirection::INOUT;
    }

    DuoCrossNode makeNode(uint32_t level,
                          uint32_t nodeIndex,
                          uint32_t lane,
                          uint32_t slot,
                          const std::string& axis,
                          const std::string& role,
                          SimplexDirection a,
                          SimplexDirection b,
                          double levelT) const {
        const bool refracted =
            (role == "refract-ab" || role == "refract-ba");

        const int av = dirValue(a);
        const int bv = dirValue(b);

        // Stable quotient from direction values:
        // IN=-1 => 1
        // INOUT=0 => 2
        // OUT=1 => 3
        const double qa = static_cast<double>(av + 2);
        const double qb = static_cast<double>(bv + 2);
        const double quotient = qa / qb;

        const double refraction =
            cfg_.baseRefraction *
            (1.0 + (static_cast<double>(lane) * 0.25)) *
            (1.0 + (levelT * 0.75)) *
            (refracted ? cfg_.refractGain : 1.0);

        const double curvature =
            cfg_.curvatureStep *
            static_cast<double>(level + 1) *
            static_cast<double>(slot + 1);

        const double gain =
            cfg_.forkGain *
            (1.0 + levelT * cfg_.levelGainStep * 22.0) *
            (refracted ? cfg_.refractGain : 1.0);

        const double weight =
            clamp(
                (1.0 / quotient) * 0.28 +
                refraction * 8.0 * 0.18 +
                gain * 0.24 +
                (refracted ? 0.18 : 0.12),
                0.001, 4.0
            );

        DuoCrossNode out;
        out.levelIndex = level;
        out.nodeIndex = nodeIndex;
        out.laneIndex = lane;
        out.slotIndex = slot;
        out.axis = axis;
        out.role = role;
        out.a = a;
        out.b = b;
        out.resolved = resolvePair(a, b, refracted);
        out.quotient = round_to(quotient, 6);
        out.refraction = round_to(refraction, 6);
        out.gain = round_to(gain, 6);
        out.curvature = round_to(curvature, 6);
        out.weight = round_to(weight, 6);
        return out;
    }
};
