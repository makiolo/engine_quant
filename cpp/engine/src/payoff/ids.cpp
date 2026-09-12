#include "engine/payoff/ids.hpp"

#include <atomic>

namespace engine {
namespace payoff {

NodeId allocate_node_id() {
    static std::atomic<std::uint32_t> counter{1};
    return NodeId{counter.fetch_add(1, std::memory_order_relaxed)};
}

} // namespace payoff
} // namespace engine
