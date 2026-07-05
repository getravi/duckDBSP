// DBSP Z-Set Implementation for DuckDB Extension
// A Z-set is a generalization of multisets that allows integer multiplicities.
// Positive weights represent insertions, negative weights represent deletions.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dbsp {

// Generic pair hash functor for use with unordered containers
template <typename T1, typename T2>
struct PairHash {
    size_t operator()(const std::pair<T1, T2>& p) const noexcept {
        auto h1 = std::hash<T1>{}(p.first);
        auto h2 = std::hash<T2>{}(p.second);
        return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
    }
};

// Forward declarations
template <typename T, typename Hash = std::hash<T>>
class ZSet;

// Dense weight map (H3): live entries stay contiguous in a vector (so
// iteration — the hottest Z-set operation — is a cache-friendly O(size)
// array walk) with an open-addressing index of (hash -> entry position)
// on the side. clear() is O(1) via generation stamps; erase is
// swap-remove plus backward-shift index repair. Replaces
// std::unordered_map as Z-set storage: no per-entry node allocations and
// no O(bucket_count) iteration of mostly-empty tables after a set shrinks.
// Key hashes are cheap to recompute (DuckDBRow caches its hash, G1).
template <typename T, typename Hash>
class FlatWeightMap {
public:
    // Named first/second so structured bindings and ->second match the
    // std::unordered_map value_type this replaces
    struct Slot {
        T first;
        int64_t second;
    };
    using const_iterator = typename std::vector<Slot>::const_iterator;

    FlatWeightMap() = default;

    size_t size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }

    void clear() {
        entries_.clear(); // keeps capacity
        generation_++;    // O(1): every index slot becomes stale
    }

    const_iterator begin() const { return entries_.begin(); }
    const_iterator end() const { return entries_.end(); }

    const_iterator find(const T &key) const {
        if (entries_.empty()) {
            return entries_.end();
        }
        const size_t i = probe(key);
        if (!live(i)) {
            return entries_.end();
        }
        return entries_.begin() +
               static_cast<std::ptrdiff_t>(index_[i].pos);
    }

    size_t count(const T &key) const {
        return !entries_.empty() && live(probe(key)) ? 1 : 0;
    }

    int64_t &operator[](const T &key) { return slot_for(key, nullptr); }
    int64_t &operator[](T &&key) { return slot_for(key, &key); }

    void erase(const T &key) {
        if (entries_.empty()) {
            return;
        }
        const size_t i = probe(key);
        if (!live(i)) {
            return;
        }
        const size_t pos = index_[i].pos;
        // Remove from the index with backward-shift to keep chains gapless
        index_[i].gen = 0;
        size_t gap = i;
        size_t k = (i + 1) & mask_;
        while (live(k)) {
            const size_t ideal = hasher_(entries_[index_[k].pos].first) & mask_;
            if (((k - ideal) & mask_) >= ((k - gap) & mask_)) {
                index_[gap] = index_[k];
                index_[k].gen = 0;
                gap = k;
            }
            k = (k + 1) & mask_;
        }
        // Swap-remove from the dense array; repair the moved entry's index
        // slot by POSITION (pos == last), not by key equality — the
        // moved-from slot at entries_[last] is in a valid-but-unspecified
        // state until pop_back, and key-probing past it is fragile
        const size_t last = entries_.size() - 1;
        if (pos != last) {
            entries_[pos] = std::move(entries_[last]);
            size_t j = hasher_(entries_[pos].first) & mask_;
            while (!(live(j) && index_[j].pos == last)) {
                j = (j + 1) & mask_;
            }
            index_[j].pos = pos;
        }
        entries_.pop_back();
    }

private:
    struct IndexSlot {
        size_t pos = 0;
        uint64_t gen = 0; // matches generation_ when live
    };

    bool live(size_t i) const { return index_[i].gen == generation_; }

    // First index slot whose entry's key equals `key`, or the first stale
    // slot in its probe chain
    size_t probe(const T &key) const {
        size_t i = hasher_(key) & mask_;
        while (live(i) && !(entries_[index_[i].pos].first == key)) {
            i = (i + 1) & mask_;
        }
        return i;
    }

    int64_t &slot_for(const T &key, T *movable) {
        if (capacity_ == 0 ||
            (entries_.size() + 1) * 10 >= capacity_ * 7) { // 0.7 load
            rehash(capacity_ == 0 ? 16 : capacity_ * 2);
        }
        const size_t i = probe(key);
        if (live(i)) {
            return entries_[index_[i].pos].second;
        }
        index_[i].pos = entries_.size();
        index_[i].gen = generation_;
        entries_.push_back(
            movable ? Slot{std::move(*movable), 0} : Slot{key, 0});
        return entries_.back().second;
    }

    void rehash(size_t new_cap) {
        capacity_ = new_cap;
        mask_ = new_cap - 1;
        generation_++;
        index_.assign(new_cap, IndexSlot{});
        for (size_t pos = 0; pos < entries_.size(); pos++) {
            size_t i = hasher_(entries_[pos].first) & mask_;
            while (live(i)) {
                i = (i + 1) & mask_;
            }
            index_[i].pos = pos;
            index_[i].gen = generation_;
        }
    }

    std::vector<Slot> entries_;
    std::vector<IndexSlot> index_;
    size_t capacity_ = 0;
    size_t mask_ = 0;
    uint64_t generation_ = 1; // index starts empty (slots default gen 0)
    Hash hasher_;
};

// Weight type - using int64_t to handle large multiplicities
using Weight = int64_t;

// A Z-set is a function from T to Z with finite support.
// It maps elements to their integer weights (multiplicities).
// Positive weight = insertion, negative weight = deletion, zero = not present.
template <typename T, typename Hash>
class ZSet {
public:
    using ElementType = T;
    using MapType = FlatWeightMap<T, Hash>;
    using Iterator = typename MapType::const_iterator;
    using iterator = Iterator;
    using const_iterator = Iterator;

    ZSet() = default;
    ZSet(const ZSet&) = default;
    ZSet(ZSet&&) = default;
    ZSet& operator=(const ZSet&) = default;
    ZSet& operator=(ZSet&&) = default;

    // Get the weight of an element (0 if not present)
    Weight operator[](const T& elem) const {
        auto it = data_.find(elem);
        return it != data_.end() ? it->second : 0;
    }

    // Insert an element with a given weight
    void insert(const T& elem, Weight weight = 1) {
        if (weight == 0) return;
        auto& w = data_[elem];
        w += weight;
        if (w == 0) {
            data_.erase(elem);
        }
    }

    // Insert an element (move version)
    void insert(T&& elem, Weight weight = 1) {
        if (weight == 0) return;
        auto& w = data_[elem];
        w += weight;
        if (w == 0) {
            data_.erase(elem);
        }
    }

    // Check if element has non-zero weight
    bool contains(const T& elem) const {
        return data_.count(elem) > 0;
    }

    // Get the weight of an element (0 if not present); alias of operator[]
    Weight get(const T& elem) const { return (*this)[elem]; }

    // Get the support (elements with non-zero weight)
    size_t support_size() const { return data_.size(); }

    // Alias of support_size(), for container-style callers
    size_t size() const { return data_.size(); }

    // Check if the Z-set is empty (all weights are zero)
    bool empty() const { return data_.empty(); }

    // Clear the Z-set
    void clear() { data_.clear(); }

    // Iteration over elements with non-zero weights
    Iterator begin() const { return data_.begin(); }
    Iterator end() const { return data_.end(); }

    // Z-set arithmetic: addition (pointwise)
    ZSet operator+(const ZSet& other) const {
        ZSet result = *this;
        for (const auto& [elem, weight] : other.data_) {
            result.insert(elem, weight);
        }
        return result;
    }

    ZSet& operator+=(const ZSet& other) {
        for (const auto& [elem, weight] : other.data_) {
            insert(elem, weight);
        }
        return *this;
    }

    // Z-set arithmetic: subtraction (pointwise)
    ZSet operator-(const ZSet& other) const {
        ZSet result = *this;
        for (const auto& [elem, weight] : other.data_) {
            result.insert(elem, -weight);
        }
        return result;
    }

    ZSet& operator-=(const ZSet& other) {
        for (const auto& [elem, weight] : other.data_) {
            insert(elem, -weight);
        }
        return *this;
    }

    // Negation
    ZSet operator-() const {
        ZSet result;
        for (const auto& [elem, weight] : data_) {
            result.data_[elem] = -weight;
        }
        return result;
    }

    // Check if this is a positive Z-set (all weights >= 0)
    bool is_positive() const {
        for (const auto& [elem, weight] : data_) {
            if (weight < 0) return false;
        }
        return true;
    }

    // Check if this is a set (all weights are 0 or 1)
    bool is_set() const {
        for (const auto& [elem, weight] : data_) {
            if (weight != 1) return false;
        }
        return true;
    }

    // Distinct operation: convert to set (weight > 0 -> 1, else 0)
    ZSet distinct() const {
        ZSet result;
        for (const auto& [elem, weight] : data_) {
            if (weight > 0) {
                result.data_[elem] = 1;
            }
        }
        return result;
    }

    // Map operation: apply function f to each element
    template <typename U, typename UHash = std::hash<U>>
    ZSet<U, UHash> map(std::function<U(const T&)> f) const {
        ZSet<U, UHash> result;
        for (const auto& [elem, weight] : data_) {
            result.insert(f(elem), weight);
        }
        return result;
    }

    // Filter operation: keep elements satisfying predicate p
    ZSet filter(std::function<bool(const T&)> p) const {
        ZSet result;
        for (const auto& [elem, weight] : data_) {
            if (p(elem)) {
                result.data_[elem] = weight;
            }
        }
        return result;
    }

    // FlatMap operation: for each element, produce a Z-set and sum the results
    template <typename U, typename UHash = std::hash<U>>
    ZSet<U, UHash> flatmap(std::function<ZSet<U, UHash>(const T&)> f) const {
        ZSet<U, UHash> result;
        for (const auto& [elem, weight] : data_) {
            auto mapped = f(elem);
            for (const auto& [u, w] : mapped) {
                result.insert(u, weight * w);
            }
        }
        return result;
    }

    // Count: sum of all weights
    Weight count() const {
        Weight total = 0;
        for (const auto& [elem, weight] : data_) {
            total += weight;
        }
        return total;
    }

    // Raw data access
    const MapType& data() const { return data_; }
    MapType& data() { return data_; }

private:
    MapType data_;
};

// Indexed Z-set: A Z-set indexed by a key for efficient joins.
// Conceptually: K -> ZSet<V>
template <typename K, typename V, typename KHash>
class IndexedZSet {
public:
    using KeyType = K;
    using ValueType = V;
    using InnerZSet = ZSet<V>;
    using MapType = std::unordered_map<K, InnerZSet, KHash>;
    using Iterator = typename MapType::const_iterator;

    IndexedZSet() = default;

    // Get the Z-set for a given key
    const InnerZSet& operator[](const K& key) const {
        static const InnerZSet empty;
        auto it = data_.find(key);
        return it != data_.end() ? it->second : empty;
    }

    // Insert an element
    void insert(const K& key, const V& value, Weight weight = 1) {
        if (weight == 0) return;
        data_[key].insert(value, weight);
        if (data_[key].empty()) {
            data_.erase(key);
        }
    }

    // Get all keys
    std::vector<K> keys() const {
        std::vector<K> result;
        result.reserve(data_.size());
        for (const auto& [k, v] : data_) {
            result.push_back(k);
        }
        return result;
    }

    // Check if key exists
    bool contains_key(const K& key) const {
        return data_.count(key) > 0;
    }

    // Support size
    size_t support_size() const {
        size_t total = 0;
        for (const auto& [k, v] : data_) {
            total += v.support_size();
        }
        return total;
    }

    bool empty() const { return data_.empty(); }
    void clear() { data_.clear(); }

    // Addition
    IndexedZSet operator+(const IndexedZSet& other) const {
        IndexedZSet result = *this;
        for (const auto& [key, zset] : other.data_) {
            for (const auto& [val, weight] : zset) {
                result.insert(key, val, weight);
            }
        }
        return result;
    }

    IndexedZSet& operator+=(const IndexedZSet& other) {
        for (const auto& [key, zset] : other.data_) {
            for (const auto& [val, weight] : zset) {
                insert(key, val, weight);
            }
        }
        return *this;
    }

    // Subtraction
    IndexedZSet operator-(const IndexedZSet& other) const {
        IndexedZSet result = *this;
        for (const auto& [key, zset] : other.data_) {
            for (const auto& [val, weight] : zset) {
                result.insert(key, val, -weight);
            }
        }
        return result;
    }

    // Iteration
    Iterator begin() const { return data_.begin(); }
    Iterator end() const { return data_.end(); }

    // Raw data access
    const MapType& data() const { return data_; }
    MapType& data() { return data_; }

private:
    MapType data_;
};

// Product of two Z-sets: (a,b) -> weight(a) * weight(b)
template <typename A, typename B, typename AHash = std::hash<A>, typename BHash = std::hash<B>>
ZSet<std::pair<A, B>> product(const ZSet<A, AHash>& zs1, const ZSet<B, BHash>& zs2) {
    ZSet<std::pair<A, B>> result;
    for (const auto& [a, wa] : zs1) {
        for (const auto& [b, wb] : zs2) {
            result.insert(std::make_pair(a, b), wa * wb);
        }
    }
    return result;
}

// Equi-join of two Z-sets on keys
template <typename A, typename B, typename K,
          typename KHash = std::hash<K>,
          typename AHash = std::hash<A>,
          typename BHash = std::hash<B>>
ZSet<std::pair<A, B>> equi_join(
    const ZSet<A, AHash>& zs1,
    const ZSet<B, BHash>& zs2,
    std::function<K(const A&)> key1,
    std::function<K(const B&)> key2
) {
    // Index the second Z-set by key
    IndexedZSet<K, B, KHash> indexed2;
    for (const auto& [b, wb] : zs2) {
        indexed2.insert(key2(b), b, wb);
    }

    // Join
    ZSet<std::pair<A, B>> result;
    for (const auto& [a, wa] : zs1) {
        K k = key1(a);
        for (const auto& [b, wb] : indexed2[k]) {
            result.insert(std::make_pair(a, b), wa * wb);
        }
    }
    return result;
}

// Aggregation: group by key and aggregate values
template <typename T, typename K, typename Agg,
          typename THash = std::hash<T>,
          typename KHash = std::hash<K>>
ZSet<std::pair<K, Agg>, std::hash<std::pair<K, Agg>>> aggregate(
    const ZSet<T, THash>& zs,
    std::function<K(const T&)> key_fn,
    std::function<Agg(const T&)> value_fn,
    std::function<Agg(Agg, Agg)> combine
) {
    // First, group by key
    std::unordered_map<K, std::vector<std::pair<Agg, Weight>>, KHash> groups;
    for (const auto& [elem, weight] : zs) {
        K k = key_fn(elem);
        Agg v = value_fn(elem);
        groups[k].emplace_back(v, weight);
    }

    // Then aggregate
    ZSet<std::pair<K, Agg>> result;
    for (const auto& [k, values] : groups) {
        // For now, simple sum aggregation considering weights
        Agg total{};
        for (const auto& [v, w] : values) {
            // This is simplified - real aggregation needs to handle weights properly
            for (Weight i = 0; i < std::abs(w); ++i) {
                total = combine(total, v);
            }
        }
        result.insert(std::make_pair(k, total), 1);
    }
    return result;
}

} // namespace dbsp
