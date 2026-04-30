# Plan: Add Support for Sorted Sets (gh:toddharding/testreposqlite#1)

## Overview

Implement Redis-style sorted sets as a SQLite loadable extension (`ext/misc/sortedsets.c`).
Each sorted set is identified by a string key. Members are unique strings within a key,
each associated with a double-precision floating-point score. Members are ordered by score
(ascending), with lexicographic tiebreaking on member name.

Data is persisted in a backing SQLite table, created automatically on first use.

## Architecture

### Storage Layer

A single backing table per database connection, created lazily:

```sql
CREATE TABLE IF NOT EXISTS _sortedsets(
  key   TEXT NOT NULL,
  member TEXT NOT NULL,
  score  REAL NOT NULL,
  PRIMARY KEY(key, member)
);
CREATE INDEX IF NOT EXISTS _sortedsets_score ON _sortedsets(key, score, member);
```

The compound index on `(key, score, member)` enables efficient range queries and rank
lookups. The primary key on `(key, member)` ensures uniqueness and efficient point lookups.

### SQL Functions (Scalar + Aggregate)

All functions are registered via `sqlite3_create_function()` in the init entry point.

| Function | Signature | Returns | Description |
|----------|-----------|---------|-------------|
| `zadd` | `zadd(key, score, member)` | INTEGER (1=new, 0=updated) | Add or update a member's score. Uses INSERT OR REPLACE. |
| `zrem` | `zrem(key, member)` | INTEGER (rows deleted: 0 or 1) | Remove a member from the set. |
| `zscore` | `zscore(key, member)` | REAL or NULL | Return the score of a member, or NULL if not present. |
| `zrank` | `zrank(key, member)` | INTEGER or NULL | 0-based rank by ascending score. NULL if member absent. |
| `zrevrank` | `zrevrank(key, member)` | INTEGER or NULL | 0-based rank by descending score. NULL if member absent. |
| `zcard` | `zcard(key)` | INTEGER | Number of members in the sorted set. |
| `zcount` | `zcount(key, min_score, max_score)` | INTEGER | Count members with score in [min, max]. |
| `zincrby` | `zincrby(key, increment, member)` | REAL | Increment member's score by `increment`. Returns new score. Creates member with `increment` as score if absent. |

### Virtual Table: `zrange`

A read-only eponymous virtual table for range queries:

```sql
-- By rank range (0-based, inclusive)
SELECT member, score FROM zrange WHERE key='myset' AND start=0 AND stop=9;

-- By score range
SELECT member, score FROM zrange WHERE key='myset' AND min=1.0 AND max=100.0;

-- Reverse order
SELECT member, score FROM zrange WHERE key='myset' AND start=0 AND stop=-1 AND rev=1;
```

Virtual table columns: `member TEXT, score REAL, key TEXT HIDDEN, start INT HIDDEN, stop INT HIDDEN, min REAL HIDDEN, max REAL HIDDEN, rev INT HIDDEN`.

`xBestIndex` requires `key` as an equality constraint (SQLITE_INDEX_CONSTRAINT_EQ). `start`/`stop` or `min`/`max` are optional filter pairs. `rev` is optional (default 0).

`stop=-1` means "to the end" (Redis convention).

## File Changes

### 1. `ext/misc/sortedsets.c` (NEW - primary implementation)

Single-file extension following the pattern of `ext/misc/series.c` and `ext/misc/csv.c`.

Structure:
1. Standard SQLite extension header (`#include <sqlite3ext.h>`, `SQLITE_EXTENSION_INIT1`)
2. Helper: `sortedsets_ensure_table(sqlite3 *db)` - lazily creates backing table + index
3. Scalar function implementations (`zadd_func`, `zrem_func`, `zscore_func`, `zrank_func`, `zrevrank_func`, `zcard_func`, `zcount_func`, `zincrby_func`)
4. Virtual table module (`zrangeModule`) with:
   - `zrangeConnect` / `zrangeDisconnect`
   - `zrangeBestIndex`
   - `zrangeOpen` / `zrangeClose`
   - `zrangeFilter` / `zrangeNext` / `zrangeEof`
   - `zrangeColumn` / `zrangeRowid`
5. Entry point: `sqlite3_sortedsets_init()` registering all functions + the virtual table module

Each scalar function:
- Calls `sortedsets_ensure_table()` on first invocation
- Prepares a statement, binds parameters, executes, returns result
- Uses `sqlite3_get_auxdata`/`sqlite3_set_auxdata` to cache prepared statements where beneficial

### 2. `main.mk` (MODIFY)

Add `$(TOP)/ext/misc/sortedsets.c` to the `TESTSRC` list (around line 803) so the extension
is statically linked into the test harness.

### 3. `test/sortedsets.test` (NEW - Tcl test file)

Tcl test file following the pattern of `test/tabfunc01.test`.

Test groups:
- **sortedsets-1.x**: Basic ZADD/ZSCORE/ZREM operations
  - 1.1: zadd returns 1 for new member
  - 1.2: zadd returns 0 for score update
  - 1.3: zscore returns correct score
  - 1.4: zscore returns NULL for missing member
  - 1.5: zrem returns 1 for existing member
  - 1.6: zrem returns 0 for missing member
- **sortedsets-2.x**: ZRANK/ZREVRANK
  - 2.1: zrank with multiple members
  - 2.2: zrevrank correctness
  - 2.3: zrank returns NULL for missing member
- **sortedsets-3.x**: ZCARD/ZCOUNT
  - 3.1: zcard on populated set
  - 3.2: zcard on empty/missing set returns 0
  - 3.3: zcount with score range
- **sortedsets-4.x**: ZINCRBY
  - 4.1: zincrby on existing member
  - 4.2: zincrby on new member (creates it)
  - 4.3: zincrby with negative increment
- **sortedsets-5.x**: Virtual table (zrange)
  - 5.1: zrange by rank (start/stop)
  - 5.2: zrange by score (min/max)
  - 5.3: zrange with reverse order
  - 5.4: zrange with stop=-1 (to end)
  - 5.5: zrange with missing key returns empty
- **sortedsets-6.x**: Edge cases
  - 6.1: Multiple independent keys
  - 6.2: Score tiebreaking by member name
  - 6.3: Very large scores (infinity-adjacent)
  - 6.4: Empty string as member
  - 6.5: Unicode member names

## Implementation Order

1. Create `ext/misc/sortedsets.c` with the backing table helper and all scalar functions
2. Add the `zrange` virtual table to the same file
3. Wire up `sqlite3_sortedsets_init()` entry point
4. Update `main.mk` to include the extension in `TESTSRC`
5. Create `test/sortedsets.test` with the test cases above
6. Build and run tests to verify

## Design Decisions

- **Single backing table** (not one table per sorted set): Simpler schema management, follows Redis's key-based namespace model. The compound index makes per-key queries efficient.
- **Eponymous virtual table for zrange**: Allows natural SQL integration without requiring CREATE VIRTUAL TABLE. Users can query it like `SELECT * FROM zrange WHERE key='x' AND start=0 AND stop=10`.
- **Scalar functions for mutations**: More natural than virtual table xUpdate for operations like ZADD, ZINCRBY. Can be used in any SQL context (SELECT, triggers, etc.).
- **Lazy table creation**: The backing table is created on first use, so loading the extension has no side effects on databases that don't use sorted sets.
- **No separate namespace/schema prefix**: Functions use Redis-style short names (zadd, zrem, etc.) for familiarity.
