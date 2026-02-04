# DBSP Theory

This document explains the mathematical foundations of Database Stream Processing (DBSP) that power the incremental view maintenance in this extension.

## Overview

DBSP is a formal framework that enables automatic incrementalization of database queries. Instead of recomputing query results from scratch when data changes, DBSP-based systems compute only the changes to the output.

## Core Concepts

### Z-Sets

A **Z-Set** is a generalization of a multiset where elements can have integer weights (including negative weights).

```
Z-Set: Element → Weight (ℤ)

Examples:
  {Alice: 1, Bob: 2}      -- Alice appears once, Bob twice
  {Alice: -1}             -- Deletion of Alice
  {Alice: 1, Alice: -1}   -- Cancels to {} (empty)
```

**Key Properties:**
- Weight +1 represents an insertion
- Weight -1 represents a deletion
- Weights can accumulate (multiset semantics)
- Zero weights are removed (element not in set)

**Operations:**
```
Addition:     {A:1, B:2} + {A:1, C:1} = {A:2, B:2, C:1}
Negation:     -{A:1, B:2} = {A:-1, B:-2}
Subtraction:  {A:2, B:1} - {A:1} = {A:1, B:1}
```

### Streams

A **Stream** is an infinite sequence of Z-Sets indexed by time:

```
Stream: ℕ → ZSet

s[0], s[1], s[2], ...

Where s[t] represents the changes at time t.
```

### Core Operators

DBSP defines several fundamental operators on streams:

#### Lifting (↑)

Lifts a Z-Set operator to work on streams (apply element-wise):

```
(↑f)(s)[t] = f(s[t])
```

#### Delay (z⁻¹)

Shifts a stream one step back in time:

```
(z⁻¹ s)[t] = s[t-1]   (where s[-1] = ∅)
```

#### Integration (I)

Computes the running sum of a stream:

```
I(s)[t] = Σᵢ₌₀ᵗ s[i]

I(s)[t] = I(s)[t-1] + s[t]
        = z⁻¹(I(s))[t] + s[t]
```

Integration converts a stream of changes into a stream of states.

#### Differentiation (D)

Computes the difference between consecutive elements:

```
D(s)[t] = s[t] - s[t-1]
        = s[t] - z⁻¹(s)[t]
```

Differentiation converts a stream of states into a stream of changes.

**Key Identity:**
```
D(I(s)) = s    (differentiation undoes integration)
I(D(s)) = s    (integration undoes differentiation)
```

## Incrementalization

The core insight of DBSP is that any query Q can be incrementalized:

```
Q^Δ = D ∘ Q ∘ I
```

Where:
- `I` integrates the input changes into full state
- `Q` applies the original query
- `D` differentiates to get output changes

### Linear Operators

For **linear** operators (filter, map, union), incrementalization is trivial:

```
filter^Δ(Δinput) = filter(Δinput)
```

The incremental version is the same as the original - just apply to changes.

### Bilinear Operators (Join)

For **bilinear** operators like join, the incremental formula is:

```
join^Δ(Δa, Δb) = Δa × I(z⁻¹(b)) + I(z⁻¹(a)) × Δb + Δa × Δb
```

Simplified (ignoring the Δa × Δb term which is often negligible):

```
join^Δ(Δa, Δb) ≈ Δa × old_b + old_a × Δb
```

This means:
- Join new left rows with all existing right rows
- Join all existing left rows with new right rows

### Distinct

The distinct operator requires tracking multiplicities:

```
distinct(zset)[x] = 1 if zset[x] > 0 else 0

distinct^Δ(Δinput):
  for each element x in Δinput:
    old_count = state[x]
    new_count = old_count + Δinput[x]

    if old_count <= 0 and new_count > 0:
      output +1 for x  (element now present)
    if old_count > 0 and new_count <= 0:
      output -1 for x  (element now absent)
```

### Aggregation

For aggregations like SUM:

```
sum^Δ(group, Δvalues):
  for each (key, Δvalue) in Δvalues:
    old_sum = state[key]
    new_sum = old_sum + Δvalue

    output: -old_sum for key (if old_sum != 0)
    output: +new_sum for key (if new_sum != 0)
```

## Implementation in This Extension

### Z-Set Implementation

```cpp
template <typename T>
class ZSet {
    std::unordered_map<T, int64_t> data_;

    void insert(const T& elem, int64_t weight) {
        data_[elem] += weight;
        if (data_[elem] == 0) {
            data_.erase(elem);  // Remove zero-weight elements
        }
    }
};
```

### Stream Operators

```cpp
// Integration: maintains running sum
class Integration {
    ZSet<T> integrated_;  // I(s)[t-1]

    ZSet<T> process(const ZSet<T>& delta) {
        integrated_ = integrated_ + delta;  // I(s)[t] = I(s)[t-1] + s[t]
        return integrated_;
    }
};

// Differentiation: computes changes
class Differentiation {
    ZSet<T> previous_;  // s[t-1]

    ZSet<T> process(const ZSet<T>& current) {
        ZSet<T> delta = current - previous_;  // D(s)[t] = s[t] - s[t-1]
        previous_ = current;
        return delta;
    }
};
```

### View Types

Each view type implements incremental processing:

```cpp
class FilterView {
    ZSet<Row> result_;

    void apply_changes(const ZSet<Row>& delta) {
        for (auto& [row, weight] : delta) {
            if (predicate(row)) {
                result_.insert(row, weight);
            }
        }
    }
};

class AggregateView {
    std::map<Key, AggState> states_;  // Running aggregates
    ZSet<Row> result_;

    void apply_changes(const ZSet<Row>& delta) {
        for (auto& [row, weight] : delta) {
            Key key = extract_key(row);
            Value val = extract_value(row);

            // Remove old result
            if (states_[key].count > 0) {
                result_.insert(make_row(key, states_[key]), -1);
            }

            // Update state
            states_[key].sum += val * weight;
            states_[key].count += weight;

            // Add new result
            if (states_[key].count > 0) {
                result_.insert(make_row(key, states_[key]), +1);
            }
        }
    }
};
```

## Dependency Graph

For cascading views, changes propagate through a dependency graph:

```
orders (table)
    │
    ├── filter_view
    │       │
    │       └── aggregate_on_filter
    │
    └── totals_view
            │
            └── vip_totals
```

When `orders` changes:
1. Compute delta for `orders`
2. Apply to `filter_view` and `totals_view` (topological order)
3. Apply `filter_view` delta to `aggregate_on_filter`
4. Apply `totals_view` delta to `vip_totals`

## Complexity Analysis

| Operation | Traditional | DBSP |
|-----------|-------------|------|
| Filter | O(n) | O(Δ) |
| Map/Project | O(n) | O(Δ) |
| Distinct | O(n) | O(Δ) |
| Group By Aggregate | O(n) | O(Δ + affected_groups) |
| Join | O(n × m) | O(Δ × m + n × Δ) |

Where:
- n, m = sizes of input tables
- Δ = size of changes

## References

1. Budiu, M., et al. "DBSP: Automatic Incremental View Maintenance." VLDB 2023.
   https://www.vldb.org/pvldb/vol16/p1601-budiu.pdf

2. Chajed, T. "Database Stream Processing Theory (Lean Formalization)"
   https://github.com/tchajed/database-stream-processing-theory

3. Feldera Documentation
   https://docs.feldera.com/

4. McSherry, F. "Differential Dataflow"
   https://github.com/TimelyDataflow/differential-dataflow
