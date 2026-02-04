# Contributing to DBSP for DuckDB

Thank you for your interest in contributing! This document provides guidelines and instructions for contributing to the project.

## Table of Contents

- [Code of Conduct](#code-of-conduct)
- [Getting Started](#getting-started)
- [Development Setup](#development-setup)
- [Making Changes](#making-changes)
- [Testing](#testing)
- [Pull Request Process](#pull-request-process)
- [Style Guide](#style-guide)

## Code of Conduct

This project follows the [Contributor Covenant](https://www.contributor-covenant.org/) code of conduct. Please be respectful and constructive in all interactions.

## Getting Started

### Finding Issues

- Look for issues labeled `good first issue` for beginner-friendly tasks
- Issues labeled `help wanted` are ready for community contribution
- Feel free to ask questions on any issue before starting work

### Reporting Bugs

1. Check existing issues to avoid duplicates
2. Use the bug report template
3. Include:
   - DuckDB version
   - Operating system
   - Minimal reproduction case
   - Expected vs actual behavior

### Suggesting Features

1. Open a feature request issue
2. Describe the use case
3. Explain why existing features don't solve the problem
4. If possible, outline a proposed implementation

## Development Setup

### Prerequisites

- C++17 compatible compiler (GCC 8+, Clang 7+, MSVC 2019+)
- CMake 3.14+
- Git

### Building the Core Library

```bash
git clone https://github.com/yourusername/duckDBSP.git
cd duckDBSP

mkdir build && cd build
cmake ..
make

# Run tests
./dbsp_tests
```

### Building the DuckDB Extension

```bash
cd duckdb_extension

# This fetches DuckDB source (~1GB) and builds
./build.sh

# Output: dbsp.duckdb_extension
```

### Running the Extension

```bash
# Start DuckDB with the extension
duckdb -cmd "LOAD './dbsp.duckdb_extension'"
```

## Making Changes

### Branch Naming

- `feature/description` - New features
- `fix/description` - Bug fixes
- `docs/description` - Documentation updates
- `refactor/description` - Code refactoring
- `test/description` - Test additions/improvements

### Commit Messages

Follow [Conventional Commits](https://www.conventionalcommits.org/):

```
type(scope): description

[optional body]

[optional footer]
```

Types: `feat`, `fix`, `docs`, `style`, `refactor`, `test`, `chore`

Examples:
```
feat(views): add support for HAVING clause

fix(cdc): handle null values in tracked tables

docs(api): add examples for persistence functions
```

## Testing

### Running Tests

```bash
cd build
make test
# or
./dbsp_tests
```

### Writing Tests

Tests use a simple assertion framework in `test/test_zset.cpp`:

```cpp
void test_my_feature() {
    // Setup
    ZSet<int> zset;

    // Action
    zset.insert(1, 1);
    zset.insert(2, 2);

    // Assert
    assert(zset.get(1) == 1);
    assert(zset.get(2) == 2);
    assert(zset.support_size() == 2);

    std::cout << "test_my_feature passed\n";
}
```

### Test Categories

1. **Unit tests** (`test/test_zset.cpp`)
   - Z-Set operations
   - Stream operators
   - Individual view types

2. **Integration tests** (SQL files in `examples/`)
   - End-to-end workflows
   - Extension functions

### Coverage Goals

- All public functions should have tests
- Edge cases: empty inputs, null values, large datasets
- Error handling: invalid inputs, circular dependencies

## Pull Request Process

### Before Submitting

1. [ ] Code compiles without warnings
2. [ ] All tests pass
3. [ ] New features have tests
4. [ ] Documentation updated if needed
5. [ ] Commit messages follow conventions

### PR Description

Include:
- Summary of changes
- Related issue number(s)
- Testing done
- Screenshots for UI changes

### Review Process

1. Automated checks run (build, tests)
2. Maintainer reviews code
3. Address feedback with new commits (don't force push)
4. Maintainer merges when approved

## Style Guide

### C++ Style

```cpp
// Naming
class MyClass {};              // PascalCase for classes
void my_function();            // snake_case for functions
int my_variable;               // snake_case for variables
const int MY_CONSTANT = 42;    // SCREAMING_SNAKE_CASE for constants
int member_variable_;          // trailing underscore for members

// Braces
if (condition) {
    // ...
} else {
    // ...
}

// Line length: 100 characters max

// Comments
// Single line comments for brief notes

/**
 * Multi-line comments for function documentation.
 * @param x Description of parameter
 * @return Description of return value
 */

// Use #pragma once for header guards
#pragma once

// Include order:
// 1. Related header
// 2. C system headers
// 3. C++ standard library
// 4. Other library headers
// 5. Project headers
```

### SQL Style (in examples)

```sql
-- Use uppercase for keywords
SELECT * FROM table WHERE condition;

-- Use meaningful aliases
SELECT
    c.name AS customer_name,
    SUM(o.amount) AS total_spent
FROM customers c
JOIN orders o ON c.id = o.customer_id
GROUP BY c.name;

-- Comment sections clearly
-- =============================================================================
-- Section Title
-- =============================================================================
```

## Project Structure

```
duckDBSP/
├── include/               # Core DBSP library (header-only)
│   ├── dbsp_zset.hpp     # Z-Set implementation
│   ├── dbsp_stream.hpp   # Stream operators
│   └── ...
├── duckdb_extension/     # DuckDB extension
│   ├── dbsp_extension.cpp
│   └── ...
├── src/                  # Standalone executables
├── test/                 # Unit tests
├── docs/                 # Documentation
├── examples/             # Usage examples
└── CMakeLists.txt
```

## Areas for Contribution

### Good First Issues

- Add missing aggregate functions (MIN, MAX)
- Improve error messages
- Add more examples
- Documentation improvements

### Intermediate

- Implement HAVING clause
- Add ORDER BY support
- Performance benchmarks
- Memory usage tracking

### Advanced

- Window functions
- Subquery support
- Parallel change propagation
- Persistent state (not just definitions)

## Getting Help

- Open a [Discussion](https://github.com/yourusername/duckDBSP/discussions) for questions
- Tag maintainers in issues if stuck
- Join community chat (if available)

## License

By contributing, you agree that your contributions will be licensed under the MIT License.
