#include "lib/phase_timer.h"

#include <iostream>
#include <map>

namespace mbtime {
namespace {

struct State {
    std::vector<std::pair<std::string, std::uint64_t>> order;
    std::map<std::string, std::size_t> index;
    std::vector<std::uint64_t *> open; // child accumulator of each live scope
};

State &state()
{
    static State s;
    return s;
}

} // namespace

Scope::Scope(const char *phase)
    : phase_(phase), start_(std::chrono::steady_clock::now()), childNs_(0)
{
    state().open.push_back(&childNs_);
}

Scope::~Scope()
{
    const auto elapsed = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start_).count());

    State &s = state();
    s.open.pop_back();
    if (!s.open.empty())
        *s.open.back() += elapsed; // the caller owns our whole span

    const std::uint64_t self = elapsed > childNs_ ? elapsed - childNs_ : 0;

    auto it = s.index.find(phase_);
    if (it == s.index.end()) {
        s.index.emplace(phase_, s.order.size());
        s.order.emplace_back(phase_, self);
    } else {
        s.order[it->second].second += self;
    }
}

const std::vector<std::pair<std::string, std::uint64_t>> &phases()
{
    return state().order;
}

void printStats()
{
    for (const auto &entry : phases())
        std::cout << "STATS:time:" << entry.first << "=" << entry.second << "\n";
}

} // namespace mbtime
