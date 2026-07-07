// DBSP Circuit/Dataflow Graph Implementation
// A circuit is a directed graph of compute nodes that process streams of Z-sets

#pragma once

#include "dbsp_stream.hpp"
#include "dbsp_zset.hpp"

#include <any>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace dbsp {

// Forward declarations
class Circuit;
class Node;

using NodeId = uint64_t;

// Base class for all compute nodes in the circuit
class Node {
public:
    explicit Node(NodeId id, std::string name = "")
        : id_(id), name_(std::move(name)) {}

    virtual ~Node() = default;

    NodeId id() const { return id_; }
    const std::string& name() const { return name_; }

    // Process one step of the circuit
    virtual void step() = 0;

    // Reset the node state
    virtual void reset() = 0;

    // Check if this node has output ready
    virtual bool has_output() const = 0;

    // --- Checkpointing (Phase D3b) ---------------------------------------
    // How this node participates in circuit-state checkpoints. STATELESS
    // nodes save/restore nothing; SERIALIZABLE nodes implement the two
    // methods below; any UNSUPPORTED node makes its whole view fall back
    // to rebuild-by-replay on load. Byte-level API keeps the core circuit
    // free of storage dependencies.
    enum class StateKind { STATELESS, SERIALIZABLE, UNSUPPORTED };

    virtual StateKind state_kind() const { return StateKind::UNSUPPORTED; }

    // Append this node's state to `out`. Only called when state_kind() is
    // SERIALIZABLE.
    virtual void serialize_state(std::vector<uint8_t>& out) const {
        (void)out;
    }

    // Restore state saved by serialize_state. Returns false on any
    // mismatch (caller discards the checkpoint and rebuilds).
    virtual bool restore_state(const uint8_t* data, size_t len) {
        (void)data;
        (void)len;
        return false;
    }

protected:
    NodeId id_;
    std::string name_;
};

// Source node: receives external input
template <typename T, typename Hash = std::hash<T>>
class SourceNode : public Node {
public:
    using ZSetType = ZSet<T, Hash>;

    SourceNode(NodeId id, std::string name = "source")
        : Node(id, std::move(name)) {}

    // Checkpoints are taken quiescent (no pending push), so a source
    // carries no durable state.
    StateKind state_kind() const override { return StateKind::STATELESS; }

    // Push new data into the source
    void push(const ZSetType& delta) {
        pending_delta_ += delta;
        has_pending_ = true;
    }

    void push(ZSetType&& delta) {
        pending_delta_ += std::move(delta);
        has_pending_ = true;
    }

    // Zero-copy push: borrow the caller's Z-set for the NEXT step only.
    // The caller must keep `delta` alive until that step() returns (true
    // for the synchronous apply_changes -> circuit.step() flow). Mixing
    // with push()/insert() in the same step falls back to a merge copy.
    void push_borrowed(const ZSetType& delta) {
        borrowed_ = &delta;
        has_pending_ = true;
    }

    // Insert a single element
    void insert(const T& elem, Weight weight = 1) {
        pending_delta_.insert(elem, weight);
        has_pending_ = true;
    }

    void step() override {
        if (has_pending_) {
            if (borrowed_ && pending_delta_.empty()) {
                external_output_ = borrowed_;
            } else {
                if (borrowed_) {
                    pending_delta_ += *borrowed_;
                }
                current_output_ = std::move(pending_delta_);
                external_output_ = nullptr;
            }
            borrowed_ = nullptr;
            pending_delta_.clear();
            has_pending_ = false;
            has_output_ = true;
        } else {
            current_output_.clear();
            external_output_ = nullptr;
            has_output_ = false;
        }
    }

    void reset() override {
        pending_delta_.clear();
        current_output_.clear();
        borrowed_ = nullptr;
        external_output_ = nullptr;
        has_pending_ = false;
        has_output_ = false;
    }

    bool has_output() const override { return has_output_; }

    const ZSetType& output() const {
        return external_output_ ? *external_output_ : current_output_;
    }

private:
    ZSetType pending_delta_;
    ZSetType current_output_;
    const ZSetType* borrowed_ = nullptr;        // valid during one step
    const ZSetType* external_output_ = nullptr; // set by step() from borrowed_
    bool has_pending_ = false;
    bool has_output_ = false;
};

// Sink node: collects output from the circuit
template <typename T, typename Hash = std::hash<T>>
class SinkNode : public Node {
public:
    using ZSetType = ZSet<T, Hash>;
    using InputFn = std::function<const ZSetType&()>;

    SinkNode(NodeId id, InputFn input_fn, std::string name = "sink")
        : Node(id, std::move(name)), input_fn_(std::move(input_fn)) {}

    // The integrated result is checkpointed at the VIEW level (the view's
    // get_result/set_result already round-trips it), so the sink itself
    // reports stateless to the circuit walk.
    StateKind state_kind() const override { return StateKind::STATELESS; }

    void step() override {
        // Accumulate the input delta into our integrated state. The delta
        // is borrowed, not copied: upstream node outputs stay alive until
        // that node's next step(), which cannot happen before ours.
        const ZSetType& delta = input_fn_();
        if (!delta.empty()) {
            delta_ref_ = &delta;
            integrated_ += delta;
            has_output_ = true;
        } else {
            delta_ref_ = nullptr;
            has_output_ = false;
        }
    }

    void reset() override {
        integrated_.clear();
        delta_ref_ = nullptr;
        has_output_ = false;
    }

    bool has_output() const override { return has_output_; }

    // Get the latest delta (borrowed from the upstream node)
    const ZSetType& delta() const {
        static const ZSetType kEmpty;
        return delta_ref_ ? *delta_ref_ : kEmpty;
    }

    // Get the integrated (materialized) result
    const ZSetType& materialized() const { return integrated_; }

    // Overwrite the integrated state (used by tests/legacy restore paths)
    void set_materialized(const ZSetType& state) {
        integrated_ = state;
        delta_ref_ = nullptr;
        has_output_ = false;
    }

private:
    InputFn input_fn_;
    ZSetType integrated_;
    const ZSetType* delta_ref_ = nullptr;
    bool has_output_ = false;
};

// Map node: applies a transformation function
template <typename InputT, typename OutputT,
          typename InputHash = std::hash<InputT>,
          typename OutputHash = std::hash<OutputT>>
class MapNode : public Node {
public:
    using InputZSet = ZSet<InputT, InputHash>;
    using OutputZSet = ZSet<OutputT, OutputHash>;
    using InputFn = std::function<const InputZSet&()>;
    using TransformFn = std::function<OutputT(const InputT&)>;

    MapNode(NodeId id, InputFn input_fn, TransformFn transform_fn, std::string name = "map")
        : Node(id, std::move(name))
        , input_fn_(std::move(input_fn))
        , transform_fn_(std::move(transform_fn)) {}

    void step() override {
        const InputZSet& input = input_fn_();
        output_ = input.template map<OutputT, OutputHash>(transform_fn_);
        has_output_ = !output_.empty();
    }

    void reset() override {
        output_.clear();
        has_output_ = false;
    }

    bool has_output() const override { return has_output_; }

    const OutputZSet& output() const { return output_; }

private:
    InputFn input_fn_;
    TransformFn transform_fn_;
    OutputZSet output_;
    bool has_output_ = false;
};

// Filter node: filters elements by predicate
template <typename T, typename Hash = std::hash<T>>
class FilterNode : public Node {
public:
    using ZSetType = ZSet<T, Hash>;
    using InputFn = std::function<const ZSetType&()>;
    using PredicateFn = std::function<bool(const T&)>;

    FilterNode(NodeId id, InputFn input_fn, PredicateFn predicate_fn, std::string name = "filter")
        : Node(id, std::move(name))
        , input_fn_(std::move(input_fn))
        , predicate_fn_(std::move(predicate_fn)) {}

    void step() override {
        const ZSetType& input = input_fn_();
        output_ = input.filter(predicate_fn_);
        has_output_ = !output_.empty();
    }

    void reset() override {
        output_.clear();
        has_output_ = false;
    }

    bool has_output() const override { return has_output_; }

    const ZSetType& output() const { return output_; }

private:
    InputFn input_fn_;
    PredicateFn predicate_fn_;
    ZSetType output_;
    bool has_output_ = false;
};

// Incremental Join node
template <typename A, typename B, typename K,
          typename AHash = std::hash<A>,
          typename BHash = std::hash<B>,
          typename KHash = std::hash<K>>
class JoinNode : public Node {
public:
    using ZSetA = ZSet<A, AHash>;
    using ZSetB = ZSet<B, BHash>;
    using ZSetResult = ZSet<std::pair<A, B>>;
    using InputFnA = std::function<const ZSetA&()>;
    using InputFnB = std::function<const ZSetB&()>;
    using KeyFnA = std::function<K(const A&)>;
    using KeyFnB = std::function<K(const B&)>;

    JoinNode(NodeId id, InputFnA input_fn_a, InputFnB input_fn_b,
             KeyFnA key_fn_a, KeyFnB key_fn_b, std::string name = "join")
        : Node(id, std::move(name))
        , input_fn_a_(std::move(input_fn_a))
        , input_fn_b_(std::move(input_fn_b))
        , join_op_(std::move(key_fn_a), std::move(key_fn_b)) {}

    void step() override {
        const ZSetA& delta_a = input_fn_a_();
        const ZSetB& delta_b = input_fn_b_();
        output_ = join_op_.process(delta_a, delta_b);
        has_output_ = !output_.empty();
    }

    void reset() override {
        join_op_.reset();
        output_.clear();
        has_output_ = false;
    }

    bool has_output() const override { return has_output_; }

    const ZSetResult& output() const { return output_; }

private:
    InputFnA input_fn_a_;
    InputFnB input_fn_b_;
    IncrementalJoin<A, B, K, AHash, BHash, KHash> join_op_;
    ZSetResult output_;
    bool has_output_ = false;
};

// Distinct node: maintains incremental distinct
template <typename T, typename Hash = std::hash<T>>
class DistinctNode : public Node {
public:
    using ZSetType = ZSet<T, Hash>;
    using InputFn = std::function<const ZSetType&()>;

    DistinctNode(NodeId id, InputFn input_fn, std::string name = "distinct")
        : Node(id, std::move(name))
        , input_fn_(std::move(input_fn)) {}

    void step() override {
        const ZSetType& input = input_fn_();
        output_ = distinct_op_.process(input);
        has_output_ = !output_.empty();
    }

    void reset() override {
        distinct_op_.reset();
        output_.clear();
        has_output_ = false;
    }

    bool has_output() const override { return has_output_; }

    const ZSetType& output() const { return output_; }

private:
    InputFn input_fn_;
    IncrementalDistinct<T, Hash> distinct_op_;
    ZSetType output_;
    bool has_output_ = false;
};

// Aggregate node: maintains incremental aggregation
template <typename T, typename K, typename V,
          typename THash = std::hash<T>,
          typename KHash = std::hash<K>>
class AggregateNode : public Node {
public:
    using InputZSet = ZSet<T, THash>;
    using OutputZSet = ZSet<std::pair<K, V>>;
    using InputFn = std::function<const InputZSet&()>;
    using KeyFn = std::function<K(const T&)>;
    using ValueFn = std::function<V(const T&)>;

    enum class AggType { SUM, COUNT };

    AggregateNode(NodeId id, InputFn input_fn, KeyFn key_fn, ValueFn value_fn,
                  AggType agg_type, std::string name = "aggregate")
        : Node(id, std::move(name))
        , input_fn_(std::move(input_fn))
        , agg_op_(std::move(key_fn), std::move(value_fn))
        , agg_type_(agg_type) {}

    void step() override {
        const InputZSet& input = input_fn_();
        switch (agg_type_) {
            case AggType::SUM:
                output_ = agg_op_.process_sum(input);
                break;
            case AggType::COUNT:
                output_ = agg_op_.process_count(input);
                break;
        }
        has_output_ = !output_.empty();
    }

    void reset() override {
        agg_op_.reset();
        output_.clear();
        has_output_ = false;
    }

    bool has_output() const override { return has_output_; }

    const OutputZSet& output() const { return output_; }

private:
    InputFn input_fn_;
    IncrementalAggregate<T, K, V, THash, KHash> agg_op_;
    AggType agg_type_;
    OutputZSet output_;
    bool has_output_ = false;
};

// Union node: combines two streams
template <typename T, typename Hash = std::hash<T>>
class UnionNode : public Node {
public:
    using ZSetType = ZSet<T, Hash>;
    using InputFn = std::function<const ZSetType&()>;

    UnionNode(NodeId id, InputFn input_fn_1, InputFn input_fn_2, std::string name = "union")
        : Node(id, std::move(name))
        , input_fn_1_(std::move(input_fn_1))
        , input_fn_2_(std::move(input_fn_2)) {}

    void step() override {
        output_ = input_fn_1_() + input_fn_2_();
        has_output_ = !output_.empty();
    }

    void reset() override {
        output_.clear();
        has_output_ = false;
    }

    bool has_output() const override { return has_output_; }

    const ZSetType& output() const { return output_; }

private:
    InputFn input_fn_1_;
    InputFn input_fn_2_;
    ZSetType output_;
    bool has_output_ = false;
};

// Integration node: maintains running sum
template <typename T, typename Hash = std::hash<T>>
class IntegrationNode : public Node {
public:
    using ZSetType = ZSet<T, Hash>;
    using InputFn = std::function<const ZSetType&()>;

    IntegrationNode(NodeId id, InputFn input_fn, std::string name = "integrate")
        : Node(id, std::move(name))
        , input_fn_(std::move(input_fn)) {}

    void step() override {
        const ZSetType& delta = input_fn_();
        integrated_ += delta;
        has_output_ = !delta.empty();
    }

    void reset() override {
        integrated_.clear();
        has_output_ = false;
    }

    bool has_output() const override { return has_output_; }

    const ZSetType& output() const { return integrated_; }

private:
    InputFn input_fn_;
    ZSetType integrated_;
    bool has_output_ = false;
};

// The Circuit class manages a collection of nodes and executes them
class Circuit {
public:
    Circuit() = default;

    // Add a node to the circuit
    template <typename NodeType>
    NodeType* add_node(std::unique_ptr<NodeType> node) {
        NodeType* ptr = node.get();
        nodes_by_id_[node->id()] = nodes_.size();
        nodes_.push_back(std::move(node));
        return ptr;
    }

    // Execute one step of the circuit
    void step() {
        for (auto& node : nodes_) {
            node->step();
        }
        ++time_;
    }

    // Reset all nodes
    void reset() {
        for (auto& node : nodes_) {
            node->reset();
        }
        time_ = 0;
    }

    // Get current time
    uint64_t time() const { return time_; }

    // Get node by id
    Node* get_node(NodeId id) {
        auto it = nodes_by_id_.find(id);
        if (it != nodes_by_id_.end()) {
            return nodes_[it->second].get();
        }
        return nullptr;
    }

    // Get number of nodes
    size_t node_count() const { return nodes_.size(); }

    // Visit every node in insertion order (checkpointing walks the circuit)
    template <typename Fn>
    void for_each_node(Fn fn) {
        for (auto& node : nodes_) {
            fn(*node);
        }
    }
    template <typename Fn>
    void for_each_node(Fn fn) const {
        for (const auto& node : nodes_) {
            fn(*node);
        }
    }

    // Generate a new unique node ID
    NodeId next_node_id() { return next_id_++; }

private:
    std::vector<std::unique_ptr<Node>> nodes_;
    std::unordered_map<NodeId, size_t> nodes_by_id_;
    uint64_t time_ = 0;
    NodeId next_id_ = 0;
};

} // namespace dbsp
