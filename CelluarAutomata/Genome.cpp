/**
 * @file Genome.cpp
 * @brief Out-of-line members of Genome. The contract for each function is
 *        documented in Genome.h.
 */
#include "Genome.h"
#include "Tuning.h"      // Tune::BITE_MIN, Tune::BITE_MAX

Genome Genome::random(std::mt19937& rng) {
	std::uniform_real_distribution<float> u01(0.0f, 1.0f);
	Genome g;
	g.period = uint8_t(1 + rng() % 4);
	for (int i = 0; i < 4; ++i) g.pattern[i] = uint8_t(T_MOVER + rng() % 4);
	g.targetLength = uint8_t(10 + rng() % 25);	// []
	g.growInterval = uint8_t(1 + rng() % 4);
	g.agility = 3.0f * u01(rng);
	for (int i = 0; i < 6; ++i) g.moveWeight[i] = 0.05f + 0.5f * u01(rng);
	static const int prefs[5] = { 0, 1, 2, 4, 5 };
	g.moveWeight[prefs[rng() % 5]] += 3.0f;
	g.biteSize = uint16_t(Tune::BITE_MIN + rng() % uint32_t(Tune::BITE_MAX - Tune::BITE_MIN + 1));
	return g;
}