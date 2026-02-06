# DBSP-E105: Subqueries Not Supported

**Category**: Parser Error
**Severity**: Error
**Since**: Version 1.0

## Description

Subqueries (nested SELECT statements) are not yet supported in duckDBSP materialized views. This includes subqueries in FROM clauses, WHERE clauses, and SELECT lists.

## Common Causes

- Subquery in FROM clause (derived table)
- Subquery with IN operator in WHERE
- Subquery with EXISTS in WHERE
- Correlated subqueries

## Example

### This will fail:
```sql
-- Subquery in FROM
SELECT * FROM (SELECT id FROM orders WHERE amount > 100) AS high_orders;

-- Subquery in WHERE
SELECT * FROM orders WHERE customer_id IN (SELECT id FROM customers WHERE active = true);
```

### Error message:
```
DBSP-E105: Subquery in FROM clause (derived table)

SQL:
SELECT * FROM (SELECT id FROM orders WHERE amount > 100) AS high_orders
               ^

Workaround:
Rewrite using JOINs or create intermediate views. Subquery support is tracked in TODO #6.

Documentation: docs/errors/E1xx/DBSP-E105.md
```

## Workaround

### Option 1: Use intermediate views

Instead of:
```sql
SELECT * FROM orders
WHERE customer_id IN (SELECT id FROM customers WHERE active = true);
```

Do this:
```sql
-- Step 1: Create view for active customers
SELECT * FROM dbsp_create_view('active_customers',
    'SELECT id FROM customers WHERE active = true');

-- Step 2: Join with orders
SELECT * FROM dbsp_create_view('filtered_orders',
    'SELECT o.* FROM orders o JOIN active_customers c ON o.customer_id = c.id');
```

### Option 2: Rewrite with JOIN

```sql
SELECT o.* FROM orders o
JOIN customers c ON o.customer_id = c.id
WHERE c.active = true;
```

## Related

- TODO #6 - Add subquery support
- [API.md](../API.md) - JOIN syntax
- [ARCHITECTURE.md](../ARCHITECTURE.md) - Cascading views

## Roadmap

Subquery support is tracked in TODO #6. Implementation will require:
- Parsing nested SELECT statements
- Creating dependency graphs for subqueries
- Ensuring proper incremental updates

## Technical Details

Subqueries complicate dependency tracking and incremental updates. Each subquery effectively becomes an intermediate materialized view that must be updated when its dependencies change.

The workaround of creating explicit intermediate views is actually the implementation strategy - making the dependencies explicit rather than implicit.
