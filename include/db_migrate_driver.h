#ifndef DB_MIGRATE_DRIVER_DEFINED
#define DB_MIGRATE_DRIVER_DEFINED

/**
 * What a driver is, for the people writing one.
 *
 * db-migrate-base is a class drivers extend: it writes the SQL every database
 * agrees on, a driver overrides what its database says differently, and
 * `this._super(...)` reaches the generic version. Here the same thing is a
 * struct of functions:
 *
 *   driver_t *pg = dbmDriverNew("pg", sizeof(pg_t));
 *
 *   pg->runSql      = pgRunSql;          // what every driver has to bring
 *   pg->mapDataType = pgMapDataType;     // and what it says differently
 *
 * `dbmDriverNew` fills every slot with the generic version first, so a driver
 * only names the ones it changes. The generic versions are ordinary functions
 * declared below, so `_super` is calling one by name:
 *
 *   static const char *pgMapDataType(driver_t *self, const char *type) {
 *     if (strcmp(type, "datetime") == 0) return "TIMESTAMP";
 *     return dbmBaseMapDataType(self, type);
 *   }
 *
 * Every generic function reaches the others through `self`, the way the
 * JavaScript reached them through `this` - so `dbmBaseCreateTable` asks
 * `self->columnDef` for each column, and a driver that overrides only
 * `columnDef` changes what `createTable` writes. That is the whole of the
 * inheritance db-migrate ever used, and none of it needs a class.
 *
 * What differs from base: one signature per operation instead of the
 * arguments-sniffing overloads, results through the return value rather than
 * a callback, and capabilities as fields rather than `typeof` probes.
 */

#include "db_migrate.h"


/** Per column, what `columnDef` was asked to write and what it decided. */
typedef struct {
  /** The only primary key column, so the key goes on the column itself. */
  bool soleKey;
} dbm_column_options_t;

struct driver_t {

  /** "pg", "sqlite3" - what `migrator_t.dialect()` answers. */
  const char *name;

  /** Where the last failure is described. */
  char error[512];

  /** Set by the walker for a dry run; every statement is printed instead. */
  bool dryRun;

  /** Every statement is printed as well as sent - `--verbose`. */
  bool verbose;

  /** `--ignore-on-init`, handed to every migration as migrator_t's. */
  bool ignoreOnInit;

  /**
   * Set by createTable and addColumn once their main statement went through,
   * before the foreign keys that follow it - node's driver signal. A v2
   * migration failing at that step then knows the table or column is there
   * and has to be undone. Cleared before every v2 step.
   */
  bool signaled;

  /**
   * Whether `removeColumn` may be given a recreation strategy for a NOT NULL
   * column - node's `_meta.supports.columnStrategies`.
   */
  bool columnStrategies;

  /**
   * The connection node's state is kept through, and the table it is kept
   * in. Set by the command line; v2 migrations and the lock need it.
   */
  struct dbm_state_t *state;

  /**
   * No transaction around a migration - `--non-transactional`, for what a
   * database refuses to do inside one, like PostgreSQL's CREATE INDEX
   * CONCURRENTLY. The record is still written after the migration.
   */
  bool noTransactions;

  /** The table the walker keeps its records in. */
  const char *migrationTable;

  /**
   * Foreign keys are written into the column - `REFERENCES t (c)` - rather
   * than added afterwards with `ALTER TABLE ... ADD CONSTRAINT`, which SQLite
   * does not have. A capability rather than an override, because the generic
   * versions ask it in three places and none of them is a slot.
   */
  bool inlineForeignKeys;

  /* ------------------------------------------- what a driver must bring */

  /** One or more statements, nothing back. 0 when every one of them worked. */
  int (*runSql)(driver_t *self, const char *text);

  /** A statement with parameters. `rows` may be NULL when nothing is wanted. */
  int (*query)(driver_t *self, const sql_t *query, json_t *rows);

  void (*close)(driver_t *self);

  /* -------------------------------- transactions, BEGIN/COMMIT by default */

  int (*startMigration)(driver_t *self);
  int (*endMigration)(driver_t *self);
  int (*abortMigration)(driver_t *self);

  /* ------------------------------------------ the pieces SQL is made of */

  /** `string` -> `VARCHAR`. Unknown names go through upper-cased. */
  const char *(*mapDataType)(driver_t *self, const char *type);

  /** An identifier, quoted the way this database quotes them. */
  void (*quoteName)(driver_t *self, dbm_text_t *out, const char *name);

  /** A string constant, quoted the way this database quotes them. */
  void (*quoteText)(driver_t *self, dbm_text_t *out, const char *text);

  /** `"name" VARCHAR (64) NOT NULL DEFAULT 'x'` for one column. */
  int (*columnDef)(driver_t *self, dbm_text_t *out, const char *table,
                   const char *column, json_t spec,
                   const dbm_column_options_t *options);

  /** What goes after the type: keys, NOT NULL, UNIQUE, DEFAULT. */
  int (*columnConstraint)(driver_t *self, dbm_text_t *out, const char *table,
                          const char *column, json_t spec,
                          const dbm_column_options_t *options);

  /** A value from a document, as a literal in SQL. */
  int (*valueLiteral)(driver_t *self, dbm_text_t *out, json_t value);

  /**
   * What goes after the closing parenthesis of a CREATE TABLE written as
   * `{columns: {...}, engine: "InnoDB"}` - MySQL's ENGINE and CHARSET,
   * CockroachDB's row TTL. Nothing by default.
   */
  int (*tableOptions)(driver_t *self, dbm_text_t *out, const char *table,
                      json_t spec);

  /* ---------------------------------------------------------- schema */

  int (*createTable)(driver_t *self, const char *table, json_t spec);
  int (*dropTable)(driver_t *self, const char *table, bool ifExists);
  int (*renameTable)(driver_t *self, const char *from, const char *to);

  int (*addColumn)(driver_t *self, const char *table, const char *column,
                   json_t spec);
  int (*removeColumn)(driver_t *self, const char *table, const char *column);
  int (*renameColumn)(driver_t *self, const char *table, const char *from,
                      const char *to);
  int (*changeColumn)(driver_t *self, const char *table, const char *column,
                      json_t spec);

  int (*addIndex)(driver_t *self, const char *table, const char *name,
                  json_t columns, bool unique);
  int (*removeIndex)(driver_t *self, const char *table, const char *name);

  int (*addForeignKey)(driver_t *self, const char *table,
                       const char *referenced, const char *name,
                       json_t mapping, json_t rules);
  int (*removeForeignKey)(driver_t *self, const char *table, const char *name);

  int (*insert)(driver_t *self, const char *table, json_t row);

  /* -------------------------------------------------------- databases */

  /** `db:create` and `db:drop`, on a connection that names no database. */
  int (*createDatabase)(driver_t *self, const char *name, bool ifNotExists);
  int (*dropDatabase)(driver_t *self, const char *name, bool ifExists);

  /* ------------------------------------------------------- bookkeeping */

  int (*createMigrationsTable)(driver_t *self);

  /** The ones already run, oldest first: rows of `{name: "/2026...-x"}`. */
  int (*loadedMigrations)(driver_t *self, json_t *names);

  int (*addMigrationRecord)(driver_t *self, const char *name);
  int (*deleteMigrationRecord)(driver_t *self, const char *name);
};

/**
 * A driver with every slot set to the generic version, and `size` bytes for
 * the driver's own state behind it - `size` is the size of a struct whose
 * first member is a `driver_t`, the way C has always done inheritance.
 * `runSql`, `query` and `close` are left failing: no database agrees on those.
 */
driver_t *dbmDriverNew(const char *name, size_t size);

/**
 * Writes a reason into `self->error`. Answers -1, so a failure is one line:
 *
 *   return dbmFail(self, TEXT`index ${name} on ${table} with no columns`);
 */
int dbmFail(driver_t *self, text_t why);

/* the generic versions, for a driver to call as its `_super` */

const char *dbmBaseMapDataType(driver_t *self, const char *type);
void dbmBaseQuoteName(driver_t *self, dbm_text_t *out, const char *name);
void dbmBaseQuoteText(driver_t *self, dbm_text_t *out, const char *text);
int dbmBaseColumnDef(driver_t *self, dbm_text_t *out, const char *table,
                     const char *column, json_t spec,
                     const dbm_column_options_t *options);
int dbmBaseColumnConstraint(driver_t *self, dbm_text_t *out, const char *table,
                            const char *column, json_t spec,
                            const dbm_column_options_t *options);
int dbmBaseValueLiteral(driver_t *self, dbm_text_t *out, json_t value);
int dbmBaseTableOptions(driver_t *self, dbm_text_t *out, const char *table,
                        json_t spec);

/**
 * ` DEFAULT ...` for a column's `defaultValue`: a value, `{special:
 * "CURRENT_TIMESTAMP"}`, or `{raw: "now()"}` written as it is. For a driver
 * that writes its own constraints and still wants the same defaults.
 */
int dbmBaseDefault(driver_t *self, dbm_text_t *out, json_t value);

/** ` REFERENCES "t" ("c") ON DELETE ...`, for a driver with inline keys. */
int dbmBaseReferences(driver_t *self, dbm_text_t *out, const char *table,
                      const char *column, json_t key);

int dbmBaseStartMigration(driver_t *self);
int dbmBaseEndMigration(driver_t *self);
int dbmBaseAbortMigration(driver_t *self);

int dbmBaseCreateTable(driver_t *self, const char *table, json_t spec);
int dbmBaseDropTable(driver_t *self, const char *table, bool ifExists);
int dbmBaseRenameTable(driver_t *self, const char *from, const char *to);
int dbmBaseAddColumn(driver_t *self, const char *table, const char *column,
                     json_t spec);
int dbmBaseRemoveColumn(driver_t *self, const char *table, const char *column);
int dbmBaseRenameColumn(driver_t *self, const char *table, const char *from,
                        const char *to);
int dbmBaseAddIndex(driver_t *self, const char *table, const char *name,
                    json_t columns, bool unique);
int dbmBaseRemoveIndex(driver_t *self, const char *table, const char *name);
int dbmBaseAddForeignKey(driver_t *self, const char *table,
                         const char *referenced, const char *name,
                         json_t mapping, json_t rules);
int dbmBaseRemoveForeignKey(driver_t *self, const char *table,
                            const char *name);
int dbmBaseInsert(driver_t *self, const char *table, json_t row);
int dbmBaseCreateDatabase(driver_t *self, const char *name, bool ifNotExists);
int dbmBaseDropDatabase(driver_t *self, const char *name, bool ifExists);

int dbmBaseCreateMigrationsTable(driver_t *self);
int dbmBaseLoadedMigrations(driver_t *self, json_t *names);
int dbmBaseAddMigrationRecord(driver_t *self, const char *name);
int dbmBaseDeleteMigrationRecord(driver_t *self, const char *name);

/**
 * The type a column spec names, whichever way it was written: `name:
 * "string"` and `name: {type: "string"}` are the same column.
 */
const char *dbmColumnType(json_t spec);

/* --------------------------------------------- node's state table */

/**
 * The key-value table node keeps its state in - key, value, run_on - read
 * and written the way node does it. `dbmKvGet` hands back a copy of the
 * value, or NULL when there is no such row; `dbmKvSwap` updates only when
 * the row still holds `expected`.
 */
int dbmKvCreate(driver_t *self, const char *table);
int dbmKvGet(driver_t *self, const char *table, const char *key, char **value);
int dbmKvInsert(driver_t *self, const char *table, const char *key,
                const char *value);
int dbmKvUpdate(driver_t *self, const char *table, const char *key,
                const char *value);
int dbmKvSwap(driver_t *self, const char *table, const char *key,
              const char *value, const char *expected);
int dbmKvDelete(driver_t *self, const char *table, const char *key);

/* ---------------------------------------------------- node's state */

#include <pthread.h>

/**
 * What node db-migrate keeps in its state table, and the lock on it.
 *
 *   __dbmigrate_state__    {"s":{"step","fin","ID","date","n"}} - who holds
 *                          the lock, and how far the running migration got
 *   __dbmigrate_schema__   {"i","c","f","e"} - the schema v2 migrations built
 *   <migration name>       {"i","c","f","s"} - what one v2 migration changed,
 *                          and the steps that undo it
 *
 * Written through a connection of its own, so that what it says survives
 * the rollback of a migration's transaction.
 */
typedef struct dbm_state_t {
  driver_t *db;
  const char *table;
  long timeoutMs;
  long intervalMs;

  /** A session of the lock is running, and whether this process holds it. */
  bool active;
  bool owner;

  /** The lock row as last written and read back: the next swap's `expected`. */
  char *current;
  char id[64];

  /** The schema the v2 migrations built: `{i, c, f, e}`, as JSON. */
  void *schema;

  /** Writes from the heartbeat and from the walker, one at a time. */
  pthread_mutex_t writing;
  pthread_cond_t wake;
  pthread_t heartbeat;
  bool beating;
  bool stopping;
} dbm_state_t;

/** The state table made if it is not there, and the schema read from it. */
dbm_state_t *dbmStateOpen(driver_t *db, const char *table, long timeoutMs,
                          long intervalMs, bool dry);
void dbmStateClose(dbm_state_t *self);

/**
 * The migration lock, waited for while another process holds it, and taken
 * over when its holder has not touched it for the timeout. False when it
 * could not be had at all, with the reason said.
 */
bool dbmStateLock(dbm_state_t *self);
void dbmStateUnlock(dbm_state_t *self);
int dbmStateReloadSchema(dbm_state_t *self);

/** How far the running migration got: -1 leaves a field as it is. */
int dbmStateProgress(dbm_state_t *self, int step, int fin);

/**
 * Fields of the lock row set, as a JSON object - `{"done":3}` - and every
 * other field left as it is, node's included.
 */
int dbmStateMark(dbm_state_t *self, const char *changes);

/**
 * A v2 migration that a previous run left unfinished, as the lock row says:
 * the last step started, the last whose undoing was recorded, the last sent
 * to the database, whether it was rolling back, and whether its file changed
 * since. node's startMigration with `recover`.
 */
typedef struct {
  bool found;
  long step;
  long learned;
  long done;
  bool rollback;
  bool changed;
} dbm_interrupted_t;

/**
 * Starting a v2 migration: its record - `{i, c, f, s}`, made if there is
 * none - as a JSON text the caller frees, and the lock row saying which
 * migration runs which way (`op`: "up", "down", "fix") from step 0, with
 * its file's hash or NULL. With `interrupted`, an unfinished "up" of the
 * same migration is not started over but described there, for the caller to
 * resume; an unfinished other one is said and left.
 */
char *dbmStateBegin(dbm_state_t *self, const char *key, const char *op,
                    const char *hash, dbm_interrupted_t *interrupted);
int dbmStateSave(dbm_state_t *self, const char *key, const char *migration);
int dbmStateForget(dbm_state_t *self, const char *key);

/** v2 migrations, learned and undone the way node does it. */
int dbmUpV2(driver_t *driver, dbm_state_t *state,
            const dbm_migration_t *migration, char *why, size_t room);
int dbmEndV2(dbm_state_t *state, bool dry);
int dbmFixV2(driver_t *driver, dbm_state_t *state,
             const dbm_migration_t *migration, char *why, size_t room);

/**
 * `fix --backup-state`: the schema as it was written to
 * `<table>_b_<seconds>.dbmigrate`, and the table renamed to that name, before
 * an empty one is made - as node keeps a backup.
 */
int dbmStateBackup(dbm_state_t *self);

/** An empty schema, in memory, for `fix` to rebuild from. */
void dbmStateForgetSchema(dbm_state_t *self);
int dbmDownV2(driver_t *driver, dbm_state_t *state,
              const dbm_migration_t *migration, char *why, size_t room);

/** SHA-256 as 64 hex digits; a file's, or false when it cannot be read. */
void dbmSha256(const void *data, size_t length, char hex[65]);
bool dbmSha256File(const char *path, char hex[65]);

/** Runs or, on a dry run, prints. The way every generic version sends SQL. */
int dbmSend(driver_t *self, dbm_text_t *sql);

/** The same for a statement with parameters, and the rows when they are wanted. */
int dbmQuery(driver_t *self, const sql_t *query, json_t *rows);

#endif
