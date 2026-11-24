/**
 * @file World.cpp
 * @brief Out-of-line members of World. The contract for each function (parameters,
 *        preconditions, callers) is documented with its declaration in World.h.
 */
#include "World.h"

#include <algorithm>   // std::sort, std::max, std::clamp
#include "Tuning.h"    // Tune::SEGMENT_HP, SIZE_BONUS, JITTER, ATTACK, DEFENSE

void World::clear() { 
    grid.clear(); orgs.clear(); tick = 0; 
    contests = deaths = 0; starvations = 0;
    energyIn = energyOut = 0; 
    foodAccum = 0; energyEaten = 0; 
    segmentsGrown = 0; 
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

int64_t World::wormEnergy() const {
    int64_t sum = 0;
    for (const Organism& o : orgs)
        if (o.alive) sum += o.energy;
    return sum;
}

int64_t World::foodEnergy() const {
    int64_t sum = 0;
    for (uint16_t f : grid.food) sum += f;
    return sum;
}

int64_t World::totalEnergy() const {
    return wormEnergy() + foodEnergy();
}

// ------------------------ Step Pipeline --------------------------

// -------------- Environment --------------

void World::environmentPhase() {
    const int cells = grid.w * grid.h;
    const int64_t dropUnit = int64_t(Tune::FOOD_DROP) * 1000;      // milli-energy per drop
    foodAccum += int64_t(Tune::FOOD_RATE) * cells;
    const int64_t drops = foodAccum / dropUnit;
    foodAccum -= drops * dropUnit;
    for (int64_t d = 0; d < drops; ++d) {
        const size_t tile = size_t(rng() % uint32_t(cells));
        const int room = std::max(0, Tune::FOOD_MAX - int(grid.food[tile]));
        const int added = std::min(Tune::FOOD_DROP, room);
        grid.food[tile] = uint16_t(grid.food[tile] + added);
        energyIn += added;                       // only what actually lands is created
    }
}

void World::seedFood(int percentOfMax) {
    const int top = Tune::FOOD_MAX * std::clamp(percentOfMax, 0, 100) / 100;
    for (uint16_t& f : grid.food) {
        const int want = int(rng() % uint32_t(top + 1));
        const int added = std::min(want, std::max(0, Tune::FOOD_MAX - int(f)));
        f = uint16_t(f + added);
        energyIn += added;
    }
}

// -------------- Eating --------------

void World::eatPhase() {
    for (Organism& o : orgs) {
        if (!o.alive) continue;
        uint16_t& tile = grid.food[size_t(o.body.front())];
        const int room = energyCapacity(o) - o.energy;
        const int bite = std::min({ int(o.genome.biteSize), int(tile), room });
        if (bite <= 0) continue;                 // full, hungry-but-nothing-here, or bite 0
        o.energy += bite;
        tile = uint16_t(tile - bite);
        energyEaten += bite;
    }
}

// ------------------------ Metabolism --------------------------

int World::upkeepCost(const Organism& o) const {
    const int len = int(o.body.size());
    int sum = 0;
    for (int i = 0; i < len; ++i) sum += Tune::UPKEEP[o.genome.typeAt(i, len)];
    return sum;
}

void World::metabolismPhase() {
    for (Organism& o : orgs) {
        if (!o.alive) continue;
        const int paid = std::min(upkeepCost(o), o.energy);    // cannot pay more than it has
        o.energy -= paid;
        energyOut += paid;                                     // upkeep is destroyed (heat)
        if (o.energy <= 0) {                                   // nothing left: starvation
            kill(o);                                           // returns 0 energy to the tiles
            ++starvations;
        }
    }
}

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
    // Growing is allowed by length and timing, and must be paid for out of the worm's own energy.
    const bool wantsToGrow = int(o.body.size()) < o.genome.targetLength
        && (tick % o.genome.growInterval == 0);
    const bool grow = wantsToGrow
        && o.energy - Tune::GROW_COST >= 1
        && o.energy * 100 >= int(o.genome.growThreshold) * energyCapacity(o);

    o.body.push_front(in.target);
    o.facing = (o.facing + in.rel) % 6;
    if (grow) {
        o.hp.push_back(Tune::SEGMENT_HP);       // new segment appended at the tail end
        o.energy -= Tune::GROW_COST;            // the worm pays for the new segment ...
        energyOut += Tune::GROW_COST;           // ... and that energy is destroyed (ledger)
        ++segmentsGrown;
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

    // 4. EAT: living heads take food from the tile they stand on.
    eatPhase();

    // 5. METABOLISM: every living worm pays upkeep; worms with no energy left starve.
    metabolismPhase();

    // 6. ENVIRONMENT: new food appears on the ground.
    environmentPhase();
    ++tick;
}

// ------------------------ Diagnostics --------------------------
std::string World::validate() const {
    const int cells = grid.w * grid.h;

    // "cell 123 (col 3, row 2)" for error messages. Only call with a valid index.
    auto cellStr = [&](int i) {
        return "cell " + std::to_string(i) + " (col " + std::to_string(i % grid.w) +
            ", row " + std::to_string(i / grid.w) + ")";
        };

    // ---- A. grid arrays ----
    if (cells <= 0 || grid.type.size() != size_t(cells) ||
        grid.orgId.size() != size_t(cells) || grid.hp.size() != size_t(cells) ||
        grid.food.size() != size_t(cells))
        return "grid arrays do not all have exactly w*h elements";

    // ---- B. organisms (also records which organism owns each cell) ----
    std::vector<uint32_t> owner(size_t(cells), 0);
    int deadCount = 0;

    for (size_t k = 0; k < orgs.size(); ++k) {
        const Organism& o = orgs[k];
        const uint32_t id = uint32_t(k + 1);
        const std::string who = "organism " + std::to_string(id);

        if (!o.alive) {
            ++deadCount;
            if (!o.body.empty() || !o.hp.empty())
                return who + " is dead but still has body/hp entries (I1/I3)";
            if (o.energy != 0)
                return who + " is dead but still holds energy " + std::to_string(o.energy) + " (I13)";
            continue;
        }

        const Genome& g = o.genome;
        if (g.period < 1 || g.period > 4)
            return who + ": genome.period " + std::to_string(int(g.period)) + " is outside 1..4 (I10)";
        if (g.growInterval < 1)
            return who + ": genome.growInterval is 0 (I10)";
        if (g.targetLength < 1)
            return who + ": genome.targetLength is 0 (I10)";
        if (o.facing < 0 || o.facing > 5)
            return who + ": facing " + std::to_string(o.facing) + " is outside 0..5 (I9)";
        if (o.body.empty())
            return who + " is alive but has an empty body (I3)";
        if (o.body.size() != o.hp.size())
            return who + ": body has " + std::to_string(o.body.size()) +
            " segments but hp has " + std::to_string(o.hp.size()) + " (I3)";
        if (o.body.size() > g.targetLength)
            return who + ": has " + std::to_string(o.body.size()) +
            " segments, more than genome.targetLength " + std::to_string(int(g.targetLength));
        if (g.growThreshold > 100)
            return who + ": genome.growThreshold " + std::to_string(int(g.growThreshold)) + " is above 100 (I10)";

        const int len = int(o.body.size());
        for (int i = 0; i < len; ++i) {
            const int c = o.body[size_t(i)];
            const std::string seg = who + " segment " + std::to_string(i);

            if (c < 0 || c >= cells)
                return seg + ": cell index " + std::to_string(c) + " is outside the grid (I3)";
            if (owner[size_t(c)] != 0)
                return seg + " at " + cellStr(c) + " is already occupied by organism " +
                std::to_string(owner[size_t(c)]) + " (cell shared, I3/I8)";
            owner[size_t(c)] = id;

            if (i > 0) {
                const int prev = o.body[size_t(i - 1)];   // validated on the previous iteration
                bool adjacent = false;
                for (int d = 0; d < 6 && !adjacent; ++d) adjacent = (grid.neighbor(prev, d) == c);
                if (!adjacent)
                    return seg + " at " + cellStr(c) + " is not adjacent to segment " +
                    std::to_string(i - 1) + " at " + cellStr(prev) + " (I3)";
            }

            const uint8_t expect = g.typeAt(i, len);
            if (expect == T_EMPTY || expect >= T_COUNT)
                return seg + ": genome gives invalid cell type " + std::to_string(int(expect)) + " (I4/I6)";
            if (grid.type[size_t(c)] != expect)
                return seg + " at " + cellStr(c) + ": grid type is " +
                std::to_string(int(grid.type[size_t(c)])) + " but genome expects " +
                std::to_string(int(expect)) + " (I4/I5)";
            if (grid.orgId[size_t(c)] != id)
                return seg + " at " + cellStr(c) + ": grid orgId is " +
                std::to_string(grid.orgId[size_t(c)]) + ", expected " + std::to_string(id) + " (I5)";
            if (o.hp[size_t(i)] <= 0)
                return seg + ": hp is " + std::to_string(o.hp[size_t(i)]) +
                " (a living worm's segments must have hp > 0)";
            if (grid.hp[size_t(c)] != uint8_t(std::clamp(o.hp[size_t(i)], 0, 255)))
                return seg + " at " + cellStr(c) + ": grid hp mirror is " +
                std::to_string(int(grid.hp[size_t(c)])) + " but segment hp is " +
                std::to_string(o.hp[size_t(i)]) + " (I5/I6)";
        }

        // Energy pool (I13). Checked AFTER the segment loop because energyCapacity()
        // indexes Tune::CAPACITY by segment type, which is only safe once every type
        // has been validated above.
        const int capacity = energyCapacity(o);
        if (o.energy < 1 || o.energy > capacity)
            return who + ": energy " + std::to_string(o.energy) + " is outside 1.." +
            std::to_string(capacity) + " (I13)";
    }

    // ---- C. grid sweep ----
    for (int i = 0; i < cells; ++i) {
        if (grid.food[size_t(i)] > Tune::FOOD_MAX)
            return cellStr(i) + " holds food " + std::to_string(int(grid.food[size_t(i)])) +
            ", above FOOD_MAX " + std::to_string(Tune::FOOD_MAX) + " (I14)";
        if (grid.type[size_t(i)] == T_EMPTY) {
            if (grid.orgId[size_t(i)] != 0 || grid.hp[size_t(i)] != 0)
                return cellStr(i) + " is empty but has stale orgId/hp (I5)";
        }
        else if (owner[size_t(i)] == 0) {
            return cellStr(i) + " is occupied (type " + std::to_string(int(grid.type[size_t(i)])) +
                ") but no living organism owns it (I5/I6)";
        }
    }

    // ---- D. bookkeeping ----
    if (uint64_t(deadCount) != deaths)
        return "deaths counter is " + std::to_string(deaths) +
        " but the number of dead organisms is " + std::to_string(deadCount);
    if (starvations > deaths)
        return "starvations counter " + std::to_string(starvations) + " exceeds deaths " + std::to_string(deaths);

    // ---- E. energy ledger (I15) ----
    if (energyIn < 0 || energyOut < 0)
        return "energy ledger counters are negative (in " + std::to_string(energyIn) +
        ", out " + std::to_string(energyOut) + ") (I15)";
    const int64_t held = totalEnergy();
    if (held != energyIn - energyOut)
        return "energy ledger broken: the world holds " + std::to_string(held) +
        " but energyIn - energyOut is " + std::to_string(energyIn - energyOut) + " (I15)";

    // ---- F. food production carry-over (I14) ----
    const int64_t dropUnit = int64_t(Tune::FOOD_DROP) * 1000;
    if (foodAccum < 0 || foodAccum >= dropUnit)
        return "food accumulator " + std::to_string(foodAccum) + " is outside 0.." +
        std::to_string(dropUnit - 1) + " (I14)";

    return std::string();
}