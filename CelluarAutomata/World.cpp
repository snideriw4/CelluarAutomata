/**
 * @file World.cpp
 * @brief Out-of-line members of World. The contract for each function (parameters,
 *        preconditions, callers) is documented with its declaration in World.h.
 */
#include "World.h"

void World::retype(const Organism& o, uint32_t id) {
    int len = int(o.body.size());
    for (int i = 0; i < len; ++i) {
        grid.type[o.body[i]] = o.genome.typeAt(i, len);
        grid.orgId[o.body[i]] = id;
        grid.hp[o.body[i]] = uint8_t(std::clamp(o.hp[i], 0, 255));
    }
}

int World::attackPower(const Organism& o) const {
    int len = int(o.body.size()), sum = 0;
    for (int i = 0; i < len; ++i) sum += Tune::ATTACK[o.genome.typeAt(i, len)];
    return sum;
}

int World::defensePower(const Organism& o) const {
    int len = int(o.body.size()), sum = 0;
    for (int i = 0; i < len; ++i) sum += Tune::DEFENSE[o.genome.typeAt(i, len)];
    return sum;
}

void World::kill(Organism& o) {
    for (int c : o.body) grid.clearCell(c);
    o.body.clear();
    o.hp.clear();
    o.alive = false;
    ++deaths;
}

bool World::spawn(int cellIdx, const Genome& g) {
    if (grid.type[cellIdx] != T_EMPTY) return false;
    Organism o;
    o.genome = g;
    o.facing = int(rng() % 6);
    o.body.push_back(cellIdx);
    o.hp.assign(1, Tune::SEGMENT_HP);
    orgs.push_back(o);
    retype(orgs.back(), uint32_t(orgs.size()));
    return true;
}

void World::spawnRandom(int count) {
    int total = grid.w * grid.h;
    for (int tries = 0, made = 0; made < count && tries < count * 20; ++tries) {
        int cell = int(rng() % total);   // draw 1: where
        Genome g = Genome::random(rng);  // draw 2: what
        if (spawn(cell, g)) ++made;
    }
}