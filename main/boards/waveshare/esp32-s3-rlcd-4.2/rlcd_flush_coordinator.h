#ifndef RLCD_FLUSH_COORDINATOR_H
#define RLCD_FLUSH_COORDINATOR_H

#include <atomic>

class RlcdFlushCoordinator {
public:
    bool BeginFlush();
    bool CompleteFlush();
    bool IsFlushInFlight() const;

private:
    std::atomic<bool> flush_in_flight_{false};
};

#endif  // RLCD_FLUSH_COORDINATOR_H
