/**
 * @file World.cpp
 * @brief Out-of-line members of World. The contract for each function (parameters,
 *        preconditions, callers) is documented with its declaration in World.h.
 */
#include "World.h"

#include <algorithm>   // std::sort, std::max, std::clamp
#include "Tuning.h"    // Tune::SEGMENT_HP, SIZE_BONUS, JITTER, ATTACK, DEFENSE

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
    returnEnergyToTiles(o);
    for (int c : o.body) grid.clearCell(c);
    o.body.clear();
    o.hp.clear();
    o.alive = false;
    ++deaths;
}

int World::energyCapacity(const Organism& o) const {
    const int len = int(o.body.size());
    int sum = 0;
    for (int i = 0; i < len; ++i) sum += Tune::CAPACITY[o.genome.typeAt(i, len)];
    return sum;
}

int64_t World::totalEnergy() const {
    int64_t sum = 0;
    for (const Organism& o : orgs)
        if (o.alive) sum += o.energy;
    for (uint16_t f : grid.food) sum += f;
    return sum;
}

void World::returnEnergyToTiles(Organism& o) {
    const int n = int(o.body.size());
    if (n == 0) {                                // should not happen; keep the ledger exact anyway
        energyOut += std::max(0, o.energy);
        o.energy = 0;
        return;
    }
    const int pool = std::max(0, o.energy);
    const int share = pool / n;
    const int extra = pool % n;                  // the first `extra` segments get one more unit
    for (int i = 0; i < n; ++i) {
        const int amount = share + (i < extra ? 1 : 0);
        uint16_t& tile = grid.food[size_t(o.body[size_t(i)])];
        const int room = std::max(0, Tune::FOOD_MAX - int(tile));
        const int put = std::min(amount, room);
        tile = uint16_t(tile + put);
        energyOut += amount - put;               // what does not fit on the tile is dissipated
    }
    o.energy = 0;
}

bool World::spawn(int cellIdx, const Genome& g) {
    if (grid.type[cellIdx] != T_EMPTY) return false;
    Organism o;
    o.genome = g;
    o.facing = int(rng() % 6);
    o.body.push_back(cellIdx);
    o.hp.assign(1, Tune::SEGMENT_HP);
    o.energy = Tune::NEWBORN_ENERGY;
    energyIn += Tune::NEWBORN_ENERGY;       // created from nothing; counted in the ledger
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

// ------------------------ Step Pipeline --------------------------

bool World::decide(const Organism& o, uint32_t id, Intent& out) {
    const Genome& g = o.genome;
    int head = o.body.front();

    double w[6];
    int target[6];
    double sum = 0.0;
    for (int rel = 0; rel < 6; ++rel) {
        int n = grid.neighbor(head, (o.facing + rel) % 6);
        target[rel] = n;
        bool free = (n >= 0 && grid.type[n] == T_EMPTY);
        w[rel] = free ? double(g.moveWeight[rel]) : 0.0;
        sum += w[rel];
    }
    if (sum <= 0.0) return false;                      // boxed in

    std::discrete_distribution<int> pick(w, w + 6);
    int rel = pick(rng);

    std::uniform_real_distribution<float> u01(0.0f, 1.0f);
    out.id = id;
    out.rel = rel;
    out.target = target[rel];
    out.score = g.agility
        + Tune::SIZE_BONUS / float(o.body.size())   // smaller worms get a bonus
        + Tune::JITTER * u01(rng);
    return true;
}

void World::applyMove(Organism& o, uint32_t id, const Intent& in) {
    bool grow = int(o.body.size()) < o.genome.targetLength
        && (tick % o.genome.growInterval == 0);

    o.body.push_front(in.target);
    o.facing = (o.facing + in.rel) % 6;
    if (grow) {
        o.hp.push_back(Tune::SEGMENT_HP);       // new segment appended at the tail end
    }
    else {
        grid.clearCell(o.body.back());
        o.body.pop_back();
    }
    retype(o, id);
}

void World::step() {
    // 1. DECIDE: every living worm proposes a move from the same snapshot.
    intents.clear();
    for (size_t i = 0; i < orgs.size(); ++i) {
        Intent in;
        if (orgs[i].alive && decide(orgs[i], uint32_t(i + 1), in)) intents.push_back(in);
    }

    // 2. RESOLVE: group intents by target cell, pick one winner per cell.
    std::sort(intents.begin(), intents.end(), [](const Intent& a, const Intent& b) {
        return a.target != b.target ? a.target < b.target : a.id < b.id;
        });

    winners.clear();
    damage.assign(orgs.size() + 1, 0);
    for (size_t a = 0; a < intents.size();) {
        size_t b = a;
        while (b < intents.size() && intents[b].target == intents[a].target) ++b;

        size_t win = a;
        for (size_t k = a + 1; k < b; ++k)
            if (intents[k].score > intents[win].score) win = k;

        if (b - a > 1) {
            ++contests;
            // Every loser's collision may hurt the winner's head.
            int winnerDef = defensePower(orgs[intents[win].id - 1]);
            for (size_t k = a; k < b; ++k) {
                if (k == win) continue;
                int atk = attackPower(orgs[intents[k].id - 1]);
                damage[intents[win].id] += std::max(0, atk - winnerDef);
            }
        }
        winners.push_back(intents[win]);
        a = b;
    }

    // 3. APPLY: winners move, then collision damage lands, then deaths.
    for (const Intent& in : winners) applyMove(orgs[in.id - 1], in.id, in);

    for (size_t i = 0; i < orgs.size(); ++i) {
        if (!orgs[i].alive || damage[i + 1] <= 0) continue;
        orgs[i].hp[0] -= damage[i + 1];
        if (orgs[i].hp[0] <= 0) kill(orgs[i]);      // head destroyed = organism dies (for now)
        else                    retype(orgs[i], uint32_t(i + 1));
    }
    ++tick;
}