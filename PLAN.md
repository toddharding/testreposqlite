# Implementation Plan: Add Support for Sorted Sets

**Ticket:** gh:toddharding/testreposqlite#1
**Complexity:** Mid
**Branch:** `feat/add-support-for-sorted-sets-gh-1-12baae7f`

## Overview

Add a sorted set extension to SQLite, implemented as a virtual table in `ext/misc/sortedset.c`. A sorted set stores unique text members, each associated with a numeric score. Members are ordered by score (ascending), with ties broken lexicographically by member name. This follows Redis-like sorted set semantics (ZADD/ZRANGE/ZREM) but exposed through standard SQL.

## Design

### Data Model

Each sorted set is backed by a **real SQLite table** (shadow table) that persists data. The virtual table provides a sorted-set-aware interface on top.

**Shadow table schema:**
```sql
CREATE TABLE IF NOT EXISTS "<name>_data" (
  member TEXT PRIMARY KEY,
  score REAL NOT NULL
);
CREATE INDEX IF NOT EXISTS "<name>_data_score" ON "<name>_data"(score, member);
```

**Virtual table schema (declared to SQLite):**
```sql
CREATE TABLE x(
  member TEXT,    -- the set member (unique key)
  score REAL,     -- the associated score
  rank INTEGER    -- computed: 0-based position in score order (read-only)
);
```

### Usage Examples

```sql
-- Create a sorted set
CREATE VIRTUAL TABLE leaderboard USING sortedset();

-- Insert members with scores (UPSERT semantics: if member exists, update score)
INSERT INTO leaderboard(member, score) VALUES ('alice', 100.0);
INSERT INTO leaderboard(member, score) VALUES ('bob', 85.5);
INSERT INTO leaderboard(member, score) VALUES ('charlie', 92.0);

-- Query all members sorted by score
SELECT member, score, rank FROM leaderboard;
-- alice   | 100.0 | 2
-- bob     | 85.5  | 0
-- charlie | 92.0  | 1

-- Range query by score
SELECT member, score FROM leaderboard WHERE score >= 90.0;

-- Delete a member
DELETE FROM leaderboard WHERE member = 'bob';

-- Update a score (via INSERT with existing member, or UPDATE)
UPDATE leaderboard SET score = 110.0 WHERE member = 'charlie';
```

### Architecture

The extension follows established SQLite virtual table patterns (modeled after `ext/misc/csv.c` and `ext/misc/completion.c`):

1. **SortedSetTab** struct - holds the database connection, shadow table name, and configuration
2. **SortedSetCursor** struct - holds a prepared statement for iterating the shadow table in score order
3. **xCreate** - creates the shadow table and index
4. **xConnect** - reconnects to an existing shadow table
5. **xDestroy** - drops the shadow table
6. **xDisconnect** - releases the connection
7. **xBestIndex** - pushes down score range constraints (`score >= ?`, `score <= ?`, `member = ?`) to the shadow table query
8. **xFilter** - prepares and executes the appropriate SELECT on the shadow table based on constraints
9. **xNext/xEof/xColumn/xRowid** - standard cursor iteration
10. **xUpdate** - handles INSERT (with UPSERT for duplicate members), UPDATE, and DELETE by modifying the shadow table

## Files to Create/Modify

### 1. `ext/misc/sortedset.c` (NEW - ~400-500 lines)

The main extension source file. Structure:

```
[License header - SQLite public domain blessing]
#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1
#include <string.h>
#include <assert.h>

#ifndef SQLITE_OMIT_VIRTUALTABLE

// --- Structures ---
typedef struct SortedSetTab {
  sqlite3_vtab base;
  sqlite3 *db;
  char *zDb;           // database name (e.g., "main")
  char *zName;         // virtual table name
  char *zDataTable;    // shadow table name ("<name>_data")
} SortedSetTab;

typedef struct SortedSetCursor {
  sqlite3_vtab_cursor base;
  sqlite3_stmt *pStmt;  // current query statement
  sqlite3_int64 iRowid; // current rowid counter
  int bEof;             // true when no more rows
} SortedSetCursor;

// --- Shadow table management ---
static int sortedsetCreateShadowTable(SortedSetTab *pTab);
static int sortedsetDropShadowTable(SortedSetTab *pTab);

// --- Virtual table methods ---
static int sortedsetCreate(sqlite3*, void*, int, const char*const*, sqlite3_vtab**, char**);
static int sortedsetConnect(sqlite3*, void*, int, const char*const*, sqlite3_vtab**, char**);
static int sortedsetBestIndex(sqlite3_vtab*, sqlite3_index_info*);
static int sortedsetDisconnect(sqlite3_vtab*);
static int sortedsetDestroy(sqlite3_vtab*);
static int sortedsetOpen(sqlite3_vtab*, sqlite3_vtab_cursor**);
static int sortedsetClose(sqlite3_vtab_cursor*);
static int sortedsetFilter(sqlite3_vtab_cursor*, int, const char*, int, sqlite3_value**);
static int sortedsetNext(sqlite3_vtab_cursor*);
static int sortedsetEof(sqlite3_vtab_cursor*);
static int sortedsetColumn(sqlite3_vtab_cursor*, sqlite3_context*, int);
static int sortedsetRowid(sqlite3_vtab_cursor*, sqlite3_int64*);
static int sortedsetUpdate(sqlite3_vtab*, int, sqlite3_value**, sqlite3_int64*);

// --- Module definition ---
static sqlite3_module sortedsetModule = {
  0,                      /* iVersion */
  sortedsetCreate,        /* xCreate */
  sortedsetConnect,       /* xConnect */
  sortedsetBestIndex,     /* xBestIndex */
  sortedsetDisconnect,    /* xDisconnect */
  sortedsetDestroy,       /* xDestroy */
  sortedsetOpen,          /* xOpen */
  sortedsetClose,         /* xClose */
  sortedsetFilter,        /* xFilter */
  sortedsetNext,          /* xNext */
  sortedsetEof,           /* xEof */
  sortedsetColumn,        /* xColumn */
  sortedsetRowid,         /* xRowid */
  sortedsetUpdate,        /* xUpdate */
  0, 0, 0, 0, 0, 0, 0,  /* xBegin..xShadowName */
  0                       /* xIntegrity */
};

#endif /* SQLITE_OMIT_VIRTUALTABLE */

// --- Extension entry point ---
int sqlite3_sortedset_init(sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi);
```

**Key implementation details:**

- **xCreate vs xConnect:** `xCreate` creates the shadow table + index; `xConnect` just connects. Both call a shared helper to populate the `SortedSetTab` struct.
- **xDestroy vs xDisconnect:** `xDestroy` drops the shadow table; `xDisconnect` just frees memory.
- **xUpdate:** Uses `INSERT OR REPLACE INTO <name>_data(member, score) VALUES(?, ?)` for inserts/updates (gives UPSERT semantics). Uses `DELETE FROM <name>_data WHERE member = ?` for deletes.
- **xBestIndex/xFilter:** Support these constraint patterns:
  - No constraints: full scan ordered by `(score, member)`
  - `member = ?`: point lookup
  - `score >= ?` and/or `score <= ?`: range scan by score
  - Use `idxNum` bitmask: bit 0 = member eq, bit 1 = score ge, bit 2 = score le
- **rank column:** Computed as `ROW_NUMBER() - 1` using a subquery or by counting rows as the cursor advances. Simplest approach: track as `iRowid` counter during iteration (0-based).

### 2. `test/sortedset1.test` (NEW - ~150-200 lines)

TCL test file following established patterns (see `test/csv01.test`):

```tcl
set testdir [file dirname $argv0]
source $testdir/tester.tcl

# Skip if virtual tables are disabled
ifcapable !vtab { finish_test ; return }

# Load the extension
load_static_extension db sortedset

# --- Test groups ---

# 1. Basic creation and insertion
do_execsql_test 1.1 {
  CREATE VIRTUAL TABLE t1 USING sortedset();
  INSERT INTO t1(member, score) VALUES('alice', 100.0);
  INSERT INTO t1(member, score) VALUES('bob', 85.5);
  INSERT INTO t1(member, score) VALUES('charlie', 92.0);
  SELECT member, score FROM t1 ORDER BY score;
} {bob 85.5 charlie 92.0 alice 100.0}

# 2. Rank computation
do_execsql_test 2.1 { ... }

# 3. UPSERT semantics (insert duplicate member updates score)
do_execsql_test 3.1 { ... }

# 4. DELETE
do_execsql_test 4.1 { ... }

# 5. UPDATE
do_execsql_test 5.1 { ... }

# 6. Score range queries
do_execsql_test 6.1 { ... }

# 7. Member lookup
do_execsql_test 7.1 { ... }

# 8. Empty set
do_execsql_test 8.1 { ... }

# 9. DROP (verify shadow table cleanup)
do_execsql_test 9.1 { ... }

# 10. Multiple sorted sets
do_execsql_test 10.1 { ... }

# 11. Edge cases: NULL member, NULL score, very large scores
do_execsql_test 11.1 { ... }

# 12. Tie-breaking: same score, different members
do_execsql_test 12.1 { ... }

finish_test
```

### 3. `main.mk` (MODIFY)

Add `sortedset.c` to the list of extension source files compiled into the test fixture. Find the section where other `ext/misc/*.c` files are listed and add:

```makefile
$(TOP)/ext/misc/sortedset.c
```

This should be added to:
- The `TESTSRC` or equivalent variable that lists extension sources for `testfixture`

### 4. `src/test1.c` (MODIFY)

Register the extension for static loading in tests. Find the `aExtension[]` array and add:

```c
extern int sqlite3_sortedset_init(sqlite3*,char**,const sqlite3_api_routines*);
```

And in the array:
```c
{ "sortedset", sqlite3_sortedset_init },
```

## Implementation Order

1. **`ext/misc/sortedset.c`** - Write the full extension:
   - Start with structs and shadow table helpers
   - Implement xCreate/xConnect/xDisconnect/xDestroy
   - Implement xOpen/xClose
   - Implement xFilter/xNext/xEof/xColumn/xRowid (read path)
   - Implement xBestIndex (constraint pushdown)
   - Implement xUpdate (write path with UPSERT)
   - Add extension init function

2. **Build integration** - Modify `main.mk` and `src/test1.c` to compile and register the extension

3. **`test/sortedset1.test`** - Write comprehensive tests

4. **Verify** - Build `testfixture` and run `test/sortedset1.test`

## Build & Test Commands

```bash
# Configure (if not already done)
./configure

# Build test fixture with the new extension
make testfixture

# Run sorted set tests
./testfixture test/sortedset1.test

# Run full test suite to check for regressions
make devtest
```

## Constraints & Assumptions

- The extension uses **shadow tables** for persistence, which is the standard SQLite pattern for virtual tables that store data (used by FTS5, rtree, etc.)
- Member uniqueness is enforced by the `PRIMARY KEY` on the shadow table's `member` column
- Score ordering uses SQLite's native `REAL` comparison
- The `rank` column is a computed value based on iteration order (0-based), not stored
- The extension is guarded by `#ifndef SQLITE_OMIT_VIRTUALTABLE`
- No changes to the core SQLite source files beyond test infrastructure registration
