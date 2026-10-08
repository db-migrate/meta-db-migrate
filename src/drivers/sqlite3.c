/**
 * SQLite.
 *
 * The generic SQL does most of it - SQLite takes double-quoted names,
 * single-quoted strings, `RENAME COLUMN` (3.25) and `DROP COLUMN` (3.35). What
 * differs:
 *
 * - a key that counts itself up is `INTEGER PRIMARY KEY AUTOINCREMENT`, and
 *   only that spelling: the type has to be exactly INTEGER, without a length
 * - a foreign key cannot be added to a table that exists, so it is written
 *   into its column as `REFERENCES` - which node's driver did not do, and
 *   dropped the key without a word instead
 * - a column cannot be changed in place; that is refused with the reason,
 *   because the honest version is a table rebuild, and a rebuild is a
 *   migration of its own rather than something to do behind somebody's back
 *
 * SQLite runs in this process over a file, so there is nothing to wait on and
 * nothing to park. `{filename: "dev.db"}` in database.json, as for node, and
 * `:memory:` works for a test that wants nothing left behind.
 */
#include <db_migrate_driver.h>

#include <meta_sqlite.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  driver_t base;
  sqlite3 *conn;
} sqlite_driver_t;

static sqlite3 *connectionOf(driver_t *self) {
  return ((sqlite_driver_t *)self)->conn;
}

/** Any number of statements; SQLite stops at the first that fails and says why. */
static int liteRunSql(driver_t *self, const char *text) {

  char *message = NULL;

  if (sqlite3_exec(connectionOf(self), text, NULL, NULL, &message) ==
      SQLITE_OK)
    return 0;

  int answer = dbmFail(self, TEXT`${message != NULL ? message
                                       : sqlite3_errmsg(connectionOf(self))}`);

  sqlite3_free(message);
  return answer;
}

/** A row as an object, each value by the kind SQLite stored it as. */
static yyjson_mut_val *rowOf(yyjson_mut_doc *doc, sqlite3_stmt *statement) {

  yyjson_mut_val *row = yyjson_mut_obj(doc);
  int columns = sqlite3_column_count(statement);

  for (int column = 0; column < columns; ++column) {

    yyjson_mut_val *held;

    switch (sqlite3_column_type(statement, column)) {
    case SQLITE_INTEGER:
      held = yyjson_mut_sint(doc, sqlite3_column_int64(statement, column));
      break;
    case SQLITE_FLOAT:
      held = yyjson_mut_real(doc, sqlite3_column_double(statement, column));
      break;
    case SQLITE_NULL:
      held = yyjson_mut_null(doc);
      break;
    default:
      held = yyjson_mut_strcpy(
          doc, (const char *)sqlite3_column_text(statement, column));
      break;
    }

    yyjson_mut_obj_add(
        row, yyjson_mut_strcpy(doc, sqlite3_column_name(statement, column)),
        held);
  }

  return row;
}

static int liteQuery(driver_t *self, const sql_t *query, json_t *rows) {

  sqlite3 *db = connectionOf(self);
  sqlite3_stmt *statement = db.ask(query);

  if (statement == NULL)
    return dbmFail(self, TEXT`${sqlite3_errmsg(db)}`);

  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *array = yyjson_mut_arr(doc);
  int stepped;

  while ((stepped = sqlite3_step(statement)) == SQLITE_ROW)
    if (rows != NULL)
      yyjson_mut_arr_append(array, rowOf(doc, statement));

  int answer = stepped == SQLITE_DONE
                   ? 0
                   : dbmFail(self, TEXT`${sqlite3_errmsg(db)}`);

  sqlite3_finalize(statement);
  yyjson_mut_doc_set_root(doc, array);

  if (answer != 0 || rows == NULL) {
    yyjson_mut_doc_free(doc);
    return answer;
  }

  *rows = meta_jsonFromMut(doc);
  return 0;
}

static void liteClose(driver_t *self) {
  sqlite3_close(connectionOf(self));
}

/* ------------------------------------------------------------------ */
/* where SQLite says it differently                                   */
/* ------------------------------------------------------------------ */

static const char *liteMapDataType(driver_t *self, const char *type) {

  /* stored as text in the ISO form, which is what SQLite's own functions read */
  if (type in {"datetime", "DATETIME"})
    return "DATETIME";

  return dbmBaseMapDataType(self, type);
}

/**
 * `INTEGER PRIMARY KEY AUTOINCREMENT` is the only form SQLite counts with,
 * and `INTEGER(11) PRIMARY KEY` is not it - so an integer has no length here.
 */
static int liteColumnDef(driver_t *self, dbm_text_t *out, const char *table,
                         const char *column, json_t spec,
                         const dbm_column_options_t *options) {

  const char *type = dbmColumnType(spec);
  const char *mapped = self->mapDataType(self, type);
  long length = spec.length;

  self->quoteName(self, out, column);
  out->put(" ");
  out->put(mapped != NULL ? mapped : type);

  if (length > 0 && !(mapped != NULL && strcmp(mapped, "INTEGER") == 0))
    out->append(TEXT`(${length})`);

  return self->columnConstraint(self, out, table, column, spec, options);
}

static int liteColumnConstraint(driver_t *self, dbm_text_t *out,
                                const char *table, const char *column,
                                json_t spec,
                                const dbm_column_options_t *options) {

  if (spec.primaryKey.truth() && options->soleKey) {

    out->put(" PRIMARY KEY");

    if (spec.autoIncrement.truth())
      out->put(" AUTOINCREMENT");

    /* the rest, without a second PRIMARY KEY */
    dbm_column_options_t rest = {false};

    return dbmBaseColumnConstraint(self, out, table, column, spec, &rest);
  }

  return dbmBaseColumnConstraint(self, out, table, column, spec, options);
}

static int liteChangeColumn(driver_t *self, const char *table,
                            const char *column, json_t spec) {

  (void)spec;

  return dbmFail(self, TEXT`SQLite cannot change a column in place - ${table}.${column} would have to be rebuilt, which is a migration of its own: create the new table, copy the rows, drop the old one, rename`);
}

/** A SQLite database is a file, made when it is first opened. */
static int liteCreateDatabase(driver_t *self, const char *name,
                              bool ifNotExists) {
  (void)self;
  (void)name;
  (void)ifNotExists;
  return 0;
}

/**
 * Refused rather than done: dropping one would be deleting a file, and a
 * migration tool that deletes files by a name on the command line is one
 * typo from deleting the wrong one.
 */
static int liteDropDatabase(driver_t *self, const char *name, bool ifExists) {
  (void)ifExists;
  return dbmFail(self, TEXT`a SQLite database is a file - delete ${name} yourself if that is what you mean`);
}

/* ------------------------------------------------------------------ */
/* opening one                                                        */
/* ------------------------------------------------------------------ */

static driver_t *liteOpen(json_t config, char *why, size_t room) {

  const char *filename = config.filename;

  if (filename[0] == '\0')
    filename = config.database;

  if (filename[0] == '\0') {
    dbmWrite(why, room, TEXT`the configuration does not say which file - \`filename\``);
    return NULL;
  }

  sqlite3 *db = NULL;

  if (sqlite3_open_v2(filename, &db,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                          SQLITE_OPEN_URI,
                      NULL) != SQLITE_OK) {
    dbmWrite(why, room, TEXT`${filename}: ${db != NULL ? sqlite3_errmsg(db) : "out of memory"}`);
    sqlite3_close(db);
    return NULL;
  }

  sqlite_driver_t *driver =
      (sqlite_driver_t *)dbmDriverNew("sqlite3", sizeof(sqlite_driver_t));

  if (driver == NULL) {
    sqlite3_close(db);
    dbmWrite(why, room, TEXT`out of memory`);
    return NULL;
  }

  driver->conn = db;

  driver_t *self = &driver->base;

  self->runSql = liteRunSql;
  self->query = liteQuery;
  self->close = liteClose;

  self->inlineForeignKeys = true;
  self->mapDataType = liteMapDataType;
  self->columnDef = liteColumnDef;
  self->columnConstraint = liteColumnConstraint;
  self->changeColumn = liteChangeColumn;
  self->createDatabase = liteCreateDatabase;
  self->dropDatabase = liteDropDatabase;

  /* a key that is declared and not enforced is a comment */
  liteRunSql(self, "PRAGMA foreign_keys = ON");

  return self;
}

__attribute__((constructor)) static void registerSqlite(void) {
  dbmRegisterDriver("sqlite3", liteOpen);
}
