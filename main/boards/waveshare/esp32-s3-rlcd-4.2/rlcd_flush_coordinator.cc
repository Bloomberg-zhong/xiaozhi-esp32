#include "rlcd_flush_coordinator.h"

bool RlcdFlushCoordinator::BeginFlush() {
    bool expected = false;
    return flush_in_flight_.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                    std::memory_order_acquire);
}

bool RlcdFlushCoordinator::CompleteFlush() {
    return flush_in_flight_.exchange(false, std::memory_order_acq_rel);
}

bool RlcdFlushCoordinator::IsFlushInFlight() const {
    return flush_in_flight_.load(std::memory_order_acquire);
}
