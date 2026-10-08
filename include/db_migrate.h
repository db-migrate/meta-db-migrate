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

/** What `up` and `down` are handed. */
typedef struct {
  driver_t *driver;

  /** Nothing is sent; every statement is written to standard output. */
  bool dryRun;

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
 * Makes a migration that is known only by its file into one that can run -
 * the launcher compiles it and opens it, and its DBM_MIGRATION fills in `up`
 * and `down`. Answers whether that worked, having said why when it did not.
 */
typedef bool (*dbm_load_t)(const char *file);

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
} dbm_migration_t;

/**
 * Called by `DBM_MIGRATION` before main runs - or when a shared object
 * holding the migration is opened, which is how the development launcher
 * loads one it has just compiled. The name comes from the file.
 */
void dbmRegister(const char *file, dbm_step_t up, dbm_step_t down);

/**
 * A migration known by its file and not loaded yet. Which ones have to run is
 * the database's to say, and the walker loads exactly those - so a project
 * with three hundred migrations and a database at the two hundred and
 * ninety-ninth compiles one of them, not three hundred.
 */
void dbmRegisterLazily(const char *file, dbm_load_t load);

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
bool dbmLoadSqlFiles(const char *upFile);

/** Every migration registered so far, sorted by name. */
const dbm_migration_t *dbmMigrations(size_t *count);

/** Forgets them, so a launcher can load a fresh set. */
void dbmForgetMigrations(void);

#define DBM_MIGRATION(up, down)                                                \
  __attribute__((constructor)) static void dbmRegisterThisFile__(void) {     \
    dbmRegister(__FILE__, up, down);                                           \
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
 *   up [n]      run what has not been run, or the next n
 *   down [n]    undo the last one, or the last n
 *   reset       undo everything
 *   check       list what has not been run
 *
 * Answers 0 when it did what was asked.
 */
int dbmUp(driver_t *driver, size_t count, bool dryRun);
int dbmDown(driver_t *driver, size_t count, bool dryRun);
int dbmReset(driver_t *driver, bool dryRun);
int dbmCheck(driver_t *driver);

/**
 * `db-migrate up -e dev --count 2`, read the way node db-migrate reads it:
 * database.json in the working directory or `--config`, the environment from
 * `-e` or `NODE_ENV` or `dev`, and `DATABASE_URL` when there is no file.
 */
int dbmCli(int argc, char **argv);

#endif
