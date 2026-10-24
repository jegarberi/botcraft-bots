#pragma once

namespace CreativeInventory
{
    // Empty slots first, then top up a plain partial stack, then reuse replaceable supplies.
    template <typename Slots, typename ItemId, typename Reusable>
    short SupplySlot(const Slots& slots, const ItemId item, const int stack_size, const Reusable& reusable)
    {
        for (short index = 9; index < 45; ++index)
        {
            const auto slot = slots.find(index);
            if (slot == slots.end() || slot->second.IsEmptySlot()) return index;
        }
        for (short index = 9; index < 45; ++index)
        {
            const auto& slot = slots.at(index);
            if (slot.GetItemId() == item && slot.GetItemCount() < stack_size && reusable(slot)) return index;
        }
        for (short index = 9; index < 45; ++index)
        {
            const auto& slot = slots.at(index);
            if (slot.GetItemId() != item && reusable(slot)) return index;
        }
        return -1;
    }
}
