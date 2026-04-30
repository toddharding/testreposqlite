/*
** 2026-04-30
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
**
** This file implements Redis-style sorted sets as a SQLite loadable
** extension.  Each sorted set is identified by a string key. Members
** are unique strings within a key, each associated with a double-precision
** floating-point score.  Members are ordered by score (ascending), with
** lexicographic tiebreaking on member name.
**
** Scalar functions: zadd, zrem, zscore, zrank, zrevrank, zcard, zcount,
** zincrby.
**
** Virtual table: zrange (eponymous, read-only) for range queries by
** rank or score.
**
** Data is persisted in a backing table (_sortedsets) created lazily.
*/
#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1
#include <assert.h>
#include <string.h>
#include <stdlib.h>

/*
** Ensure the backing table and index exist.  This is called lazily
** before any operation that touches the _sortedsets table.
** Returns SQLITE_OK on success.
*/
static int sortedsets_ensure_table(sqlite3 *db){
  int rc;
  rc = sqlite3_exec(db,
    "CREATE TABLE IF NOT EXISTS _sortedsets("
    "  key   TEXT NOT NULL,"
    "  member TEXT NOT NULL,"
    "  score  REAL NOT NULL,"
    "  PRIMARY KEY(key, member)"
    ");"
    "CREATE INDEX IF NOT EXISTS _sortedsets_score "
    "  ON _sortedsets(key, score, member);",
    0, 0, 0);
  return rc;
}

/*
** zadd(key, score, member) -> INTEGER (1=new, 0=updated)
**
** Add or update a member's score in the sorted set.
*/
static void zadd_func(
  sqlite3_context *ctx,
  int argc,
  sqlite3_value **argv
){
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  const char *key;
  const char *member;
  double score;
  sqlite3_stmt *pStmt = 0;
  int rc;
  int existed;

  assert( argc==3 );
  if( sqlite3_value_type(argv[0])==SQLITE_NULL
   || sqlite3_value_type(argv[2])==SQLITE_NULL
  ){
    sqlite3_result_null(ctx);
    return;
  }

  rc = sortedsets_ensure_table(db);
  if( rc!=SQLITE_OK ){
    sqlite3_result_error_code(ctx, rc);
    return;
  }

  key = (const char *)sqlite3_value_text(argv[0]);
  score = sqlite3_value_double(argv[1]);
  member = (const char *)sqlite3_value_text(argv[2]);

  /* Check if the member already exists */
  rc = sqlite3_prepare_v2(db,
    "SELECT 1 FROM _sortedsets WHERE key=?1 AND member=?2",
    -1, &pStmt, 0);
  if( rc!=SQLITE_OK ){
    sqlite3_result_error_code(ctx, rc);
    return;
  }
  sqlite3_bind_text(pStmt, 1, key, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(pStmt, 2, member, -1, SQLITE_TRANSIENT);
  existed = (sqlite3_step(pStmt)==SQLITE_ROW);
  sqlite3_finalize(pStmt);

  /* Insert or replace */
  rc = sqlite3_prepare_v2(db,
    "INSERT OR REPLACE INTO _sortedsets(key,member,score) VALUES(?1,?2,?3)",
    -1, &pStmt, 0);
  if( rc!=SQLITE_OK ){
    sqlite3_result_error_code(ctx, rc);
    return;
  }
  sqlite3_bind_text(pStmt, 1, key, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(pStmt, 2, member, -1, SQLITE_TRANSIENT);
  sqlite3_bind_double(pStmt, 3, score);
  rc = sqlite3_step(pStmt);
  sqlite3_finalize(pStmt);

  if( rc!=SQLITE_DONE ){
    sqlite3_result_error_code(ctx, rc);
    return;
  }
  sqlite3_result_int(ctx, existed ? 0 : 1);
}

/*
** zrem(key, member) -> INTEGER (0 or 1)
**
** Remove a member from the sorted set.
*/
static void zrem_func(
  sqlite3_context *ctx,
  int argc,
  sqlite3_value **argv
){
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  const char *key;
  const char *member;
  sqlite3_stmt *pStmt = 0;
  int rc;

  assert( argc==2 );
  if( sqlite3_value_type(argv[0])==SQLITE_NULL
   || sqlite3_value_type(argv[1])==SQLITE_NULL
  ){
    sqlite3_result_null(ctx);
    return;
  }

  rc = sortedsets_ensure_table(db);
  if( rc!=SQLITE_OK ){
    sqlite3_result_error_code(ctx, rc);
    return;
  }

  key = (const char *)sqlite3_value_text(argv[0]);
  member = (const char *)sqlite3_value_text(argv[1]);

  rc = sqlite3_prepare_v2(db,
    "DELETE FROM _sortedsets WHERE key=?1 AND member=?2",
    -1, &pStmt, 0);
  if( rc!=SQLITE_OK ){
    sqlite3_result_error_code(ctx, rc);
    return;
  }
  sqlite3_bind_text(pStmt, 1, key, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(pStmt, 2, member, -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(pStmt);
  sqlite3_finalize(pStmt);

  if( rc!=SQLITE_DONE ){
    sqlite3_result_error_code(ctx, rc);
    return;
  }
  sqlite3_result_int(ctx, sqlite3_changes(db));
}

/*
** zscore(key, member) -> REAL or NULL
**
** Return the score of a member, or NULL if not present.
*/
static void zscore_func(
  sqlite3_context *ctx,
  int argc,
  sqlite3_value **argv
){
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  const char *key;
  const char *member;
  sqlite3_stmt *pStmt = 0;
  int rc;

  assert( argc==2 );
  if( sqlite3_value_type(argv[0])==SQLITE_NULL
   || sqlite3_value_type(argv[1])==SQLITE_NULL
  ){
    sqlite3_result_null(ctx);
    return;
  }

  rc = sortedsets_ensure_table(db);
  if( rc!=SQLITE_OK ){
    sqlite3_result_error_code(ctx, rc);
    return;
  }

  key = (const char *)sqlite3_value_text(argv[0]);
  member = (const char *)sqlite3_value_text(argv[1]);

  rc = sqlite3_prepare_v2(db,
    "SELECT score FROM _sortedsets WHERE key=?1 AND member=?2",
    -1, &pStmt, 0);
  if( rc!=SQLITE_OK ){
    sqlite3_result_error_code(ctx, rc);
    return;
  }
  sqlite3_bind_text(pStmt, 1, key, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(pStmt, 2, member, -1, SQLITE_TRANSIENT);
  if( sqlite3_step(pStmt)==SQLITE_ROW ){
    sqlite3_result_double(ctx, sqlite3_column_double(pStmt, 0));
  }else{
    sqlite3_result_null(ctx);
  }
  sqlite3_finalize(pStmt);
}

/*
** zrank(key, member) -> INTEGER or NULL
**
** 0-based rank by ascending score.  NULL if member absent.
*/
static void zrank_func(
  sqlite3_context *ctx,
  int argc,
  sqlite3_value **argv
){
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  const char *key;
  const char *member;
  sqlite3_stmt *pStmt = 0;
  int rc;
  double memberScore;

  assert( argc==2 );
  if( sqlite3_value_type(argv[0])==SQLITE_NULL
   || sqlite3_value_type(argv[1])==SQLITE_NULL
  ){
    sqlite3_result_null(ctx);
    return;
  }

  rc = sortedsets_ensure_table(db);
  if( rc!=SQLITE_OK ){
    sqlite3_result_error_code(ctx, rc);
    return;
  }

  key = (const char *)sqlite3_value_text(argv[0]);
  member = (const char *)sqlite3_value_text(argv[1]);

  /* First check if the member exists and get its score */
  rc = sqlite3_prepare_v2(db,
    "SELECT score FROM _sortedsets WHERE key=?1 AND member=?2",
    -1, &pStmt, 0);
  if( rc!=SQLITE_OK ){
    sqlite3_result_error_code(ctx, rc);
    return;
  }
  sqlite3_bind_text(pStmt, 1, key, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(pStmt, 2, member, -1, SQLITE_TRANSIENT);
  if( sqlite3_step(pStmt)!=SQLITE_ROW ){
    sqlite3_result_null(ctx);
    sqlite3_finalize(pStmt);
    return;
  }
  memberScore = sqlite3_column_double(pStmt, 0);
  sqlite3_finalize(pStmt);

  /* Count members that rank before this one */
  rc = sqlite3_prepare_v2(db,
    "SELECT COUNT(*) FROM _sortedsets "
    "WHERE key=?1 AND (score < ?3 OR (score = ?3 AND member < ?2))",
    -1, &pStmt, 0);
  if( rc!=SQLITE_OK ){
    sqlite3_result_error_code(ctx, rc);
    return;
  }
  sqlite3_bind_text(pStmt, 1, key, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(pStmt, 2, member, -1, SQLITE_TRANSIENT);
  sqlite3_bind_double(pStmt, 3, memberScore);
  if( sqlite3_step(pStmt)==SQLITE_ROW ){
    sqlite3_result_int64(ctx, sqlite3_column_int64(pStmt, 0));
  }else{
    sqlite3_result_null(ctx);
  }
  sqlite3_finalize(pStmt);
}

/*
** zrevrank(key, member) -> INTEGER or NULL
**
** 0-based rank by descending score.  NULL if member absent.
*/
static void zrevrank_func(
  sqlite3_context *ctx,
  int argc,
  sqlite3_value **argv
){
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  const char *key;
  const char *member;
  sqlite3_stmt *pStmt = 0;
  int rc;
  double memberScore;

  assert( argc==2 );
  if( sqlite3_value_type(argv[0])==SQLITE_NULL
   || sqlite3_value_type(argv[1])==SQLITE_NULL
  ){
    sqlite3_result_null(ctx);
    return;
  }

  rc = sortedsets_ensure_table(db);
  if( rc!=SQLITE_OK ){
    sqlite3_result_error_code(ctx, rc);
    return;
  }

  key = (const char *)sqlite3_value_text(argv[0]);
  member = (const char *)sqlite3_value_text(argv[1]);

  /* First check if the member exists and get its score */
  rc = sqlite3_prepare_v2(db,
    "SELECT score FROM _sortedsets WHERE key=?1 AND member=?2",
    -1, &pStmt, 0);
  if( rc!=SQLITE_OK ){
    sqlite3_result_error_code(ctx, rc);
    return;
  }
  sqlite3_bind_text(pStmt, 1, key, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(pStmt, 2, member, -1, SQLITE_TRANSIENT);
  if( sqlite3_step(pStmt)!=SQLITE_ROW ){
    sqlite3_result_null(ctx);
    sqlite3_finalize(pStmt);
    return;
  }
  memberScore = sqlite3_column_double(pStmt, 0);
  sqlite3_finalize(pStmt);

  /* Count members that rank before this one in reverse order */
  rc = sqlite3_prepare_v2(db,
    "SELECT COUNT(*) FROM _sortedsets "
    "WHERE key=?1 AND (score > ?3 OR (score = ?3 AND member > ?2))",
    -1, &pStmt, 0);
  if( rc!=SQLITE_OK ){
    sqlite3_result_error_code(ctx, rc);
    return;
  }
  sqlite3_bind_text(pStmt, 1, key, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(pStmt, 2, member, -1, SQLITE_TRANSIENT);
  sqlite3_bind_double(pStmt, 3, memberScore);
  if( sqlite3_step(pStmt)==SQLITE_ROW ){
    sqlite3_result_int64(ctx, sqlite3_column_int64(pStmt, 0));
  }else{
    sqlite3_result_null(ctx);
  }
  sqlite3_finalize(pStmt);
}

/*
** zcard(key) -> INTEGER
**
** Number of members in the sorted set.
*/
static void zcard_func(
  sqlite3_context *ctx,
  int argc,
  sqlite3_value **argv
){
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  const char *key;
  sqlite3_stmt *pStmt = 0;
  int rc;

  assert( argc==1 );
  if( sqlite3_value_type(argv[0])==SQLITE_NULL ){
    sqlite3_result_int(ctx, 0);
    return;
  }

  rc = sortedsets_ensure_table(db);
  if( rc!=SQLITE_OK ){
    sqlite3_result_error_code(ctx, rc);
    return;
  }

  key = (const char *)sqlite3_value_text(argv[0]);

  rc = sqlite3_prepare_v2(db,
    "SELECT COUNT(*) FROM _sortedsets WHERE key=?1",
    -1, &pStmt, 0);
  if( rc!=SQLITE_OK ){
    sqlite3_result_error_code(ctx, rc);
    return;
  }
  sqlite3_bind_text(pStmt, 1, key, -1, SQLITE_TRANSIENT);
  if( sqlite3_step(pStmt)==SQLITE_ROW ){
    sqlite3_result_int64(ctx, sqlite3_column_int64(pStmt, 0));
  }else{
    sqlite3_result_int(ctx, 0);
  }
  sqlite3_finalize(pStmt);
}

/*
** zcount(key, min_score, max_score) -> INTEGER
**
** Count members with score in [min, max].
*/
static void zcount_func(
  sqlite3_context *ctx,
  int argc,
  sqlite3_value **argv
){
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  const char *key;
  double minScore, maxScore;
  sqlite3_stmt *pStmt = 0;
  int rc;

  assert( argc==3 );
  if( sqlite3_value_type(argv[0])==SQLITE_NULL ){
    sqlite3_result_int(ctx, 0);
    return;
  }

  rc = sortedsets_ensure_table(db);
  if( rc!=SQLITE_OK ){
    sqlite3_result_error_code(ctx, rc);
    return;
  }

  key = (const char *)sqlite3_value_text(argv[0]);
  minScore = sqlite3_value_double(argv[1]);
  maxScore = sqlite3_value_double(argv[2]);

  rc = sqlite3_prepare_v2(db,
    "SELECT COUNT(*) FROM _sortedsets "
    "WHERE key=?1 AND score>=?2 AND score<=?3",
    -1, &pStmt, 0);
  if( rc!=SQLITE_OK ){
    sqlite3_result_error_code(ctx, rc);
    return;
  }
  sqlite3_bind_text(pStmt, 1, key, -1, SQLITE_TRANSIENT);
  sqlite3_bind_double(pStmt, 2, minScore);
  sqlite3_bind_double(pStmt, 3, maxScore);
  if( sqlite3_step(pStmt)==SQLITE_ROW ){
    sqlite3_result_int64(ctx, sqlite3_column_int64(pStmt, 0));
  }else{
    sqlite3_result_int(ctx, 0);
  }
  sqlite3_finalize(pStmt);
}

/*
** zincrby(key, increment, member) -> REAL
**
** Increment member's score by increment.  Returns new score.
** Creates member with increment as score if absent.
*/
static void zincrby_func(
  sqlite3_context *ctx,
  int argc,
  sqlite3_value **argv
){
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  const char *key;
  const char *member;
  double increment;
  sqlite3_stmt *pStmt = 0;
  int rc;

  assert( argc==3 );
  if( sqlite3_value_type(argv[0])==SQLITE_NULL
   || sqlite3_value_type(argv[2])==SQLITE_NULL
  ){
    sqlite3_result_null(ctx);
    return;
  }

  rc = sortedsets_ensure_table(db);
  if( rc!=SQLITE_OK ){
    sqlite3_result_error_code(ctx, rc);
    return;
  }

  key = (const char *)sqlite3_value_text(argv[0]);
  increment = sqlite3_value_double(argv[1]);
  member = (const char *)sqlite3_value_text(argv[2]);

  /* Use INSERT ... ON CONFLICT to atomically upsert */
  rc = sqlite3_prepare_v2(db,
    "INSERT INTO _sortedsets(key,member,score) VALUES(?1,?2,?3) "
    "ON CONFLICT(key,member) DO UPDATE SET score=score+?3",
    -1, &pStmt, 0);
  if( rc!=SQLITE_OK ){
    sqlite3_result_error_code(ctx, rc);
    return;
  }
  sqlite3_bind_text(pStmt, 1, key, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(pStmt, 2, member, -1, SQLITE_TRANSIENT);
  sqlite3_bind_double(pStmt, 3, increment);
  rc = sqlite3_step(pStmt);
  sqlite3_finalize(pStmt);

  if( rc!=SQLITE_DONE ){
    sqlite3_result_error_code(ctx, rc);
    return;
  }

  /* Retrieve and return the new score */
  rc = sqlite3_prepare_v2(db,
    "SELECT score FROM _sortedsets WHERE key=?1 AND member=?2",
    -1, &pStmt, 0);
  if( rc!=SQLITE_OK ){
    sqlite3_result_error_code(ctx, rc);
    return;
  }
  sqlite3_bind_text(pStmt, 1, key, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(pStmt, 2, member, -1, SQLITE_TRANSIENT);
  if( sqlite3_step(pStmt)==SQLITE_ROW ){
    sqlite3_result_double(ctx, sqlite3_column_double(pStmt, 0));
  }else{
    sqlite3_result_null(ctx);
  }
  sqlite3_finalize(pStmt);
}

/* -----------------------------------------------------------------------
** Virtual table: zrange
** ----------------------------------------------------------------------- */

#ifndef SQLITE_OMIT_VIRTUALTABLE

/* Column indices for the zrange virtual table */
#define ZRANGE_COL_MEMBER  0
#define ZRANGE_COL_SCORE   1
#define ZRANGE_COL_KEY     2   /* HIDDEN */
#define ZRANGE_COL_START   3   /* HIDDEN */
#define ZRANGE_COL_STOP    4   /* HIDDEN */
#define ZRANGE_COL_MIN     5   /* HIDDEN */
#define ZRANGE_COL_MAX     6   /* HIDDEN */
#define ZRANGE_COL_REV     7   /* HIDDEN */

typedef struct zrange_vtab zrange_vtab;
struct zrange_vtab {
  sqlite3_vtab base;
  sqlite3 *db;
};

typedef struct zrange_cursor zrange_cursor;
struct zrange_cursor {
  sqlite3_vtab_cursor base;
  sqlite3_stmt *pStmt;   /* Current query */
  int bEof;              /* True when done */
  sqlite3_int64 iRowid;  /* Current rowid */
};

static int zrangeConnect(
  sqlite3 *db,
  void *pAux,
  int argc, const char *const*argv,
  sqlite3_vtab **ppVtab,
  char **pzErr
){
  zrange_vtab *pNew;
  int rc;

  (void)pAux;
  (void)argc;
  (void)argv;
  (void)pzErr;

  rc = sqlite3_declare_vtab(db,
    "CREATE TABLE x("
    "  member TEXT,"
    "  score REAL,"
    "  key TEXT HIDDEN,"
    "  start INT HIDDEN,"
    "  stop INT HIDDEN,"
    "  min REAL HIDDEN,"
    "  max REAL HIDDEN,"
    "  rev INT HIDDEN"
    ")");
  if( rc!=SQLITE_OK ) return rc;

  pNew = sqlite3_malloc64(sizeof(*pNew));
  if( pNew==0 ) return SQLITE_NOMEM;
  memset(pNew, 0, sizeof(*pNew));
  pNew->db = db;
  *ppVtab = &pNew->base;
  return SQLITE_OK;
}

static int zrangeDisconnect(sqlite3_vtab *pVtab){
  sqlite3_free(pVtab);
  return SQLITE_OK;
}

static int zrangeOpen(sqlite3_vtab *pVtab, sqlite3_vtab_cursor **ppCursor){
  zrange_cursor *pCur;
  (void)pVtab;
  pCur = sqlite3_malloc64(sizeof(*pCur));
  if( pCur==0 ) return SQLITE_NOMEM;
  memset(pCur, 0, sizeof(*pCur));
  *ppCursor = &pCur->base;
  return SQLITE_OK;
}

static int zrangeClose(sqlite3_vtab_cursor *cur){
  zrange_cursor *pCur = (zrange_cursor *)cur;
  if( pCur->pStmt ){
    sqlite3_finalize(pCur->pStmt);
  }
  sqlite3_free(pCur);
  return SQLITE_OK;
}

static int zrangeNext(sqlite3_vtab_cursor *cur){
  zrange_cursor *pCur = (zrange_cursor *)cur;
  if( sqlite3_step(pCur->pStmt)==SQLITE_ROW ){
    pCur->iRowid++;
  }else{
    pCur->bEof = 1;
  }
  return SQLITE_OK;
}

static int zrangeEof(sqlite3_vtab_cursor *cur){
  return ((zrange_cursor *)cur)->bEof;
}

static int zrangeColumn(
  sqlite3_vtab_cursor *cur,
  sqlite3_context *ctx,
  int iCol
){
  zrange_cursor *pCur = (zrange_cursor *)cur;
  switch( iCol ){
    case ZRANGE_COL_MEMBER:
      sqlite3_result_value(ctx, sqlite3_column_value(pCur->pStmt, 0));
      break;
    case ZRANGE_COL_SCORE:
      sqlite3_result_value(ctx, sqlite3_column_value(pCur->pStmt, 1));
      break;
    default:
      sqlite3_result_null(ctx);
      break;
  }
  return SQLITE_OK;
}

static int zrangeRowid(sqlite3_vtab_cursor *cur, sqlite_int64 *pRowid){
  *pRowid = ((zrange_cursor *)cur)->iRowid;
  return SQLITE_OK;
}

/*
** idxNum encoding:
**   0x01 - key= constraint present
**   0x02 - start= constraint present
**   0x04 - stop= constraint present
**   0x08 - min= constraint present
**   0x10 - max= constraint present
**   0x20 - rev= constraint present
*/
static int zrangeBestIndex(
  sqlite3_vtab *pVTab,
  sqlite3_index_info *pIdxInfo
){
  int idxNum = 0;
  int nArg = 0;
  int keyIdx = -1;
  int startIdx = -1, stopIdx = -1;
  int minIdx = -1, maxIdx = -1;
  int revIdx = -1;
  int i;
  const struct sqlite3_index_constraint *pC;

  (void)pVTab;

  pC = pIdxInfo->aConstraint;
  for(i=0; i<pIdxInfo->nConstraint; i++, pC++){
    if( pC->usable==0 ) continue;
    if( pC->op!=SQLITE_INDEX_CONSTRAINT_EQ ) continue;
    switch( pC->iColumn ){
      case ZRANGE_COL_KEY:   keyIdx = i;   idxNum |= 0x01; break;
      case ZRANGE_COL_START: startIdx = i; idxNum |= 0x02; break;
      case ZRANGE_COL_STOP:  stopIdx = i;  idxNum |= 0x04; break;
      case ZRANGE_COL_MIN:   minIdx = i;   idxNum |= 0x08; break;
      case ZRANGE_COL_MAX:   maxIdx = i;   idxNum |= 0x10; break;
      case ZRANGE_COL_REV:   revIdx = i;   idxNum |= 0x20; break;
    }
  }

  /* key is required */
  if( (idxNum & 0x01)==0 ){
    pIdxInfo->estimatedCost = 1e15;
    pIdxInfo->estimatedRows = 1e15;
    return SQLITE_OK;
  }

  /* Assign argv indices in a fixed order: key, start, stop, min, max, rev */
  if( keyIdx>=0 ){
    pIdxInfo->aConstraintUsage[keyIdx].argvIndex = ++nArg;
    pIdxInfo->aConstraintUsage[keyIdx].omit = 1;
  }
  if( startIdx>=0 ){
    pIdxInfo->aConstraintUsage[startIdx].argvIndex = ++nArg;
    pIdxInfo->aConstraintUsage[startIdx].omit = 1;
  }
  if( stopIdx>=0 ){
    pIdxInfo->aConstraintUsage[stopIdx].argvIndex = ++nArg;
    pIdxInfo->aConstraintUsage[stopIdx].omit = 1;
  }
  if( minIdx>=0 ){
    pIdxInfo->aConstraintUsage[minIdx].argvIndex = ++nArg;
    pIdxInfo->aConstraintUsage[minIdx].omit = 1;
  }
  if( maxIdx>=0 ){
    pIdxInfo->aConstraintUsage[maxIdx].argvIndex = ++nArg;
    pIdxInfo->aConstraintUsage[maxIdx].omit = 1;
  }
  if( revIdx>=0 ){
    pIdxInfo->aConstraintUsage[revIdx].argvIndex = ++nArg;
    pIdxInfo->aConstraintUsage[revIdx].omit = 1;
  }

  pIdxInfo->idxNum = idxNum;
  pIdxInfo->estimatedCost = 100.0;
  pIdxInfo->estimatedRows = 100;
  return SQLITE_OK;
}

static int zrangeFilter(
  sqlite3_vtab_cursor *pVtabCursor,
  int idxNum, const char *idxStr,
  int argc, sqlite3_value **argv
){
  zrange_cursor *pCur = (zrange_cursor *)pVtabCursor;
  zrange_vtab *pVtab = (zrange_vtab *)(pVtabCursor->pVtab);
  sqlite3 *db = pVtab->db;
  const char *key;
  int iArg = 0;
  int hasStart = 0, hasStop = 0, hasMin = 0, hasMax = 0, rev = 0;
  sqlite3_int64 startVal = 0, stopVal = 0;
  double minVal = 0.0, maxVal = 0.0;
  int rc;

  (void)idxStr;
  (void)argc;

  if( pCur->pStmt ){
    sqlite3_finalize(pCur->pStmt);
    pCur->pStmt = 0;
  }
  pCur->bEof = 0;
  pCur->iRowid = 0;

  rc = sortedsets_ensure_table(db);
  if( rc!=SQLITE_OK ) return rc;

  if( (idxNum & 0x01)==0 ){
    /* No key constraint - return empty */
    pCur->bEof = 1;
    return SQLITE_OK;
  }
  key = (const char *)sqlite3_value_text(argv[iArg++]);

  if( idxNum & 0x02 ){
    startVal = sqlite3_value_int64(argv[iArg++]);
    hasStart = 1;
  }
  if( idxNum & 0x04 ){
    stopVal = sqlite3_value_int64(argv[iArg++]);
    hasStop = 1;
  }
  if( idxNum & 0x08 ){
    minVal = sqlite3_value_double(argv[iArg++]);
    hasMin = 1;
  }
  if( idxNum & 0x10 ){
    maxVal = sqlite3_value_double(argv[iArg++]);
    hasMax = 1;
  }
  if( idxNum & 0x20 ){
    rev = sqlite3_value_int(argv[iArg++]);
  }

  if( hasMin || hasMax ){
    /* Score-based range query */
    char *zSql;
    if( hasMin && hasMax ){
      if( rev ){
        zSql = sqlite3_mprintf(
          "SELECT member, score FROM _sortedsets "
          "WHERE key=?1 AND score>=?2 AND score<=?3 "
          "ORDER BY score DESC, member DESC");
      }else{
        zSql = sqlite3_mprintf(
          "SELECT member, score FROM _sortedsets "
          "WHERE key=?1 AND score>=?2 AND score<=?3 "
          "ORDER BY score ASC, member ASC");
      }
    }else if( hasMin ){
      if( rev ){
        zSql = sqlite3_mprintf(
          "SELECT member, score FROM _sortedsets "
          "WHERE key=?1 AND score>=?2 "
          "ORDER BY score DESC, member DESC");
      }else{
        zSql = sqlite3_mprintf(
          "SELECT member, score FROM _sortedsets "
          "WHERE key=?1 AND score>=?2 "
          "ORDER BY score ASC, member ASC");
      }
    }else{
      if( rev ){
        zSql = sqlite3_mprintf(
          "SELECT member, score FROM _sortedsets "
          "WHERE key=?1 AND score<=?2 "
          "ORDER BY score DESC, member DESC");
      }else{
        zSql = sqlite3_mprintf(
          "SELECT member, score FROM _sortedsets "
          "WHERE key=?1 AND score<=?2 "
          "ORDER BY score ASC, member ASC");
      }
    }
    if( zSql==0 ) return SQLITE_NOMEM;
    rc = sqlite3_prepare_v2(db, zSql, -1, &pCur->pStmt, 0);
    sqlite3_free(zSql);
    if( rc!=SQLITE_OK ) return rc;

    sqlite3_bind_text(pCur->pStmt, 1, key, -1, SQLITE_TRANSIENT);
    if( hasMin && hasMax ){
      sqlite3_bind_double(pCur->pStmt, 2, minVal);
      sqlite3_bind_double(pCur->pStmt, 3, maxVal);
    }else if( hasMin ){
      sqlite3_bind_double(pCur->pStmt, 2, minVal);
    }else{
      sqlite3_bind_double(pCur->pStmt, 2, maxVal);
    }
  }else{
    /* Rank-based range query using LIMIT/OFFSET */
    sqlite3_int64 offset = 0;
    sqlite3_int64 limit = -1;  /* -1 means no limit */
    sqlite3_int64 card = 0;

    if( hasStart || hasStop ){
      /* First get the cardinality to handle negative indices and stop=-1 */
      sqlite3_stmt *pCount = 0;
      rc = sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM _sortedsets WHERE key=?1",
        -1, &pCount, 0);
      if( rc!=SQLITE_OK ) return rc;
      sqlite3_bind_text(pCount, 1, key, -1, SQLITE_TRANSIENT);
      if( sqlite3_step(pCount)==SQLITE_ROW ){
        card = sqlite3_column_int64(pCount, 0);
      }
      sqlite3_finalize(pCount);

      if( card==0 ){
        pCur->bEof = 1;
        return SQLITE_OK;
      }

      offset = hasStart ? startVal : 0;
      sqlite3_int64 end = hasStop ? stopVal : (card - 1);

      /* Handle stop=-1 meaning "to the end" (Redis convention) */
      if( end==-1 ) end = card - 1;

      /* Handle negative indices (count from end) */
      if( offset<0 ) offset = card + offset;
      if( end<0 ) end = card + end;

      /* Clamp */
      if( offset<0 ) offset = 0;
      if( end>=card ) end = card - 1;
      if( offset>end ){
        pCur->bEof = 1;
        return SQLITE_OK;
      }
      limit = end - offset + 1;
    }

    if( rev ){
      rc = sqlite3_prepare_v2(db,
        "SELECT member, score FROM _sortedsets "
        "WHERE key=?1 ORDER BY score DESC, member DESC "
        "LIMIT ?2 OFFSET ?3",
        -1, &pCur->pStmt, 0);
    }else{
      rc = sqlite3_prepare_v2(db,
        "SELECT member, score FROM _sortedsets "
        "WHERE key=?1 ORDER BY score ASC, member ASC "
        "LIMIT ?2 OFFSET ?3",
        -1, &pCur->pStmt, 0);
    }
    if( rc!=SQLITE_OK ) return rc;
    sqlite3_bind_text(pCur->pStmt, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(pCur->pStmt, 2, limit);
    sqlite3_bind_int64(pCur->pStmt, 3, offset);
  }

  /* Step to first row */
  if( sqlite3_step(pCur->pStmt)==SQLITE_ROW ){
    pCur->iRowid = 1;
  }else{
    pCur->bEof = 1;
  }
  return SQLITE_OK;
}

static sqlite3_module zrangeModule = {
  0,                         /* iVersion */
  0,                         /* xCreate */
  zrangeConnect,             /* xConnect */
  zrangeBestIndex,           /* xBestIndex */
  zrangeDisconnect,          /* xDisconnect */
  0,                         /* xDestroy */
  zrangeOpen,                /* xOpen */
  zrangeClose,               /* xClose */
  zrangeFilter,              /* xFilter */
  zrangeNext,                /* xNext */
  zrangeEof,                 /* xEof */
  zrangeColumn,              /* xColumn */
  zrangeRowid,               /* xRowid */
  0,                         /* xUpdate */
  0,                         /* xBegin */
  0,                         /* xSync */
  0,                         /* xCommit */
  0,                         /* xRollback */
  0,                         /* xFindMethod */
  0,                         /* xRename */
  0,                         /* xSavepoint */
  0,                         /* xRelease */
  0,                         /* xRollbackTo */
  0,                         /* xShadowName */
  0                          /* xIntegrity */
};

#endif /* SQLITE_OMIT_VIRTUALTABLE */

/*
** Extension entry point.
*/
#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_sortedsets_init(
  sqlite3 *db,
  char **pzErrMsg,
  const sqlite3_api_routines *pApi
){
  int rc = SQLITE_OK;
  SQLITE_EXTENSION_INIT2(pApi);
  (void)pzErrMsg;

  rc = sqlite3_create_function(db, "zadd", 3,
    SQLITE_UTF8|SQLITE_DIRECTONLY, 0, zadd_func, 0, 0);
  if( rc!=SQLITE_OK ) return rc;

  rc = sqlite3_create_function(db, "zrem", 2,
    SQLITE_UTF8|SQLITE_DIRECTONLY, 0, zrem_func, 0, 0);
  if( rc!=SQLITE_OK ) return rc;

  rc = sqlite3_create_function(db, "zscore", 2,
    SQLITE_UTF8|SQLITE_INNOCUOUS, 0, zscore_func, 0, 0);
  if( rc!=SQLITE_OK ) return rc;

  rc = sqlite3_create_function(db, "zrank", 2,
    SQLITE_UTF8|SQLITE_INNOCUOUS, 0, zrank_func, 0, 0);
  if( rc!=SQLITE_OK ) return rc;

  rc = sqlite3_create_function(db, "zrevrank", 2,
    SQLITE_UTF8|SQLITE_INNOCUOUS, 0, zrevrank_func, 0, 0);
  if( rc!=SQLITE_OK ) return rc;

  rc = sqlite3_create_function(db, "zcard", 1,
    SQLITE_UTF8|SQLITE_INNOCUOUS, 0, zcard_func, 0, 0);
  if( rc!=SQLITE_OK ) return rc;

  rc = sqlite3_create_function(db, "zcount", 3,
    SQLITE_UTF8|SQLITE_INNOCUOUS, 0, zcount_func, 0, 0);
  if( rc!=SQLITE_OK ) return rc;

  rc = sqlite3_create_function(db, "zincrby", 3,
    SQLITE_UTF8|SQLITE_DIRECTONLY, 0, zincrby_func, 0, 0);
  if( rc!=SQLITE_OK ) return rc;

#ifndef SQLITE_OMIT_VIRTUALTABLE
  rc = sqlite3_create_module(db, "zrange", &zrangeModule, 0);
#endif

  return rc;
}
