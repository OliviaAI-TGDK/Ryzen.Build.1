// dawnstar_ryzen_stack.cpp
//
// Single-file Dawnstar stack for Ryzen/AMD GPU Linux deployments.
//
// What it does:
// - wraps Dawnstar scoring + live compression ratio
// - emits 9-level mip retention profile
// - carries ds_pyramid_desc-compatible metadata
// - adds metronome adder
// - adds GPU acceleration policy
// - adds bounded fixed-performance / overdrive plan
// - adds u_scalar, supercompute switcher, chip relay
//
// Notes:
// - This is a control/policy shim, not a firmware flasher.
// - Overdrive is clamped to 20%.
// - Actual pp_od_clk_voltage tuning syntax varies by device/driver generation.
// - power_dpm_force_performance_level path is rendered as a plan target.
// - Safe default mode is "manual" perf level with bounded policy emission.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace dawnstar {

constexpr std::size_t DS_MIP_LEVELS = 9;

enum ds_compression_mode {
    DS_MODE_LOSSLESS    = 0,
    DS_MODE_LOSSY_Q8    = 1,
    DS_MODE_LOSSY_Q4    = 2,
    DS_MODE_FEATURE_MAP = 3
};

struct ds_mip_level_desc {
    uint32_t level       = 0;
    uint32_t width       = 0;
    uint32_t height      = 0;
    uint32_t stride      = 0;
    uint64_t gpu_addr    = 0;
    uint64_t bytes_in    = 0;
    uint64_t bytes_out   = 0;
    uint32_t tile_size   = 0;
    uint32_t quant_bits  = 0;
    uint32_t checksum32  = 0;
    uint32_t flags       = 0;
};

struct ds_pyramid_desc {
    uint32_t version         = 1;
    uint32_t levels          = DS_MIP_LEVELS;
    uint32_t mode            = DS_MODE_LOSSY_Q4;
    uint32_t reserved        = 0;
    uint64_t src_gpu_addr    = 0;
    uint64_t src_bytes       = 0;
    uint64_t total_out_bytes = 0;
    float target_ratio       = 0.0102f;
    ds_mip_level_desc mip[DS_MIP_LEVELS];
};

template <typename T>
static inline T clamp(T value, T lo, T hi) {
    return std::max(lo, std::min(hi, value));
}

static inline double round_to(double value, int digits) {
    const double p = std::pow(10.0, digits);
    return std::round(value * p) / p;
}

static inline double avg(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    return std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
}

struct StrongestElement {
    double energy = 0.0;
};

struct BroadbandChannel {
    double utilization = 0.0;
};

struct MeshStatus {
    bool delivered = false;
    int hops = 0;
};

struct DawnstarFrame {
    double totalEnergy = 0.0;
    std::vector<StrongestElement> strongestElements;
    std::vector<BroadbandChannel> broadband;
    MeshStatus mesh;
    std::string mode = "hybrid";
    double targetAngleDeg = 0.0;
    double timestamp = 0.0;
};

struct DawnstarHealthReport {
    double averageTemperatureC = 25.0;
    double averageHealth = 1.0;
    int foldsActive = 21;
};

struct PerformanceScores {
    double signal = 0.0;
    double health = 0.0;
    double thermal = 0.0;
    double mesh = 0.0;
    double broadband = 0.0;
    double foldActivity = 0.0;
    double total = 0.0;
};

struct CompressionProfile {
    PerformanceScores scores;
    double retainedRatio = 0.0102;
    double reductionRatio = 0.9898;
    double compressionFactor = 98.0392;
};

class DawnstarPerformanceEngine {
public:
    static double strongestEnergyMean(const DawnstarFrame& frame) {
        if (frame.strongestElements.empty()) return 0.0;
        std::vector<double> vals;
        vals.reserve(frame.strongestElements.size());
        for (const auto& e : frame.strongestElements) vals.push_back(e.energy);
        return avg(vals);
    }

    static double broadbandUtilizationScore(const DawnstarFrame& frame) {
        if (frame.broadband.empty()) return 0.5;

        std::vector<double> utils;
        utils.reserve(frame.broadband.size());
        for (const auto& ch : frame.broadband) utils.push_back(ch.utilization);

        const double mean = avg(utils);

        double spreadPenalty = 0.0;
        for (double v : utils) spreadPenalty += std::abs(v - mean);
        spreadPenalty /= static_cast<double>(utils.size());

        double score = 1.0 - std::abs(mean - 0.55) / 0.55;
        score -= spreadPenalty * 0.5;

        return clamp(score, 0.0, 1.0);
    }

    static double meshScore(const DawnstarFrame& frame) {
        double score = 0.0;
        if (frame.mesh.delivered) score += 0.55;
        score += clamp(static_cast<double>(frame.mesh.hops) / 16.0, 0.0, 0.45);
        return clamp(score, 0.0, 1.0);
    }

    static double thermalScore(const DawnstarHealthReport& healthReport) {
        const double t = healthReport.averageTemperatureC;
        if (t <= 35.0) return 1.0;
        if (t <= 45.0) return 0.88;
        if (t <= 55.0) return 0.68;
        if (t <= 65.0) return 0.42;
        return 0.18;
    }

    static double healthScore(const DawnstarHealthReport& healthReport) {
        return clamp(healthReport.averageHealth, 0.0, 1.0);
    }

    static double foldActivityScore(const DawnstarHealthReport& healthReport) {
        return clamp(static_cast<double>(healthReport.foldsActive) / 21.0, 0.0, 1.0);
    }

    static double signalScore(const DawnstarFrame& frame) {
        const double totalEnergy = frame.totalEnergy;
        const double strongestMean = strongestEnergyMean(frame);

        const double energyComponent = clamp(totalEnergy / 80.0, 0.0, 1.0);
        const double strongestComponent = clamp(strongestMean / 1.25, 0.0, 1.0);

        return clamp((energyComponent * 0.6) + (strongestComponent * 0.4), 0.0, 1.0);
    }

    static PerformanceScores platformScore(const DawnstarFrame& frame,
                                           const DawnstarHealthReport& healthReport) {
        PerformanceScores s{};
        s.signal = round_to(signalScore(frame), 4);
        s.health = round_to(healthScore(healthReport), 4);
        s.thermal = round_to(thermalScore(healthReport), 4);
        s.mesh = round_to(meshScore(frame), 4);
        s.broadband = round_to(broadbandUtilizationScore(frame), 4);
        s.foldActivity = round_to(foldActivityScore(healthReport), 4);

        const double total =
            s.signal * 0.24 +
            s.health * 0.18 +
            s.thermal * 0.12 +
            s.mesh * 0.16 +
            s.broadband * 0.14 +
            s.foldActivity * 0.16;

        s.total = round_to(clamp(total, 0.0, 1.0), 4);
        return s;
    }

    static CompressionProfile compressionProfile(const DawnstarFrame& frame,
                                                 const DawnstarHealthReport& healthReport,
                                                 double minRetainedRatio = 0.0102,
                                                 double maxRetainedRatio = 0.12,
                                                 bool aggressiveWhenHealthy = true) {
        CompressionProfile out{};
        out.scores = platformScore(frame, healthReport);

        const double total = clamp(out.scores.total, 0.0, 1.0);

        double retainedRatio = 0.0;
        if (aggressiveWhenHealthy) {
            retainedRatio =
                maxRetainedRatio - (maxRetainedRatio - minRetainedRatio) * total;
        } else {
            retainedRatio =
                minRetainedRatio + (maxRetainedRatio - minRetainedRatio) * total;
        }

        retainedRatio = clamp(retainedRatio, minRetainedRatio, maxRetainedRatio);
        out.retainedRatio = round_to(retainedRatio, 6);
        out.reductionRatio = round_to(1.0 - retainedRatio, 6);
        out.compressionFactor = round_to(1.0 / retainedRatio, 4);
        return out;
    }
};

struct MetronomeConfig {
    double hz = 2.0;
    double adder = 1.0;
    double phase = 0.0;
    bool enabled = true;

    void tick(double dt) {
        if (!enabled) return;
        phase = std::fmod(phase + (dt * hz * adder * 2.0 * M_PI), 2.0 * M_PI);
        if (phase < 0.0) phase += 2.0 * M_PI;
    }

    double pulse01() const {
        if (!enabled) return 0.5;
        return 0.5 + 0.5 * std::sin(phase);
    }
};

struct GpuAccelPolicy {
    bool enabled = true;
    bool fixedPerformance = true;
    std::string perfLevel = "manual";    // maps to power_dpm_force_performance_level
    unsigned overdrivePct = 10;          // bounded to 20
    double u_scalar = 1.0;               // supercompute scalar
    bool supercomputeSwitcher = false;
    bool chipRelay = false;
    bool metronomeBoost = true;

    void normalize() {
        overdrivePct = std::min(overdrivePct, 20u);
        u_scalar = clamp(u_scalar, 0.125, 16.0);
        if (perfLevel.empty()) perfLevel = "manual";
    }
};

struct StackConfig {
    uint32_t width = 4096;
    uint32_t height = 4096;
    uint32_t stride = 4096;
    uint32_t tileSize = 16;
    uint32_t bytesPerElement = 4;
    ds_compression_mode mode = DS_MODE_LOSSY_Q4;
    double minRetainedRatio = 0.0102;
    double maxRetainedRatio = 0.12;
    bool aggressiveWhenHealthy = true;
    uint64_t srcGpuAddr = 0x0;
    uint64_t srcBytes = 0;
    MetronomeConfig metronome{};
    GpuAccelPolicy gpu{};
};

class RyzenStack {
public:
    explicit RyzenStack(StackConfig cfg = {}) : cfg_(cfg) {
        cfg_.gpu.normalize();
        if (cfg_.srcBytes == 0) {
            cfg_.srcBytes = static_cast<uint64_t>(cfg_.width) *
                            static_cast<uint64_t>(cfg_.height) *
                            static_cast<uint64_t>(cfg_.bytesPerElement);
        }
    }

    std::array<double, DS_MIP_LEVELS>
    buildMipRetainedRatios(const DawnstarFrame& frame,
                           const DawnstarHealthReport& health,
                           double dt = 0.25) {
        cfg_.metronome.tick(dt);

        CompressionProfile profile = DawnstarPerformanceEngine::compressionProfile(
            frame,
            health,
            cfg_.minRetainedRatio,
            cfg_.maxRetainedRatio,
            cfg_.aggressiveWhenHealthy
        );

        const double pulse = cfg_.metronome.pulse01();
        const double metroScale =
            cfg_.gpu.metronomeBoost ? clamp(0.85 + pulse * 0.30 * cfg_.metronome.adder, 0.5, 1.5) : 1.0;

        const double accelScale =
            cfg_.gpu.enabled ? clamp(1.0 / cfg_.gpu.u_scalar, 0.0625, 8.0) : 1.0;

        const double relayScale =
            cfg_.gpu.chipRelay ? 0.96 : 1.0;

        const double supercomputeScale =
            cfg_.gpu.supercomputeSwitcher ? 0.92 : 1.0;

        const double base =
            clamp(profile.retainedRatio * metroScale * accelScale * relayScale * supercomputeScale,
                  cfg_.minRetainedRatio,
                  cfg_.maxRetainedRatio);

        static constexpr std::array<double, DS_MIP_LEVELS> weights = {
            2.40, 1.85, 1.40, 1.05, 0.80, 0.60, 0.45, 0.30, 0.20
        };

        std::array<double, DS_MIP_LEVELS> out{};
        for (std::size_t i = 0; i < DS_MIP_LEVELS; ++i) {
            out[i] = clamp(base * weights[i], 0.001, 1.0);
        }
        return out;
    }

    ds_pyramid_desc
    buildPyramid(const DawnstarFrame& frame,
                 const DawnstarHealthReport& health,
                 double dt = 0.25) {
        ds_pyramid_desc desc{};
        desc.version = 1;
        desc.levels = static_cast<uint32_t>(DS_MIP_LEVELS);
        desc.mode = static_cast<uint32_t>(cfg_.mode);
        desc.src_gpu_addr = cfg_.srcGpuAddr;
        desc.src_bytes = cfg_.srcBytes;

        const auto retained = buildMipRetainedRatios(frame, health, dt);

        uint64_t totalOut = 0;
        uint32_t w = cfg_.width;
        uint32_t h = cfg_.height;

        for (std::size_t i = 0; i < DS_MIP_LEVELS; ++i) {
            auto& m = desc.mip[i];
            m.level = static_cast<uint32_t>(i);
            m.width = std::max(1u, w);
            m.height = std::max(1u, h);
            m.stride = std::max(1u, w * cfg_.bytesPerElement);
            m.tile_size = cfg_.tileSize;

            const uint64_t bytesIn =
                static_cast<uint64_t>(m.width) *
                static_cast<uint64_t>(m.height) *
                static_cast<uint64_t>(cfg_.bytesPerElement);

            const uint64_t bytesOut =
                static_cast<uint64_t>(std::max(1.0, std::round(bytesIn * retained[i])));

            m.bytes_in = bytesIn;
            m.bytes_out = bytesOut;
            m.quant_bits = quantBitsForLevel(i, cfg_.mode);
            m.flags = buildFlagsForLevel(i);
            m.gpu_addr = cfg_.srcGpuAddr + static_cast<uint64_t>(i) * 0x100000ull;
            m.checksum32 = simpleChecksum(m.level, m.width, m.height, m.bytes_out, m.quant_bits);

            totalOut += bytesOut;

            w = std::max(1u, w >> 1);
            h = std::max(1u, h >> 1);
        }

        desc.total_out_bytes = totalOut;
        desc.target_ratio = static_cast<float>(
            clamp(static_cast<double>(totalOut) / static_cast<double>(std::max<uint64_t>(1, desc.src_bytes)),
                  0.0, 1.0)
        );

        return desc;
    }

    std::string renderLinuxAmdPlan(const std::string& card = "card0") const {
        std::ostringstream os;
        const std::string base = "/sys/class/drm/" + card + "/device";

        os << "{\n";
        os << "  \"platform\": \"linux-amdgpu\",\n";
        os << "  \"perf_level_path\": \"" << base << "/power_dpm_force_performance_level\",\n";
        os << "  \"od_voltage_path\": \"" << base << "/pp_od_clk_voltage\",\n";
        os << "  \"fixed_performance\": " << (cfg_.gpu.fixedPerformance ? "true" : "false") << ",\n";
        os << "  \"perf_level\": \"" << cfg_.gpu.perfLevel << "\",\n";
        os << "  \"overdrive_pct\": " << cfg_.gpu.overdrivePct << ",\n";
        os << "  \"u_scalar\": " << round_to(cfg_.gpu.u_scalar, 4) << ",\n";
        os << "  \"supercompute_switcher\": " << (cfg_.gpu.supercomputeSwitcher ? "true" : "false") << ",\n";
        os << "  \"chip_relay\": " << (cfg_.gpu.chipRelay ? "true" : "false") << ",\n";
        os << "  \"commands\": [\n";
        os << "    \"echo " << cfg_.gpu.perfLevel << " > " << base << "/power_dpm_force_performance_level\",\n";
        os << "    \"# apply bounded overdrive via AMD SMI or pp_od_clk_voltage on supported hardware\",\n";
        os << "    \"# bounded overdrive percent = " << cfg_.gpu.overdrivePct << "\"\n";
        os << "  ]\n";
        os << "}";
        return os.str();
    }

    std::string renderManifestJSON(const DawnstarFrame& frame,
                                   const DawnstarHealthReport& health,
                                   const std::string& card = "card0",
                                   double dt = 0.25) {
        const CompressionProfile profile = DawnstarPerformanceEngine::compressionProfile(
            frame,
            health,
            cfg_.minRetainedRatio,
            cfg_.maxRetainedRatio,
            cfg_.aggressiveWhenHealthy
        );

        const auto retained = buildMipRetainedRatios(frame, health, dt);
        const ds_pyramid_desc pyramid = buildPyramid(frame, health, dt);

        std::ostringstream os;
        os << std::fixed << std::setprecision(6);
        os << "{\n";
        os << "  \"stack\": \"dawnstar-ryzen-stack\",\n";
        os << "  \"frame\": {\n";
        os << "    \"mode\": \"" << frame.mode << "\",\n";
        os << "    \"target_angle_deg\": " << frame.targetAngleDeg << ",\n";
        os << "    \"timestamp\": " << frame.timestamp << ",\n";
        os << "    \"total_energy\": " << frame.totalEnergy << "\n";
        os << "  },\n";
        os << "  \"health\": {\n";
        os << "    \"average_temperature_c\": " << health.averageTemperatureC << ",\n";
        os << "    \"average_health\": " << health.averageHealth << ",\n";
        os << "    \"folds_active\": " << health.foldsActive << "\n";
        os << "  },\n";
        os << "  \"scores\": {\n";
        os << "    \"signal\": " << profile.scores.signal << ",\n";
        os << "    \"health\": " << profile.scores.health << ",\n";
        os << "    \"thermal\": " << profile.scores.thermal << ",\n";
        os << "    \"mesh\": " << profile.scores.mesh << ",\n";
        os << "    \"broadband\": " << profile.scores.broadband << ",\n";
        os << "    \"fold_activity\": " << profile.scores.foldActivity << ",\n";
        os << "    \"total\": " << profile.scores.total << "\n";
        os << "  },\n";
        os << "  \"compression\": {\n";
        os << "    \"retained_ratio\": " << profile.retainedRatio << ",\n";
        os << "    \"reduction_ratio\": " << profile.reductionRatio << ",\n";
        os << "    \"compression_factor\": " << profile.compressionFactor << "\n";
        os << "  },\n";
        os << "  \"metronome\": {\n";
        os << "    \"enabled\": " << (cfg_.metronome.enabled ? "true" : "false") << ",\n";
        os << "    \"hz\": " << cfg_.metronome.hz << ",\n";
        os << "    \"adder\": " << cfg_.metronome.adder << ",\n";
        os << "    \"phase\": " << cfg_.metronome.phase << ",\n";
        os << "    \"pulse01\": " << cfg_.metronome.pulse01() << "\n";
        os << "  },\n";
        os << "  \"gpu_policy\": {\n";
        os << "    \"enabled\": " << (cfg_.gpu.enabled ? "true" : "false") << ",\n";
        os << "    \"fixed_performance\": " << (cfg_.gpu.fixedPerformance ? "true" : "false") << ",\n";
        os << "    \"perf_level\": \"" << cfg_.gpu.perfLevel << "\",\n";
        os << "    \"overdrive_pct\": " << cfg_.gpu.overdrivePct << ",\n";
        os << "    \"u_scalar\": " << cfg_.gpu.u_scalar << ",\n";
        os << "    \"supercompute_switcher\": " << (cfg_.gpu.supercomputeSwitcher ? "true" : "false") << ",\n";
        os << "    \"chip_relay\": " << (cfg_.gpu.chipRelay ? "true" : "false") << "\n";
        os << "  },\n";
        os << "  \"mip_retained_ratios\": [";
        for (std::size_t i = 0; i < retained.size(); ++i) {
            if (i) os << ", ";
            os << retained[i];
        }
        os << "],\n";
        os << "  \"pyramid\": {\n";
        os << "    \"version\": " << pyramid.version << ",\n";
        os << "    \"levels\": " << pyramid.levels << ",\n";
        os << "    \"mode\": " << pyramid.mode << ",\n";
        os << "    \"src_gpu_addr\": " << pyramid.src_gpu_addr << ",\n";
        os << "    \"src_bytes\": " << pyramid.src_bytes << ",\n";
        os << "    \"total_out_bytes\": " << pyramid.total_out_bytes << ",\n";
        os << "    \"target_ratio\": " << pyramid.target_ratio << ",\n";
        os << "    \"mips\": [\n";
        for (std::size_t i = 0; i < DS_MIP_LEVELS; ++i) {
            const auto& m = pyramid.mip[i];
            os << "      {\n";
            os << "        \"level\": " << m.level << ",\n";
            os << "        \"width\": " << m.width << ",\n";
            os << "        \"height\": " << m.height << ",\n";
            os << "        \"stride\": " << m.stride << ",\n";
            os << "        \"gpu_addr\": " << m.gpu_addr << ",\n";
            os << "        \"bytes_in\": " << m.bytes_in << ",\n";
            os << "        \"bytes_out\": " << m.bytes_out << ",\n";
            os << "        \"tile_size\": " << m.tile_size << ",\n";
            os << "        \"quant_bits\": " << m.quant_bits << ",\n";
            os << "        \"checksum32\": " << m.checksum32 << ",\n";
            os << "        \"flags\": " << m.flags << "\n";
            os << "      }" << (i + 1 < DS_MIP_LEVELS ? "," : "") << "\n";
        }
        os << "    ]\n";
        os << "  },\n";
        os << "  \"linux_amdgpu_plan\": " << renderLinuxAmdPlan(card) << "\n";
        os << "}\n";

        return os.str();
    }

    bool writeFile(const std::string& path, const std::string& contents) const {
        std::ofstream out(path, std::ios::binary);
        if (!out) return false;
        out << contents;
        return static_cast<bool>(out);
    }

private:
    StackConfig cfg_;

    static uint32_t quantBitsForLevel(std::size_t level, ds_compression_mode mode) {
        if (mode == DS_MODE_LOSSLESS) return 16;
        if (mode == DS_MODE_FEATURE_MAP) {
            static constexpr uint32_t q[DS_MIP_LEVELS] = {8, 8, 6, 6, 4, 4, 3, 2, 1};
            return q[level];
        }
        if (mode == DS_MODE_LOSSY_Q8) {
            static constexpr uint32_t q[DS_MIP_LEVELS] = {8, 8, 8, 6, 6, 4, 4, 3, 2};
            return q[level];
        }
        static constexpr uint32_t q[DS_MIP_LEVELS] = {8, 8, 6, 6, 4, 4, 3, 2, 1};
        return q[level];
    }

    static uint32_t buildFlagsForLevel(std::size_t level) {
        const bool sparse = level >= 5;
        const bool residual = level <= 3;
        const bool relayEligible = (level % 2) == 0;

        uint32_t flags = 0;
        if (sparse) flags |= 0x01;
        if (residual) flags |= 0x02;
        if (relayEligible) flags |= 0x04;
        return flags;
    }

    static uint32_t simpleChecksum(uint32_t a, uint32_t b, uint32_t c,
                                   uint64_t d, uint32_t e) {
        uint64_t x = 0x9E3779B97F4A7C15ull;
        x ^= static_cast<uint64_t>(a) + 0x9E37 + (x << 6) + (x >> 2);
        x ^= static_cast<uint64_t>(b) + 0x85EB + (x << 6) + (x >> 2);
        x ^= static_cast<uint64_t>(c) + 0xC2B2 + (x << 6) + (x >> 2);
        x ^= d + 0x27D4 + (x << 6) + (x >> 2);
        x ^= static_cast<uint64_t>(e) + 0x1656 + (x << 6) + (x >> 2);
        return static_cast<uint32_t>((x >> 32) ^ (x & 0xffffffffu));
    }
};

} // namespace dawnstar

int main() {
    using namespace dawnstar;

    DawnstarFrame frame;
    frame.totalEnergy = 61.188;
    frame.mode = "hybrid";
    frame.targetAngleDeg = 144.0;
    frame.timestamp = 1.0;
    frame.mesh = { true, 12 };
    frame.strongestElements = {
        {1.12}, {1.08}, {1.05}, {0.99}, {0.95}, {0.91},
        {0.88}, {0.82}, {0.79}, {0.75}, {0.70}, {0.68}
    };
    frame.broadband = {
        {0.41}, {0.49}, {0.63}, {0.58}
    };

    DawnstarHealthReport health;
    health.averageTemperatureC = 39.5;
    health.averageHealth = 0.972;
    health.foldsActive = 21;

    StackConfig cfg;
    cfg.width = 4096;
    cfg.height = 4096;
    cfg.stride = 4096 * 4;
    cfg.tileSize = 16;
    cfg.bytesPerElement = 4;
    cfg.mode = DS_MODE_LOSSY_Q4;
    cfg.minRetainedRatio = 0.0102;
    cfg.maxRetainedRatio = 0.12;
    cfg.aggressiveWhenHealthy = true;
    cfg.srcGpuAddr = 0x100000000ull;
    cfg.srcBytes = static_cast<uint64_t>(cfg.width) *
                   static_cast<uint64_t>(cfg.height) *
                   static_cast<uint64_t>(cfg.bytesPerElement);

    cfg.metronome.enabled = true;
    cfg.metronome.hz = 2.4;
    cfg.metronome.adder = 1.188;

    cfg.gpu.enabled = true;
    cfg.gpu.fixedPerformance = true;
    cfg.gpu.perfLevel = "manual";
    cfg.gpu.overdrivePct = 12;       // clamped <= 20
    cfg.gpu.u_scalar = 1.75;
    cfg.gpu.supercomputeSwitcher = true;
    cfg.gpu.chipRelay = true;
    cfg.gpu.metronomeBoost = true;

    RyzenStack stack(cfg);

    const std::string manifest = stack.renderManifestJSON(frame, health, "card0", 0.25);
    std::cout << manifest << "\n";

    // Optional:
    // stack.writeFile("dawnstar_ryzen_manifest.json", manifest);

    return 0;
}
