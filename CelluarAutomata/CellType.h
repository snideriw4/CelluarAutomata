#pragma once
/**
 * @file CellType.h
 * @brief The set of cell kinds a worm segment can have.
 * Dependencies: none. Used by: Tuning.h, HexGrid.h, Genome, World, Render.
 */
#include <cstdint>

 /**
  * @brief Kind of a grid cell / worm segment. Stored in HexGrid::type as uint8_t.
  *
  * T_EMPTY must stay 0: HexGrid is zero/empty-initialized and "type == 0" means
  * unoccupied everywhere. T_COUNT must stay last (it sizes the lookup tables).
  *
  * @warning When adding a type, extend TYPE_COLOR[] (Render.cpp) and
  *          Tune::ATTACK[] / Tune::DEFENSE[] (Tuning.h) at the same position.
  *          All three tables are checked by static_assert, so a missing entry
  *          is a compile error.
  */
enum CellType : uint8_t {
    T_EMPTY = 0, T_HEAD, T_MOVER, T_STORAGE, T_DEFENSE, T_OFFENSE, T_TAIL, T_COUNT
};