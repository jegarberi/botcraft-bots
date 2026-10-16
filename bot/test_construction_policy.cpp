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

int main() {}
