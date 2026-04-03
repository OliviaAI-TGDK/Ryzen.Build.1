enum class SimplexDirection : uint8_t {
    IN = 1,
    OUT = 2,
    INOUT = 3
};

static inline const char* simplexDirName(SimplexDirection d) {
    switch (d) {
        case SimplexDirection::IN: return "in";
        case SimplexDirection::OUT: return "out";
        case SimplexDirection::INOUT: return "in/out";
        default: return "unknown";
    }
}

struct SimplexModifierHook {
    std::string name = "identity";
    double gain = 1.0;
    double bias = 0.0;
    double clampMin = 0.001;
    double clampMax = 1.0;
    bool enabled = true;

    double apply(double v) const {
        if (!enabled) return v;
        return clamp(v * gain + bias, clampMin, clampMax);
    }
};

struct SimplexForkState {
    uint32_t forkIndex = 0;
    SimplexDirection direction = SimplexDirection::INOUT;
    double rake = 0.0;
    double returned = 0.0;
    double released = 0.0;
    double relay = 0.0;
};

struct SimplexDeckLane {
    uint32_t laneIndex = 0;
    uint32_t sourceMipLevel = 0;
    double retainedRatio = 0.0;
    double reductionRatio = 0.0;
    double vramWeight = 0.0;
    double armedValue = 0.0;
    double addedValue = 0.0;
    double modifiedValue = 0.0;
    std::array<SimplexForkState, 3> forks{};
};

struct SimplexAdapterConfig {
    // 12 VRAM deck mod
    uint32_t deckCount = 12;

    // ARM / ADDER / Modifier
    double arm = 1.0;
    double adder = 0.0;
    double modifier = 1.0;

    // Rake / Return / Release
    double rake = 1.0;
    double returnGain = 0.92;
    double releaseGain = 0.97;

    // u_scalar / supercompute / relay
    double u_scalar = 1.0;
    bool supercomputeSwitcher = false;
    bool chipRelay = false;

    // Knot alignment
    double knotScalar = 0.4896; // 8*4*3/2*0.0102
};

struct SimplexAdapterReport {
    double adapterRetainedRatio = 0.0;
    double adapterReductionRatio = 0.0;
    double deckEnergy = 0.0;
    double knotScalar = 0.4896;
    double routeConfidence = 0.0;
    std::array<SimplexDeckLane, 12> lanes{};
};

class DawnstarSimplexAdapter {
public:
    explicit DawnstarSimplexAdapter(SimplexAdapterConfig cfg = {}) : cfg_(cfg) {
        cfg_.deckCount = 12;
        cfg_.u_scalar = clamp(cfg_.u_scalar, 0.125, 16.0);
        cfg_.arm = clamp(cfg_.arm, 0.0, 4.0);
        cfg_.modifier = clamp(cfg_.modifier, 0.125, 8.0);
        cfg_.rake = clamp(cfg_.rake, 0.125, 8.0);
        cfg_.returnGain = clamp(cfg_.returnGain, 0.0, 1.5);
        cfg_.releaseGain = clamp(cfg_.releaseGain, 0.0, 1.5);
    }

    void addModifier(const SimplexModifierHook& hook) {
        modifiers_.push_back(hook);
    }

    SimplexAdapterReport adapt(const CompressionProfile& profile,
                               const std::array<double, DS_MIP_LEVELS>& mipRetained) const {
        SimplexAdapterReport report{};
        report.knotScalar = cfg_.knotScalar;

        static constexpr std::array<SimplexDirection, 3> forkDirs = {
            SimplexDirection::INOUT,
            SimplexDirection::INOUT,
            SimplexDirection::OUT
        };

        // fork weights for R{fork x 3}
        static constexpr std::array<double, 3> forkWeights = {
            1.000, 0.875, 0.750
        };

        double retainedAccum = 0.0;
        double energyAccum = 0.0;
        double confidenceAccum = 0.0;

        for (uint32_t lane = 0; lane < 12; ++lane) {
            const uint32_t mipLevel = lane % DS_MIP_LEVELS;
            const double baseRetained = mipRetained[mipLevel];

            // 12-lane deck weighting
            const double deckNorm = static_cast<double>(lane) / 11.0;
            const double vramWeight =
                0.88 +
                (0.18 * (1.0 - deckNorm)) +
                (0.04 * static_cast<double>(lane % 3));

            // ARM stage
            const double armGate =
                clamp(profile.scores.total * cfg_.arm * cfg_.u_scalar, 0.0, 2.0);

            const double armedValue =
                clamp(baseRetained * armGate * vramWeight, 0.001, 1.0);

            // ADDER stage
            const double adderBias =
                cfg_.adder * 0.0102 * (1.0 + (static_cast<double>(lane % 4) * 0.125));

            const double addedValue =
                clamp(armedValue + adderBias, 0.001, 1.0);

            // Modifier stage
            double modifiedValue =
                clamp(addedValue * cfg_.modifier, 0.001, 1.0);

            for (const auto& mod : modifiers_) {
                modifiedValue = mod.apply(modifiedValue);
            }

            // supercompute / relay shaping
            if (cfg_.supercomputeSwitcher) {
                modifiedValue = clamp(modifiedValue * 0.94, 0.001, 1.0);
            }
            if (cfg_.chipRelay) {
                modifiedValue = clamp(modifiedValue * 0.98, 0.001, 1.0);
            }

            SimplexDeckLane deck{};
            deck.laneIndex = lane;
            deck.sourceMipLevel = mipLevel;
            deck.vramWeight = round_to(vramWeight, 6);
            deck.armedValue = round_to(armedValue, 6);
            deck.addedValue = round_to(addedValue, 6);
            deck.modifiedValue = round_to(modifiedValue, 6);
            deck.retainedRatio = round_to(modifiedValue, 6);
            deck.reductionRatio = round_to(1.0 - modifiedValue, 6);

            for (uint32_t f = 0; f < 3; ++f) {
                const double rakeVal =
                    clamp(modifiedValue * cfg_.rake * forkWeights[f], 0.0, 1.5);
                const double returned =
                    clamp(rakeVal * cfg_.returnGain, 0.0, 1.5);
                const double released =
                    clamp(returned * cfg_.releaseGain, 0.0, 1.5);
                const double relay =
                    cfg_.chipRelay ? clamp(released * 0.985, 0.0, 1.5) : released;

                deck.forks[f] = {
                    f,
                    forkDirs[f],
                    round_to(rakeVal, 6),
                    round_to(returned, 6),
                    round_to(released, 6),
                    round_to(relay, 6)
                };
            }

            // deck energy and confidence
            const double forkMean =
                (deck.forks[0].relay + deck.forks[1].relay + deck.forks[2].relay) / 3.0;

            const double laneConfidence =
                clamp(
                    (profile.scores.signal * 0.26) +
                    (profile.scores.mesh * 0.18) +
                    (profile.scores.health * 0.16) +
                    (profile.scores.broadband * 0.12) +
                    (modifiedValue * 0.28),
                    0.0, 1.0
                );

            retainedAccum += deck.retainedRatio;
            energyAccum += forkMean * cfg_.knotScalar;
            confidenceAccum += laneConfidence;

            report.lanes[lane] = deck;
        }

        report.adapterRetainedRatio = round_to(retainedAccum / 12.0, 6);
        report.adapterReductionRatio = round_to(1.0 - report.adapterRetainedRatio, 6);
        report.deckEnergy = round_to(energyAccum / 12.0, 6);
        report.routeConfidence = round_to(confidenceAccum / 12.0, 6);

        return report;
    }

    std::string renderJSON(const SimplexAdapterReport& report) const {
        std::ostringstream os;
        os << std::fixed << std::setprecision(6);
        os << "{\n";
        os << "  \"simplex_adapter\": {\n";
        os << "    \"deck_count\": 12,\n";
        os << "    \"arm\": " << cfg_.arm << ",\n";
        os << "    \"adder\": " << cfg_.adder << ",\n";
        os << "    \"modifier\": " << cfg_.modifier << ",\n";
        os << "    \"rake\": " << cfg_.rake << ",\n";
        os << "    \"return_gain\": " << cfg_.returnGain << ",\n";
        os << "    \"release_gain\": " << cfg_.releaseGain << ",\n";
        os << "    \"u_scalar\": " << cfg_.u_scalar << ",\n";
        os << "    \"supercompute_switcher\": " << (cfg_.supercomputeSwitcher ? "true" : "false") << ",\n";
        os << "    \"chip_relay\": " << (cfg_.chipRelay ? "true" : "false") << ",\n";
        os << "    \"knot_scalar\": " << cfg_.knotScalar << "\n";
        os << "  },\n";
        os << "  \"report\": {\n";
        os << "    \"adapter_retained_ratio\": " << report.adapterRetainedRatio << ",\n";
        os << "    \"adapter_reduction_ratio\": " << report.adapterReductionRatio << ",\n";
        os << "    \"deck_energy\": " << report.deckEnergy << ",\n";
        os << "    \"route_confidence\": " << report.routeConfidence << ",\n";
        os << "    \"lanes\": [\n";

        for (std::size_t i = 0; i < report.lanes.size(); ++i) {
            const auto& lane = report.lanes[i];
            os << "      {\n";
            os << "        \"lane_index\": " << lane.laneIndex << ",\n";
            os << "        \"source_mip_level\": " << lane.sourceMipLevel << ",\n";
            os << "        \"vram_weight\": " << lane.vramWeight << ",\n";
            os << "        \"armed_value\": " << lane.armedValue << ",\n";
            os << "        \"added_value\": " << lane.addedValue << ",\n";
            os << "        \"modified_value\": " << lane.modifiedValue << ",\n";
            os << "        \"retained_ratio\": " << lane.retainedRatio << ",\n";
            os << "        \"reduction_ratio\": " << lane.reductionRatio << ",\n";
            os << "        \"forks\": [\n";
            for (std::size_t f = 0; f < lane.forks.size(); ++f) {
                const auto& fork = lane.forks[f];
                os << "          {\n";
                os << "            \"fork_index\": " << fork.forkIndex << ",\n";
                os << "            \"direction\": \"" << simplexDirName(fork.direction) << "\",\n";
                os << "            \"rake\": " << fork.rake << ",\n";
                os << "            \"returned\": " << fork.returned << ",\n";
                os << "            \"released\": " << fork.released << ",\n";
                os << "            \"relay\": " << fork.relay << "\n";
                os << "          }" << (f + 1 < lane.forks.size() ? "," : "") << "\n";
            }
            os << "        ]\n";
            os << "      }" << (i + 1 < report.lanes.size() ? "," : "") << "\n";
        }

        os << "    ]\n";
        os << "  }\n";
        os << "}\n";
        return os.str();
    }

private:
    SimplexAdapterConfig cfg_{};
    std::vector<SimplexModifierHook> modifiers_{};
};
