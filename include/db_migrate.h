#ifndef DB_MIGRATE_DEFINED
#define DB_MIGRATE_DEFINED

/**
 * db-migrate, for programs written in meta.
 *
 * A migration is a file of meta with an `up` and a `down` in it:
 *
 *   #include <db_migrate.h>
 *
 *   static int up(migrator_t *db) {
 *     return db->createTable("pets", {
 *       id:   {type: "int", primaryKey: true, autoIncrement: true},
 *       name: {type: "string", notNull: true},
 *     });
 *   }
 *
 *   static int down(migrator_t *db) {
 *     return db->dropTable("pets");
 *   }
 *
 *   DBM_MIGRATION(up, down)
 *
 * The specs are documents rather than structs on purpose. What a column can
 * say belongs to whichever driver is connected - `engine` means something to
 * MySQL and nothing to PostgreSQL - and a struct would have to know every
 * key of every driver. A `json_t` only has to carry them, which is what the
 * JavaScript always did, and meta writes `{...}` as one where a `json_t` is
 * wanted.
 *
 * This header is read by meta and by the C compiler, so it is plain C. The
 * methods are spelled `migrator_t__name`, which meta reads as `db->name(...)`.
 *
 * Errors stick. The first call that fails records why, and every call after it
 * on the same migrator does nothing and answers -1 - so a migration can be
 * written as the straight list of steps it is, and the one that went wrong is
 * still the one reported. The walker asks afterwards and rolls back. A
 * migration that wants to stop early returns what the failing call gave it.
 */

#include <meta_json.h>
#include <meta_sql.h>
#include <meta_text.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#define DBM_VERSION "0.3.0"

typedef struct driver_t driver_t;

/**
 * Growing text, for the SQL a driver writes.
 *
 * Plain C because a driver's override takes one and a header has to be able
 * to say so. `put` appends, `name` appends an identifier the way this driver
 * quotes them, and `literal` a string constant the way it quotes those.
 */
typedef struct {
  char *text;
  size_t length;
  size_t room;
  bool failed;
} dbm_text_t;

void dbm_text_t__put(dbm_text_t *self, const char *text);
void dbm_text_t__putn(dbm_text_t *self, const char *text, size_t length);

/** A `TEXT` template, appended: `sql.append(TEXT\`(${length})\`)`. */
void dbm_text_t__append(dbm_text_t *self, text_t piece);

void dbm_text_t__release(dbm_text_t *self);

/**
 * A line to a stream, written as a template: `dbmSay(stderr, TEXT\`[ERROR]
 * ${name}: ${why}\n\`)`. The type of each hole picks its conversion, so
 * there is no format string to disagree with the arguments.
 */
void dbmSay(FILE *to, text_t line);

/** A template into a fixed buffer; answers the length it wanted, as snprintf. */
size_t dbmWrite(char *into, size_t room, text_t text);

/**
 * node's --log-level: which of `[INFO]`, `[WARN]`, `[ERROR]` and the
 * statements (`sql`) dbmSay lets through - `"sql|warn"`. Lines without one
 * of those marks (usage, the version) always go out.
 */
enum { DBM_LOG_INFO = 1, DBM_LOG_WARN = 2, DBM_LOG_ERROR = 4, DBM_LOG_SQL = 8 };

void dbmSetLogLevel(const char *levels);
bool dbmLogs(int level);

/**
 * Where the lines go that would go to standard output and standard error:
 * a program with a log of its own - an nginx module - hands them to it.
 * `level` is the line's mark (DBM_LOG_INFO, ...), 0 for one without; `line`
 * has no newline at its end. --log-level still decides which lines come.
 * It may be called from the lock's heartbeat thread. NULL goes back to the
 * streams.
 */
typedef void (*dbm_logger_t)(int level, const char *line);

void dbmSetLogger(dbm_logger_t logger);

/** The last `[ERROR]` line said, without its mark, or "". */
const char *dbmLastError(void);

/**
 * The first line of an -up.sql that only runs without --ignore-on-init, as
 * `create --sql-file --ignore-on-init` writes it.
 */
#define DBM_IGNORE_ON_INIT_MARK "-- db-migrate: ignore-on-init"

/** What `up` and `down` are handed. */
typedef struct {
  driver_t *driver;

  /** Nothing is sent; every statement is written to standard output. */
  bool dryRun;

  /**
   * Run with --ignore-on-init: the database already has what the migrations
   * made, and they are only being recorded. A migration written for that
   * returns from `up` early: `if (db->ignoreOnInit) return 0;`
   */
  bool ignoreOnInit;

  /** The first thing that went wrong, and the reason every later call is a no-op. */
  char error[512];
  bool failed;
} migrator_t;

/* ------------------------------------------------------------- schema */

int migrator_t__createTable(migrator_t *self, const char *table, json_t spec);
int migrator_t__dropTable(migrator_t *self, const char *table);
int migrator_t__dropTableIfExists(migrator_t *self, const char *table);
int migrator_t__renameTable(migrator_t *self, const char *from, const char *to);

int migrator_t__addColumn(migrator_t *self, const char *table,
                          const char *column, json_t spec);
int migrator_t__removeColumn(migrator_t *self, const char *table,
                             const char *column);
int migrator_t__renameColumn(migrator_t *self, const char *table,
                             const char *from, const char *to);
int migrator_t__changeColumn(migrator_t *self, const char *table,
                             const char *column, json_t spec);

/** `columns` is an array of names: `db->addIndex("pets", "pets_name", ["name"])`. */
int migrator_t__addIndex(migrator_t *self, const char *table, const char *name,
                         json_t columns);
int migrator_t__addUniqueIndex(migrator_t *self, const char *table,
                               const char *name, json_t columns);
int migrator_t__removeIndex(migrator_t *self, const char *table,
                            const char *name);

/**
 * `mapping` is `{local: "foreign"}` - the column here to the column there -
 * and `rules` may say `{onDelete: "CASCADE", onUpdate: "RESTRICT"}`.
 */
int migrator_t__addForeignKey(migrator_t *self, const char *table,
                              const char *referenced, const char *name,
                              json_t mapping, json_t rules);
int migrator_t__removeForeignKey(migrator_t *self, const char *table,
                                 const char *name);

/* --------------------------------------------------------------- data */

/** `db->insert("pets", {name: "kurbel", age: 3})` */
int migrator_t__insert(migrator_t *self, const char *table, json_t row);

/** A statement written by hand. Taken by value and released here. */
int migrator_t__run(migrator_t *self, sql_t query);

/** Text sent as it is, for what the SQL literal cannot hold: DDL, `DO $$`. */
int migrator_t__runSql(migrator_t *self, const char *text);

/**
 * The rows a query answers, as an array of objects keyed by column name.
 *
 *   json_t pets = db->all(SQL`select id, name from pets`);
 *   defer pets.release();
 *
 *   for (int i = 0; i < pets.count(); ++i)
 *     greet(pets[i].name);
 *
 * The query is taken by value and released here. The rows are the caller's.
 */
json_t migrator_t__all(migrator_t *self, sql_t query);

/* ------------------------------------------------------------ the rest */

/** Whether a call has failed, and why. */
bool migrator_t__hasFailed(migrator_t *self);
const char *migrator_t__lastError(migrator_t *self);

/** Fails the migration on purpose: `return db->fail(TEXT\`no ${table}\`);` */
int migrator_t__fail(migrator_t *self, text_t why);

/** "pg", "sqlite3" - for a migration that has to differ by database. */
const char *migrator_t__dialect(migrator_t *self);

/* ---------------------------------------------------------- migrations */

typedef int (*dbm_step_t)(migrator_t *db);

/**
 * What a v2 migration is handed - node's `_meta: {version: 2}`.
 *
 * A v2 migration has no `down`. Every change it makes is learned: the schema
 * it builds is kept in node's state table, and so is how to undo each step,
 * which is what `down` - and a rollback when one fails halfway - runs
 * backwards. That is why it may only change the schema: a statement written
 * by hand cannot be undone by anybody but the person who wrote it, so there
 * is no `runSql` here, as there is none in node's v2.
 *
 *   static int migrate(schema_t *db) {
 *     db->createTable("pets", {id: {type: "int", primaryKey: true}});
 *     return db->addIndex("pets", "pets_id_idx", ["id"]);
 *   }
 *
 *   DBM_MIGRATION_V2(migrate)
 *
 * Every table it creates gets a column `__dbmigrate__flag`, as node's do.
 */
typedef struct schema_t schema_t;

typedef int (*dbm_v2_t)(schema_t *db);

int schema_t__createTable(schema_t *self, const char *table, json_t spec);
int schema_t__dropTable(schema_t *self, const char *table);
int schema_t__renameTable(schema_t *self, const char *from, const char *to);
int schema_t__addColumn(schema_t *self, const char *table, const char *column,
                        json_t spec);
int schema_t__removeColumn(schema_t *self, const char *table,
                           const char *column);

/**
 * A NOT NULL column cannot be dropped without saying how it comes back:
 * `{columnStrategy: "defaultValue", passthrough: {defaultValue: 0}}` adds it
 * again with that default, `{columnStrategy: "delay"}` renames it out of the
 * way instead of dropping it, as node does.
 */
int schema_t__removeColumnWith(schema_t *self, const char *table,
                               const char *column, json_t options);
int schema_t__renameColumn(schema_t *self, const char *table, const char *from,
                           const char *to);
int schema_t__changeColumn(schema_t *self, const char *table,
                           const char *column, json_t spec);
int schema_t__addIndex(schema_t *self, const char *table, const char *name,
                       json_t columns);
int schema_t__addUniqueIndex(schema_t *self, const char *table,
                             const char *name, json_t columns);
int schema_t__removeIndex(schema_t *self, const char *table, const char *name);
int schema_t__addForeignKey(schema_t *self, const char *table,
                            const char *referenced, const char *name,
                            json_t mapping, json_t rules);
int schema_t__removeForeignKey(schema_t *self, const char *table,
                               const char *name);

bool schema_t__hasFailed(schema_t *self);
const char *schema_t__lastError(schema_t *self);
int schema_t__fail(schema_t *self, text_t why);

/**
 * Makes a migration that is known only by its file into one that can run -
 * the launcher compiles it and opens it, and its DBM_MIGRATION fills in `up`
 * and `down`. Answers whether that worked, having said why when it did not.
 */
typedef bool (*dbm_load_t)(const char *file, const char *name);

typedef struct {
  /** As node db-migrate records it: the file name without `.c`, after a `/`. */
  char name[256];
  dbm_step_t up;
  dbm_step_t down;

  /** For one registered by `dbmRegisterLazily`: where it is and how to load it. */
  char file[1024];
  dbm_load_t load;

  /**
   * A migration written as SQL rather than as code: what `up` and `down`
   * send, in place of the two functions. Owned by the entry.
   */
  char *upSql;
  char *downSql;

  /** A v2 migration: one function, and `down` is learned from it. */
  dbm_v2_t migrate;

  /**
   * What a v2 migration that a previous run left unfinished does next -
   * node's `_meta.recovery`: "skip" (NULL) leaves the steps that ran and
   * carries on after them, "rollback" undoes them and runs it again.
   */
  const char *recovery;
} dbm_migration_t;

/**
 * Called by `DBM_MIGRATION` before main runs - or when a shared object
 * holding the migration is opened, which is how the development launcher
 * loads one it has just compiled. The name comes from the file.
 */
void dbmRegister(const char *file, dbm_step_t up, dbm_step_t down);

/**
 * A migration known by its file and not loaded yet, named by `as` - a path
 * ending in `migrations/[scope/]<name>.c`, which may differ from `file` when
 * the directory is called something else.
 *
 * Which ones have to run is the database's to say, and the walker loads
 * exactly those - so a project with three hundred migrations and a database
 * at the two hundred and ninety-ninth compiles one of them, not three
 * hundred.
 */
void dbmRegisterLazily(const char *as, const char *file, dbm_load_t load);

/** Makes sure a migration's steps are there, loading it if it was registered lazily. */
bool dbmLoaded(const dbm_migration_t *migration);

/**
 * A migration written as SQL: `migrations/sqls/<name>-up.sql` and
 * `<name>-down.sql`, the files node db-migrate's `create --sql-file` makes.
 * `file` is any path whose last part is the migration's name, with or without
 * `-up.sql`; the texts are copied.
 */
void dbmRegisterSql(const char *file, const char *up, const char *down);

/**
 * A loader for `dbmRegisterLazily`: reads `<name>-up.sql` and the
 * `-down.sql` beside it. A missing down file is a migration that cannot be
 * undone, which `down` then says rather than doing nothing.
 */
bool dbmLoadSqlFiles(const char *upFile, const char *name);

/** Every migration registered so far, sorted by name. */
const dbm_migration_t *dbmMigrations(size_t *count);

/** Forgets them, so a launcher can load a fresh set. */
void dbmForgetMigrations(void);

#define DBM_MIGRATION(up, down)                                                \
  __attribute__((constructor)) static void dbmRegisterThisFile__(void) {     \
    dbmRegister(__FILE__, up, down);                                           \
  }

void dbmRegisterV2(const char *file, dbm_v2_t migrate);

#define DBM_MIGRATION_V2(migrate)                                              \
  __attribute__((constructor)) static void dbmRegisterThisFile__(void) {     \
    dbmRegisterV2(__FILE__, migrate);                                          \
  }

void dbmRegisterV2Recovering(const char *file, dbm_v2_t migrate,
                             const char *recovery);

/**
 * A v2 migration that says how it is resumed after an interrupted run -
 * `DBM_MIGRATION_V2_RECOVERY(migrate, "rollback")` - as node's
 * `_meta: {version: 2, recovery: 'rollback'}`.
 */
#define DBM_MIGRATION_V2_RECOVERY(migrate, recovery)                           \
  __attribute__((constructor)) static void dbmRegisterThisFile__(void) {     \
    dbmRegisterV2Recovering(__FILE__, migrate, recovery);                      \
  }

/* ------------------------------------------------------------- drivers */

/**
 * Opens a connection from a configuration object - one entry of a
 * database.json, already resolved. Answers NULL and writes the reason into
 * `why` when it cannot.
 */
typedef driver_t *(*dbm_open_t)(json_t config, char *why, size_t room);

/** A driver says it exists, by name. Called from its own constructor. */
void dbmRegisterDriver(const char *name, dbm_open_t open);

/**
 * Where drivers that are not linked in are loaded from, as
 * `libdbmigrate-<name>.so`. NULL - the default - loads nothing, which is
 * right for a program with its drivers compiled in; the launcher sets it.
 */
extern const char *dbmDriverDirectory;

/** Opens a connection with the driver the configuration names. */
driver_t *dbmOpen(json_t config, char *why, size_t room);

void dbmClose(driver_t *driver);

/* ------------------------------------------------------------- running */

/**
 * The commands, as the command line spells them.
 *
 *   up [name]       run what has not been run, up to and including name
 *   down [name]     undo the last one - or everything after name
 *   sync name       up or down, whichever reaches name
 *   reset           undo everything
 *   check           list what has not been run
 *
 * `-c n` limits up and down to n migrations. A name may be cut short: a
 * timestamp, or the start of one, is a destination too.
 *
 * Answers 0 when it did what was asked.
 */
int dbmUp(driver_t *driver, size_t count, const char *destination,
          bool dryRun);
int dbmDown(driver_t *driver, size_t count, const char *destination,
            bool dryRun);
int dbmReset(driver_t *driver, bool dryRun);
int dbmSync(driver_t *driver, const char *destination, bool dryRun);

/** node's `fix`: the state rebuilt from the v2 migrations that ran. */
int dbmFix(driver_t *driver, bool backup, bool dryRun);

/**
 * The scope the commands work in: "" or NULL for migrations/ itself, a
 * directory under it otherwise - what `up:billing` names.
 */
void dbmUseScope(const char *scope);
int dbmCheck(driver_t *driver);

/**
 * What dbmMigrateUp is told besides the connection; zeroed is node's
 * defaults: tables `migrations` and `migrations_state`, a lock taken over
 * after 60000 ms untouched, looked at every 1000 ms.
 */
typedef struct {
  const char *migrationTable;
  const char *stateTable;
  long lockTimeout;
  long lockInterval;
  bool verbose;
} dbm_options_t;

/**
 * Every migration this program has that the database has not run, as
 * `up` does it: a connection, a second one for node's state, the lock with
 * its heartbeat, the migrations, and everything closed again. For a program
 * that migrates itself when it starts, with the migrations compiled in.
 *
 * `config` is one connection, as an environment of database.json says it,
 * `tunnel` included; it is not released. Blocks until it is done. Answers 0,
 * or -1 with the reason in `why`.
 */
int dbmMigrateUp(json_t config, const dbm_options_t *options, char *why,
                 size_t room);

/**
 * `db-migrate up -e dev --count 2`, read the way node db-migrate reads it:
 * database.json in the working directory or `--config`, the environment from
 * `-e` or `NODE_ENV` or `dev`, and `DATABASE_URL` when there is no file.
 */
int dbmCli(int argc, char **argv);

#endif
