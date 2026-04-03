const std::string zcoldPolicy = read_text_file("zcold_policy.json");
if (!zcoldPolicy.empty()) {
    std::cout << "=== XEM ZRAM COLDMET POLICY ===\n";
    std::cout << zcoldPolicy << "\n";
}

DuoQuadratalizerConfig dqCfg;
dqCfg.levelCount = 22;
dqCfg.baseRefraction = 0.0102;
dqCfg.forkGain = 1.0;
dqCfg.refractGain = 1.08;
dqCfg.curvatureStep = 0.03125;
dqCfg.levelGainStep = 0.0125;
dqCfg.knotScalar = 0.4896;

DuoQuadratalizer22 duo22(dqCfg);
auto duoLevels = duo22.build();

std::cout << duo22.renderJSON(duoLevels) << "\n";

CompressionProfile profile = DawnstarPerformanceEngine::compressionProfile(
    frame,
    health,
    cfg.minRetainedRatio,
    cfg.maxRetainedRatio,
    cfg.aggressiveWhenHealthy
);

auto mipRetained = stack.buildMipRetainedRatios(frame, health, 0.25);

SimplexAdapterConfig simplexCfg;
simplexCfg.arm = 1.188;
simplexCfg.adder = 0.42;
simplexCfg.modifier = 1.035;
simplexCfg.rake = 1.0;
simplexCfg.returnGain = 0.93;
simplexCfg.releaseGain = 0.975;
simplexCfg.u_scalar = cfg.gpu.u_scalar;
simplexCfg.supercomputeSwitcher = cfg.gpu.supercomputeSwitcher;
simplexCfg.chipRelay = cfg.gpu.chipRelay;
simplexCfg.knotScalar = 0.4896;

DawnstarSimplexAdapter simplex(simplexCfg);

SimplexModifierHook mod1;
mod1.name = "deck-prebias";
mod1.gain = 1.012;
mod1.bias = 0.00102;
mod1.clampMin = 0.001;
mod1.clampMax = 1.0;
mod1.enabled = true;
simplex.addModifier(mod1);

SimplexModifierHook mod2;
mod2.name = "release-trim";
mod2.gain = 0.988;
mod2.bias = 0.0;
mod2.clampMin = 0.001;
mod2.clampMax = 1.0;
mod2.enabled = true;
simplex.addModifier(mod2);

SimplexAdapterReport simplexReport = simplex.adapt(profile, mipRetained);

std::cout << simplex.renderJSON(simplexReport) << "\n";
