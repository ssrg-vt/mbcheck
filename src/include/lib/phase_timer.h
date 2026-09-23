#ifndef MBCHECK_PHASE_TIMER_H
#define MBCHECK_PHASE_TIMER_H

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace mbtime {

// Per-stage timing.  Scopes nest, so a scope charges its whole span to its
// parent and keeps only its exclusive time.
class Scope {
public:
    explicit Scope(const char *phase);
    ~Scope();

    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

private:
    const char *phase_;
    std::chrono::steady_clock::time_point start_;
    std::uint64_t childNs_;
};

// Exclusive ns per phase, in first-run order.
const std::vector<std::pair<std::string, std::uint64_t>> &phases();

// Emit STATS:time:<phase>=<ns> per phase.
void printStats();

} // namespace mbtime

#endif // MBCHECK_PHASE_TIMER_H
