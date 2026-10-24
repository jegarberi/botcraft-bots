#include "creative_inventory.hpp"
#include <cassert>
#include <map>

struct Slot
{
    int item, count;
    bool customized = false;
    bool IsEmptySlot() const { return count == 0; }
    int GetItemId() const { return item; }
    int GetItemCount() const { return count; }
};
int main()
{
    std::map<short, Slot> slots;
    const auto reusable = [](const Slot& slot) { return !slot.customized; };
    assert(CreativeInventory::SupplySlot(slots, 1, 64, reusable) == 9);
    for (short i = 9; i < 45; ++i) slots[i] = {2, 64};
    // Before the fix, losing creation history left no replaceable slot after reconnect.
    assert(CreativeInventory::SupplySlot(slots, 1, 64, [](const Slot&) { return false; }) == -1);
    // Plain supplies remain reusable without that history.
    assert(CreativeInventory::SupplySlot(slots, 1, 64, reusable) == 9);
    slots[12] = {1, 16};
    assert(CreativeInventory::SupplySlot(slots, 1, 64, reusable) == 12);
    for (auto& entry : slots) entry.second.customized = true;
    assert(CreativeInventory::SupplySlot(slots, 1, 64, reusable) == -1);
    slots[20] = {2, 64};
    assert(CreativeInventory::SupplySlot(slots, 1, 64, reusable) == 20);
    slots[25] = {0, 0};
    assert(CreativeInventory::SupplySlot(slots, 1, 64, reusable) == 25);
    for (short i = 9; i < 45; ++i) slots[i] = {1, 64};
    assert(CreativeInventory::SupplySlot(slots, 1, 64, reusable) == -1);
}
