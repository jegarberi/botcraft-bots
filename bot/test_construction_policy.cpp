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
static_assert(Construction::PlacementStage(false, "minecraft:powered_rail") == 2 && Construction::PlacementStage(false, "minecraft:rail") == 2, "rails after the blocks they lean on");
static_assert(Construction::PlacementStage(false, "minecraft:lantern") == 2 && Construction::PlacementStage(false, "minecraft:chain") == 1, "a lantern after the chain it hangs from");
static_assert(Construction::PlacementStage(false, "minecraft:stone_button") == 2 && Construction::PlacementStage(false, "minecraft:redstone_wall_torch") == 2);
static_assert(Construction::PlacementStage(false, "minecraft:redstone_block") == 1);
static_assert(Construction::PlacementStage(false, "minecraft:white_bed") == 2);
static_assert(Construction::PlacementStage(false, "minecraft:oak_door") == 2);
static_assert(Construction::PlacementStage(false, "minecraft:oak_fence_gate") == 2);
static_assert(Construction::PlacementStage(false, "minecraft:campfire") == 2);
static_assert(Construction::PlacementStage(false, "minecraft:torch") == 2);
static_assert(Construction::PlacementStage(false, "minecraft:water") == 2);

int main() {}
