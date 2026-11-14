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
  * @warning When adding a type, extend TYPE_COLOR[] (render layer) and
  *          Tune::ATTACK[] / Tune::DEFENSE[] at the same position. The Tune
  *          tables are checked by static_assert; TYPE_COLOR is not yet.
  */
enum CellType : uint8_t {
    T_EMPTY = 0, T_HEAD, T_MOVER, T_STORAGE, T_DEFENSE, T_OFFENSE, T_TAIL, T_COUNT
};