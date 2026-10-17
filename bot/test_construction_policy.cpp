#include "construction_policy.hpp"

static_assert(!Construction::CanPursuePlayer(true, 20, 20.0));
static_assert(!Construction::CanPursuePlayer(false, 0, 20.0));
static_assert(!Construction::CanPursuePlayer(false, 20, 11.5));
static_assert(Construction::CanPursuePlayer(false, 8, 12.0));
static_assert(Construction::RetryReady(0, 0));
static_assert(!Construction::RetryReady(1, 59));
static_assert(Construction::RetryReady(1, 60));
static_assert(Construction::RetryReady(8, 60));
static_assert(Construction::RetryReady(100, 60));
static_assert(!Construction::ShouldFinishGatherFirst(false, 16));
static_assert(!Construction::ShouldFinishGatherFirst(true, 0));
static_assert(Construction::ShouldFinishGatherFirst(true, 1));
static_assert(Construction::PlacementStage(true, "minecraft:white_bed") == 0);
static_assert(Construction::PlacementStage(false, "minecraft:cobblestone") == 1);
static_assert(Construction::PlacementStage(false, "minecraft:oak_planks") == 1);
static_assert(Construction::PlacementStage(false, "minecraft:white_bed") == 2);

int main() {}
