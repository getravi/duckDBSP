// DBSP Stream Operators Implementation for DuckDB Extension
// Implements the core stream operators: Integration (I), Differentiation (D), and Delay (z^-1)

#pragma once

#include "dbsp_zset.hpp"
#include <functional>
#include <memory>
#include <optional>
#include <queue>
#include <vector>

namespace dbsp {

// A stream is conceptually an infinite sequence of Z-sets indexed by time.
// In practice, we process streams incrementally, one time step at a time.

// StreamState holds the state needed for stream operations
template <typename T, typename Hash = std::hash<T>>
class StreamState {
public:
    using ZSetType = ZSet<T, Hash>;

    StreamState() = default;

    // Get the current integrated value (running sum)
    const ZSetType& integrated() const { return integrated_; }

    // Get the previous value (for delay)
    const ZSetType& delayed() const { return delayed_; }

    // Advance the stream by one time step with a new delta
    void step(const ZSetType& delta) {
        delayed_ = integrated_;
        integrated_ += delta;
        ++time_;
    }

    // Get the current time
    uint64_t time() const { return time_; }

    // Reset to initial state
    void reset() {
        integrated_.clear();
        delayed_.clear();
        time_ = 0;
    }

private:
    ZSetType integrated_;  // I(s)[t] = sum of s[0..t]
    ZSetType delayed_;     // z^-1(I(s))[t] = I(s)[t-1]
    uint64_t time_ = 0;
};

// Integration operator: I(s)[t] = sum_{i<=t} s[i]
// Returns the running sum of Z-sets
template <typename T, typename Hash = std::hash<T>>
class Integration {
public:
    using ZSetType = ZSet<T, Hash>;

    // Process a new delta and return the integrated result
    ZSetType process(const ZSetType& delta) {
        state_ += delta;
        return state_;
    }

    // Get current state without adding new delta
    const ZSetType& current() const { return state_; }

    // Reset
    void reset() { state_.clear(); }

private:
    ZSetType state_;
};

// Differentiation operator: D(s)[t] = s[t] - s[t-1]
// In the incremental setting, if input is already a delta stream, D just returns the input
template <typename T, typename Hash = std::hash<T>>
class Differentiation {
public:
    using ZSetType = ZSet<T, Hash>;

    // Process a new value and return the difference from previous
    ZSetType process(const ZSetType& current) {
        ZSetType result = current - previous_;
        previous_ = current;
        return result;
    }

    // Reset
    void reset() { previous_.clear(); }

private:
    ZSetType previous_;
};

// Delay operator: z^-1(s)[t] = s[t-1] (0 at t=0)
template <typename T, typename Hash = std::hash<T>>
class Delay {
public:
    using ZSetType = ZSet<T, Hash>;

    // Process a new value and return the previous value
    ZSetType process(const ZSetType& current) {
        ZSetType result = std::move(previous_);
        previous_ = current;
        return result;
    }

    // Get the delayed value without consuming
    const ZSetType& peek() const { return previous_; }

    // Reset
    void reset() { previous_.clear(); }

private:
    ZSetType previous_;
};

// Incremental operator wrapper: Q^Δ = D ∘ Q ∘ I
// Takes deltas as input, produces deltas as output
template <typename InputT, typename OutputT,
          typename InputHash = std::hash<InputT>,
          typename OutputHash = std::hash<OutputT>>
class IncrementalOperator {
public:
    using InputZSet = ZSet<InputT, InputHash>;
    using OutputZSet = ZSet<OutputT, OutputHash>;
    using OperatorFn = std::function<OutputZSet(const InputZSet&)>;

    explicit IncrementalOperator(OperatorFn op) : op_(std::move(op)) {}

    // Process an input delta and return the output delta
    OutputZSet process(const InputZSet& input_delta) {
        // I: Integrate the input delta
        input_integrated_ += input_delta;

        // Q: Apply the operator to the integrated input
        OutputZSet output_value = op_(input_integrated_);

        // D: Differentiate the output
        OutputZSet output_delta = output_value - output_previous_;
        output_previous_ = std::move(output_value);

        return output_delta;
    }

    // Reset
    void reset() {
        input_integrated_.clear();
        output_previous_.clear();
    }

private:
    OperatorFn op_;
    InputZSet input_integrated_;
    OutputZSet output_previous_;
};

// Lifted unary operator: applies a function pointwise at each time step
template <typename InputT, typename OutputT,
          typename InputHash = std::hash<InputT>,
          typename OutputHash = std::hash<OutputT>>
class LiftedUnaryOperator {
public:
    using InputZSet = ZSet<InputT, InputHash>;
    using OutputZSet = ZSet<OutputT, OutputHash>;
    using TransformFn = std::function<OutputZSet(const InputZSet&)>;

    explicit LiftedUnaryOperator(TransformFn fn) : fn_(std::move(fn)) {}

    OutputZSet process(const InputZSet& input) {
        return fn_(input);
    }

private:
    TransformFn fn_;
};

// Bilinear operator incrementalization
// For bilinear T: T^Δ2(a, b) = a×b + I(z^-1(a))×b + a×I(z^-1(b))
template <typename A, typename B, typename C,
          typename AHash = std::hash<A>,
          typename BHash = std::hash<B>,
          typename CHash = std::hash<C>>
class BilinearIncremental {
public:
    using ZSetA = ZSet<A, AHash>;
    using ZSetB = ZSet<B, BHash>;
    using ZSetC = ZSet<C, CHash>;
    using BilinearFn = std::function<ZSetC(const ZSetA&, const ZSetB&)>;

    explicit BilinearIncremental(BilinearFn fn) : fn_(std::move(fn)) {}

    // Process incremental updates to both inputs
    ZSetC process(const ZSetA& delta_a, const ZSetB& delta_b) {
        // Compute the three terms of the bilinear incremental formula:
        // T^Δ2(a, b) = a×b + I(z^-1(a))×b + a×I(z^-1(b))

        // Term 1: delta_a × delta_b
        ZSetC result = fn_(delta_a, delta_b);

        // Term 2: I(z^-1(a)) × delta_b = prev_integrated_a × delta_b
        result += fn_(prev_integrated_a_, delta_b);

        // Term 3: delta_a × I(z^-1(b)) = delta_a × prev_integrated_b
        result += fn_(delta_a, prev_integrated_b_);

        // Update the integrated states for next iteration
        prev_integrated_a_ += delta_a;
        prev_integrated_b_ += delta_b;

        return result;
    }

    // Reset
    void reset() {
        prev_integrated_a_.clear();
        prev_integrated_b_.clear();
    }

private:
    BilinearFn fn_;
    ZSetA prev_integrated_a_;  // I(z^-1(a))
    ZSetB prev_integrated_b_;  // I(z^-1(b))
};

// Incremental Join operator
// Uses the bilinear incremental formula since join is bilinear
template <typename A, typename B, typename K,
          typename AHash = std::hash<A>,
          typename BHash = std::hash<B>,
          typename KHash = std::hash<K>>
class IncrementalJoin {
public:
    using ZSetA = ZSet<A, AHash>;
    using ZSetB = ZSet<B, BHash>;
    using ZSetResult = ZSet<std::pair<A, B>>;
    using KeyFnA = std::function<K(const A&)>;
    using KeyFnB = std::function<K(const B&)>;

    IncrementalJoin(KeyFnA key_a, KeyFnB key_b)
        : key_a_(std::move(key_a)), key_b_(std::move(key_b)) {}

    // Process incremental updates
    ZSetResult process(const ZSetA& delta_a, const ZSetB& delta_b) {
        ZSetResult result;

        // Index delta_b by key for efficient joining
        IndexedZSet<K, B, KHash> indexed_delta_b;
        for (const auto& [b, wb] : delta_b) {
            indexed_delta_b.insert(key_b_(b), b, wb);
        }

        // Index delta_a by key
        IndexedZSet<K, A, KHash> indexed_delta_a;
        for (const auto& [a, wa] : delta_a) {
            indexed_delta_a.insert(key_a_(a), a, wa);
        }

        // Term 1: delta_a ⋈ delta_b
        for (const auto& [a, wa] : delta_a) {
            K k = key_a_(a);
            for (const auto& [b, wb] : indexed_delta_b[k]) {
                result.insert(std::make_pair(a, b), wa * wb);
            }
        }

        // Term 2: integrated_a ⋈ delta_b
        for (const auto& [k, zset_a] : indexed_a_) {
            for (const auto& [a, wa] : zset_a) {
                for (const auto& [b, wb] : indexed_delta_b[k]) {
                    result.insert(std::make_pair(a, b), wa * wb);
                }
            }
        }

        // Term 3: delta_a ⋈ integrated_b
        for (const auto& [a, wa] : delta_a) {
            K k = key_a_(a);
            for (const auto& [b, wb] : indexed_b_[k]) {
                result.insert(std::make_pair(a, b), wa * wb);
            }
        }

        // Update integrated states
        for (const auto& [a, wa] : delta_a) {
            indexed_a_.insert(key_a_(a), a, wa);
        }
        for (const auto& [b, wb] : delta_b) {
            indexed_b_.insert(key_b_(b), b, wb);
        }

        return result;
    }

    // Reset
    void reset() {
        indexed_a_.clear();
        indexed_b_.clear();
    }

private:
    KeyFnA key_a_;
    KeyFnB key_b_;
    IndexedZSet<K, A, KHash> indexed_a_;  // Indexed integrated A
    IndexedZSet<K, B, KHash> indexed_b_;  // Indexed integrated B
};

// Incremental Distinct operator
// distinct^Δ needs special handling since distinct is not linear
template <typename T, typename Hash = std::hash<T>>
class IncrementalDistinct {
public:
    using ZSetType = ZSet<T, Hash>;

    // Process a delta and return the delta to the distinct result
    ZSetType process(const ZSetType& delta) {
        ZSetType result;

        for (const auto& [elem, weight] : delta) {
            Weight old_weight = integrated_[elem];
            Weight new_weight = old_weight + weight;

            // Update integrated state
            integrated_.insert(elem, weight);

            // Compute change in distinct output
            bool was_positive = old_weight > 0;
            bool is_positive = new_weight > 0;

            if (!was_positive && is_positive) {
                // Element became positive: add to output
                result.insert(elem, 1);
            } else if (was_positive && !is_positive) {
                // Element became non-positive: remove from output
                result.insert(elem, -1);
            }
            // If both positive or both non-positive, no change to distinct output
        }

        return result;
    }

    // Reset
    void reset() { integrated_.clear(); }

private:
    ZSetType integrated_;
};

// Incremental Aggregation (for sum/count)
template <typename T, typename K, typename V,
          typename THash = std::hash<T>,
          typename KHash = std::hash<K>>
class IncrementalAggregate {
public:
    using InputZSet = ZSet<T, THash>;
    using OutputZSet = ZSet<std::pair<K, V>>;
    using KeyFn = std::function<K(const T&)>;
    using ValueFn = std::function<V(const T&)>;

    IncrementalAggregate(KeyFn key_fn, ValueFn value_fn)
        : key_fn_(std::move(key_fn)), value_fn_(std::move(value_fn)) {}

    // For SUM aggregation, the delta is linear:
    // When a new row (k, v) with weight w arrives, the sum for key k changes by v * w
    OutputZSet process_sum(const InputZSet& delta) {
        OutputZSet result;

        // Group changes by key
        std::unordered_map<K, V, KHash> changes;
        for (const auto& [elem, weight] : delta) {
            K k = key_fn_(elem);
            V v = value_fn_(elem);
            changes[k] += v * static_cast<V>(weight);
        }

        // Emit changes
        for (const auto& [k, delta_v] : changes) {
            if (delta_v != V{}) {
                V old_sum = sums_[k];
                V new_sum = old_sum + delta_v;
                sums_[k] = new_sum;

                // Output the change: remove old, add new
                if (old_sum != V{}) {
                    result.insert(std::make_pair(k, old_sum), -1);
                }
                if (new_sum != V{}) {
                    result.insert(std::make_pair(k, new_sum), 1);
                }
            }
        }

        return result;
    }

    // For COUNT aggregation
    OutputZSet process_count(const InputZSet& delta) {
        OutputZSet result;

        // Group changes by key
        std::unordered_map<K, Weight, KHash> changes;
        for (const auto& [elem, weight] : delta) {
            K k = key_fn_(elem);
            changes[k] += weight;
        }

        // Emit changes
        for (const auto& [k, delta_count] : changes) {
            if (delta_count != 0) {
                V old_count = static_cast<V>(counts_[k]);
                V new_count = old_count + static_cast<V>(delta_count);
                counts_[k] = static_cast<Weight>(new_count);

                // Output the change: remove old, add new
                result.insert(std::make_pair(k, old_count), -1);
                result.insert(std::make_pair(k, new_count), 1);
            }
        }

        return result;
    }

    void reset() {
        sums_.clear();
        counts_.clear();
    }

private:
    KeyFn key_fn_;
    ValueFn value_fn_;
    std::unordered_map<K, V, KHash> sums_;
    std::unordered_map<K, Weight, KHash> counts_;
};

} // namespace dbsp
