# Implementation Plan: Sorted Set Extension for SQLite

**Ticket:** gh:toddharding/testreposqlite#1
**Complexity:** Complex (new extension with virtual table, SQL functions, backing storage, tests, build integration)

---

## Overview

Add a Redis-style sorted set extension to SQLite as `ext/misc/sortedset.c`. A sorted set is a collection where each element has a unique **member** (text) and a **score** (real number). Elements are ordered by score, with lexicographic member ordering as tiebreaker.

The extension provides:
1. A **virtual table module** (`sortedset`) for full CRUD via standard SQL
2. **Scalar SQL functions** for quick lookups without cursors
3. Backing storage in regular SQLite tables with B-tree indexes

---

## Design Decisions

### Storage Model
Each virtual sorted set is backed by a real SQLite table:
```sql
CREATE TABLE "_zset_<name>" (
  member TEXT PRIMARY KEY,
  score  REAL NOT NULL
);
CREATE INDEX "_zset_<name>_score_idx" ON "_zset_<name>"(score, member);
```

This leverages SQLite's existing B-tree indexing for O(log n) inserts, lookups, and ordered scans. The composite index `(score, member)` provides natural sorted-set ordering.

### Why a Virtual Table (not just functions)
- Standard SQL syntax for queries: `SELECT`, `INSERT`, `UPDATE`, `DELETE`
- Query planner integration via `xBestIndex` for efficient range scans
- Natural `ORDER BY`, `WHERE`, `LIMIT` support
- Composable with JOINs and subqueries

### Internal Data Structure
No custom C data structure beyond the vtab/cursor structs. The SQLite B-tree indexes on the backing table handle all ordering. The cursor wraps a prepared statement that queries the backing table.

---

## SQL Interface

### Virtual Table Usage

```sql
-- Create a sorted set
CREATE VIRTUAL TABLE leaderboard USING sortedset;

-- ZADD: Insert or replace members
INSERT INTO leaderboard(member, score) VALUES('alice', 100.0);
INSERT OR REPLACE INTO leaderboard(member, score) VALUES('alice', 150.0);

-- ZREM: Remove a member
DELETE FROM leaderboard WHERE member = 'alice';

-- ZRANGE: Get all members ordered by score (ascending by default)
SELECT member, score FROM leaderboard;

-- ZRANGEBYSCORE: Get members in a score range
SELECT member, score FROM leaderboard WHERE score BETWEEN 50.0 AND 200.0;

-- ZREVRANGE: Descending order
SELECT member, score FROM leaderboard ORDER BY score DESC;

-- ZRANK: Get rank (via rank column)
SELECT member, score, rank FROM leaderboard WHERE member = 'bob';

-- ZSCORE: Get score for a member
SELECT score FROM leaderboard WHERE member = 'alice';

-- ZCARD: Count
SELECT count(*) FROM leaderboard;

-- ZCOUNT: Count in range
SELECT count(*) FROM leaderboard WHERE score BETWEEN 10.0 AND 100.0;

-- Update score
UPDATE leaderboard SET score = score + 10 WHERE member = 'alice';

-- Drop the sorted set (removes backing table too)
DROP TABLE leaderboard;
```

### Scalar SQL Functions

For convenience when you need a quick lookup without a full query:

| Function | Description | Example |
|----------|-------------|---------|
| `zset_score(name, member)` | Get score of member | `SELECT zset_score('leaderboard', 'alice')` |
| `zset_rank(name, member)` | Get 0-based rank of member | `SELECT zset_rank('leaderboard', 'alice')` |
| `zset_card(name)` | Get cardinality of set | `SELECT zset_card('leaderboard')` |
| `zset_count(name, min, max)` | Count members in score range | `SELECT zset_count('leaderboard', 10.0, 100.0)` |

These functions execute SQL against the backing table directly via prepared statements.

---

## File Plan

### New Files

#### 1. `ext/misc/sortedset.c` (~800-1000 lines)

Main extension source file. Structure:

```
┌─────────────────────────────────────┐
│ Headers & Macros                     │
│   #include "sqlite3ext.h"           │
│   SQLITE_EXTENSION_INIT1            │
├─────────────────────────────────────┤
│ Type Definitions                     │
│   sortedset_vtab (extends sqlite3_vtab)  │
│   sortedset_cursor (extends sqlite3_vtab_cursor) │
├─────────────────────────────────────┤
│ Helper Functions                     │
│   zsetBackingTableName()            │
│   zsetCreateBackingTable()          │
│   zsetDropBackingTable()            │
│   zsetPrepareCursor()              │
├─────────────────────────────────────┤
│ Virtual Table Methods                │
│   xCreate, xConnect, xDisconnect, xDestroy │
│   xOpen, xClose                     │
│   xBestIndex                        │
│   xFilter, xNext, xEof             │
│   xColumn, xRowid                   │
│   xUpdate                           │
├─────────────────────────────────────┤
│ sqlite3_module Definition            │
├─────────────────────────────────────┤
│ Scalar Function Implementations      │
│   zsetScoreFunc()                   │
│   zsetRankFunc()                    │
│   zsetCardFunc()                    │
│   zsetCountFunc()                   │
├─────────────────────────────────────┤
│ Extension Init                       │
│   sqlite3_sortedset_init()          │
└─────────────────────────────────────┘
```

#### 2. `test/sortedset01.test` (~300-400 lines)

TCL test file covering all operations.

### Modified Files

#### 3. `main.mk`
Add `$(TOP)/ext/misc/sortedset.c` to the `SRC.static_extension` list (~line 790).

#### 4. `Makefile.msc`
Add sortedset.c to the Windows build extension list (follow the pattern of other ext/misc entries).

---

## Implementation Steps

### Step 1: Scaffold the Extension File

Create `ext/misc/sortedset.c` with:
- Standard extension boilerplate (`sqlite3ext.h`, `SQLITE_EXTENSION_INIT1/2`)
- Type definitions for `sortedset_vtab` and `sortedset_cursor`
- The `sqlite3_module` struct with all method pointers
- The `sqlite3_sortedset_init` entry point

**Reference:** `ext/misc/templatevtab.c` for minimal skeleton, `ext/misc/series.c` for full virtual table pattern.

#### sortedset_vtab struct
```c
typedef struct sortedset_vtab {
  sqlite3_vtab base;          /* Base class - must be first */
  sqlite3 *db;                /* Database connection */
  char *zTableName;           /* Name of this virtual table */
  char *zBackingTable;        /* Name of backing real table: "_zset_<name>" */
  char *zScoreIndex;          /* Name of score index */
} sortedset_vtab;
```

#### sortedset_cursor struct
```c
typedef struct sortedset_cursor {
  sqlite3_vtab_cursor base;   /* Base class - must be first */
  sqlite3_stmt *pStmt;        /* Prepared statement for current scan */
  sqlite3_int64 iRowid;       /* Current rowid (row counter) */
  int bEof;                   /* True if past last row */
} sortedset_cursor;
```

### Step 2: Implement Lifecycle Methods

#### xCreate
1. Parse virtual table arguments (just the table name from `argv[2]`)
2. Construct backing table name: `"_zset_" + argv[2]`
3. Execute `CREATE TABLE IF NOT EXISTS` and `CREATE INDEX IF NOT EXISTS` on backing table
4. Call `sqlite3_declare_vtab(db, "CREATE TABLE x(member TEXT, score REAL, rank INTEGER HIDDEN)")` — rank is HIDDEN since it's computed and expensive
5. Allocate and populate `sortedset_vtab`
6. Call `sqlite3_vtab_config(db, SQLITE_VTAB_INNOCUOUS)`

**Note:** `rank` is HIDDEN because computing it requires a subquery for every row. Users opt in by explicitly selecting it.

#### xConnect
Same as xCreate but skip the `CREATE TABLE` (backing table already exists).

#### xDisconnect
Free the vtab struct and its string members.

#### xDestroy
Execute `DROP TABLE IF EXISTS` on the backing table, then free.

### Step 3: Implement Cursor Lifecycle

#### xOpen
Allocate a `sortedset_cursor`, zero-initialize, return it.

#### xClose
Finalize `pStmt` if not NULL, free the cursor.

### Step 4: Implement xBestIndex

This is the query planner integration. Encode query plan in `idxNum` bitmask:

| Bit | Meaning |
|-----|---------|
| 0x01 | `member = ?` constraint |
| 0x02 | `score = ?` constraint |
| 0x04 | `score >= ?` constraint |
| 0x08 | `score > ?` constraint |
| 0x10 | `score <= ?` constraint |
| 0x20 | `score < ?` constraint |
| 0x40 | ORDER BY score ASC (natural) |
| 0x80 | ORDER BY score DESC |

Cost estimation:
- `member = ?` → estimatedRows = 1, cost = 1.0
- `score = ?` → estimatedRows = 10, cost = 10.0
- Score range → estimatedRows = 100, cost = 50.0
- Full scan → estimatedRows = 1000000, cost = 1000000.0

Set `orderByConsumed = 1` when ORDER BY score matches the scan direction.

For each usable constraint on member or score columns, set `aConstraintUsage[i].argvIndex` and `aConstraintUsage[i].omit = 1`.

### Step 5: Implement xFilter

Build a SQL query against the backing table based on `idxNum`:

```c
/* Base query */
"SELECT rowid, member, score FROM \"%w\" "

/* Add WHERE clauses based on idxNum bits */
if (idxNum & 0x01) → "WHERE member = ?1"
if (idxNum & 0x02) → "WHERE score = ?N"
if (idxNum & 0x04) → "WHERE score >= ?N"
if (idxNum & 0x08) → "WHERE score > ?N"
if (idxNum & 0x10) → "AND score <= ?N"  (combine with >= for BETWEEN)
if (idxNum & 0x20) → "AND score < ?N"

/* Add ORDER BY */
if (idxNum & 0x80) → "ORDER BY score DESC, member DESC"
else               → "ORDER BY score ASC, member ASC"
```

Prepare the statement, bind constraint values from `argv[]`, step to first row.

Use `sqlite3_mprintf` for safe string formatting (the `%w` format escapes identifiers).

### Step 6: Implement xNext, xEof, xColumn, xRowid

#### xNext
Call `sqlite3_step(pStmt)`. If result is `SQLITE_ROW`, increment `iRowid`. If `SQLITE_DONE`, set `bEof = 1`.

#### xEof
Return `pCur->bEof`.

#### xColumn
- Column 0 (member): `sqlite3_result_value(ctx, sqlite3_column_value(pStmt, 1))`
- Column 1 (score): `sqlite3_result_value(ctx, sqlite3_column_value(pStmt, 2))`
- Column 2 (rank, HIDDEN): Execute a subquery:
  ```sql
  SELECT COUNT(*) FROM "_zset_<name>"
  WHERE score < ?1 OR (score = ?1 AND member < ?2)
  ```
  Bind current score and member, return count as rank (0-based).

#### xRowid
Return `pCur->iRowid` (sequential counter) or the backing table's rowid from `sqlite3_column_int64(pStmt, 0)`.

### Step 7: Implement xUpdate (INSERT/DELETE/UPDATE)

The `xUpdate` method signature: `int xUpdate(sqlite3_vtab *pVTab, int argc, sqlite3_value **argv, sqlite3_int64 *pRowid)`

Three cases based on `argc` and `argv[0]`:

#### DELETE (`argc == 1`)
```sql
DELETE FROM "_zset_<name>" WHERE rowid = ?1
```
Bind `argv[0]` as the rowid.

#### INSERT (`argc > 1 && argv[0] == NULL`)
```sql
INSERT OR REPLACE INTO "_zset_<name>"(member, score) VALUES(?1, ?2)
```
Bind `argv[2]` (member) and `argv[3]` (score). Use `INSERT OR REPLACE` to handle ZADD semantics (upsert).
Set `*pRowid` to `sqlite3_last_insert_rowid(db)`.

#### UPDATE (`argc > 1 && argv[0] != NULL`)
```sql
UPDATE "_zset_<name>" SET member = ?1, score = ?2 WHERE rowid = ?3
```
Bind from argv appropriately.

**Conflict handling:** Call `sqlite3_vtab_on_conflict(db)` to check the conflict resolution strategy and adjust `INSERT` vs `INSERT OR REPLACE` vs `INSERT OR IGNORE` accordingly.

### Step 8: Implement Scalar Functions

Register these in `sqlite3_sortedset_init`:

#### zset_score(set_name, member) → REAL or NULL
```c
static void zsetScoreFunc(sqlite3_context *ctx, int argc, sqlite3_value **argv){
  /* Get db from sqlite3_context_db_handle(ctx) */
  /* Build: SELECT score FROM "_zset_%w" WHERE member = ?1 */
  /* Prepare, bind, step, return result or NULL */
}
```

#### zset_rank(set_name, member) → INTEGER or NULL
```c
static void zsetRankFunc(sqlite3_context *ctx, int argc, sqlite3_value **argv){
  /* First get the member's score */
  /* Then: SELECT COUNT(*) FROM "_zset_%w" WHERE score < ?1 OR (score = ?1 AND member < ?2) */
  /* Return count as 0-based rank */
}
```

#### zset_card(set_name) → INTEGER
```c
static void zsetCardFunc(sqlite3_context *ctx, int argc, sqlite3_value **argv){
  /* SELECT COUNT(*) FROM "_zset_%w" */
}
```

#### zset_count(set_name, min_score, max_score) → INTEGER
```c
static void zsetCountFunc(sqlite3_context *ctx, int argc, sqlite3_value **argv){
  /* SELECT COUNT(*) FROM "_zset_%w" WHERE score >= ?1 AND score <= ?2 */
}
```

All scalar functions must:
- Use `sqlite3_context_db_handle(ctx)` to get the db connection
- Validate that the backing table exists (return NULL or error if not)
- Properly finalize prepared statements
- Use `sqlite3_mprintf` with `%w` for safe identifier escaping

### Step 9: Extension Init Function

```c
#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_sortedset_init(
  sqlite3 *db,
  char **pzErrMsg,
  const sqlite3_api_routines *pApi
){
  int rc = SQLITE_OK;
  SQLITE_EXTENSION_INIT2(pApi);

#ifndef SQLITE_OMIT_VIRTUALTABLE
  rc = sqlite3_create_module(db, "sortedset", &sortedsetModule, 0);
  if( rc==SQLITE_OK ){
    rc = sqlite3_create_function(db, "zset_score", 2, SQLITE_UTF8|SQLITE_INNOCUOUS, 0, zsetScoreFunc, 0, 0);
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3_create_function(db, "zset_rank", 2, SQLITE_UTF8|SQLITE_INNOCUOUS, 0, zsetRankFunc, 0, 0);
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3_create_function(db, "zset_card", 1, SQLITE_UTF8|SQLITE_INNOCUOUS, 0, zsetCardFunc, 0, 0);
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3_create_function(db, "zset_count", 3, SQLITE_UTF8|SQLITE_INNOCUOUS, 0, zsetCountFunc, 0, 0);
  }
#endif

  return rc;
}
```

### Step 10: Build System Integration

#### main.mk (~line 790)
Add to `SRC.static_extension`:
```makefile
  $(TOP)/ext/misc/sortedset.c \
```

#### Makefile.msc
Find the equivalent extension list and add `sortedset.c`.

### Step 11: Write Tests

Create `test/sortedset01.test` with these test groups:

1. **Basic creation and destruction**
   - Create virtual table
   - Verify backing table exists
   - Drop virtual table
   - Verify backing table is gone

2. **INSERT operations (ZADD semantics)**
   - Insert single member
   - Insert multiple members
   - Insert duplicate member (should update score)
   - INSERT OR REPLACE
   - INSERT OR IGNORE
   - NULL member/score handling (should error)

3. **SELECT operations (ZRANGE/query)**
   - Select all (verify score ordering)
   - Select with score range (BETWEEN)
   - Select specific member
   - Select with LIMIT/OFFSET
   - Select with ORDER BY score DESC
   - Verify rank computation

4. **UPDATE operations**
   - Update score of existing member
   - Verify re-ordering after score change

5. **DELETE operations (ZREM)**
   - Delete by member
   - Delete nonexistent member (no error)

6. **Scalar functions**
   - `zset_score` for existing/nonexistent member
   - `zset_rank` correctness with ties
   - `zset_card` accuracy
   - `zset_count` with various ranges
   - Functions on nonexistent set (should return NULL)

7. **Edge cases**
   - Empty set operations
   - Very large scores (inf, -inf)
   - Unicode members
   - Members with special characters (quotes, backslashes)
   - Score ties (verify lexicographic ordering)
   - Large sets (1000+ members for performance sanity)

8. **Multiple sorted sets**
   - Create multiple independent sets
   - Verify no cross-contamination

---

## Risk Areas & Mitigations

| Risk | Mitigation |
|------|------------|
| SQL injection in backing table name | Use `sqlite3_mprintf("%w", ...)` for all identifier escaping |
| Rank computation performance on large sets | Make rank a HIDDEN column (opt-in); document O(n) cost |
| Memory leaks in cursor statements | Always finalize in xClose; check all error paths |
| Conflict handling complexity in xUpdate | Use `sqlite3_vtab_on_conflict()` to respect INSERT OR REPLACE/IGNORE |
| Backing table name collision | Prefix with `_zset_` to minimize collision risk |

---

## Implementation Order (for the implementing agent)

1. Create `ext/misc/sortedset.c` with all boilerplate and types
2. Implement xCreate/xConnect/xDisconnect/xDestroy (lifecycle)
3. Implement xOpen/xClose (cursor lifecycle)
4. Implement xBestIndex (query planning)
5. Implement xFilter/xNext/xEof/xColumn/xRowid (read path)
6. Implement xUpdate (write path: INSERT/DELETE/UPDATE)
7. Implement scalar functions (zset_score, zset_rank, zset_card, zset_count)
8. Write extension init function and register everything
9. Update build files (main.mk, Makefile.msc)
10. Write test file (test/sortedset01.test)
11. Build and run tests

---

## Key Reference Files

- `ext/misc/series.c` — Best pattern for virtual table with xBestIndex optimization
- `ext/misc/templatevtab.c` — Minimal virtual table skeleton
- `ext/misc/csv.c` — Shows xCreate vs xConnect distinction, complex parameter handling
- `ext/misc/closure.c` — Shows writable virtual table (xUpdate) with backing table pattern
- `main.mk:~790` — Static extension list
- `test/csv01.test` — Test file pattern to follow
