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

template <typename K, typename V, typename KHash = std::hash<K>>
class IndexedZSet;

// Weight type - using int64_t to handle large multiplicities
using Weight = int64_t;

// A Z-set is a function from T to Z with finite support.
// It maps elements to their integer weights (multiplicities).
// Positive weight = insertion, negative weight = deletion, zero = not present.
template <typename T, typename Hash>
class ZSet {
public:
    using ElementType = T;
    using MapType = std::unordered_map<T, Weight, Hash>;
    using Iterator = typename MapType::const_iterator;

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

    // Get the support (elements with non-zero weight)
    size_t support_size() const { return data_.size(); }

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
