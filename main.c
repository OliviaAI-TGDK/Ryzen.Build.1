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
