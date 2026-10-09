/**
 * PostgreSQL, and so CockroachDB, which speaks the same protocol.
 *
 * What the driver brings is the connection and four functions; everything
 * else is base. The overrides are the places PostgreSQL disagrees with the
 * generic SQL: a key that counts itself up is a type of its own (`SERIAL`)
 * rather than a constraint, a timestamp is `TIMESTAMP` rather than base's
 * `INTEGER`, and changing a column is three statements rather than one.
 *
 * The queries go through meta_pg.h, so they park the task they run in rather
 * than the thread - and from `main`, where there is no task, they wait the
 * way a thread waits.
 */
#include "pg_driver.h"

#include <db_migrate/pg.h>

#include <meta_pg.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static PGconn *connectionOf(driver_t *self) {
  return ((dbm_pg_t *)self)->conn;
}

/** Whether a result is a success, and the server's words when it is not. */
static int judged(driver_t *self, PGresult *result) {

  if (result == NULL)
    return dbmFail(self, TEXT`${PQerrorMessage(connectionOf(self))}`);

  ExecStatusType status = PQresultStatus(result);

  if (status in {PGRES_COMMAND_OK, PGRES_TUPLES_OK, PGRES_EMPTY_QUERY})
    return 0;

  /* what node's pg driver puts on its error, for dbmSayFailure */
  static const struct { int code; const char *name; } fields[] = {
      {PG_DIAG_SQLSTATE, "code"},
      {PG_DIAG_MESSAGE_DETAIL, "detail"},
      {PG_DIAG_MESSAGE_HINT, "hint"},
      {PG_DIAG_CONTEXT, "where"},
      {PG_DIAG_SCHEMA_NAME, "schema"},
      {PG_DIAG_TABLE_NAME, "table"},
      {PG_DIAG_COLUMN_NAME, "column"},
      {PG_DIAG_DATATYPE_NAME, "dataType"},
      {PG_DIAG_CONSTRAINT_NAME, "constraint"},
  };

  for (size_t i = 0; i < sizeof fields / sizeof fields[0]; ++i)
    dbmFailedField(self, fields[i].name,
                   PQresultErrorField(result, fields[i].code));

  const char *position = PQresultErrorField(result, PG_DIAG_STATEMENT_POSITION);
  const char *primary = PQresultErrorField(result, PG_DIAG_MESSAGE_PRIMARY);

  self->failedPosition = position != NULL ? atol(position) : 0;

  /* the message alone: the statement and its marker are said apart now */
  if (primary != NULL)
    return dbmFail(self, TEXT`${primary}`);

  return dbmFail(self, TEXT`${PQresultErrorMessage(result)}`);
}

static int pgRunSql(driver_t *self, const char *text) {

  PGresult *result = connectionOf(self).query(text);
  int answer = judged(self, result);

  PQclear(result);
  return answer;
}

/**
 * A result as a JSON array of objects, keyed by column name.
 *
 * The kind comes from the column's type where JSON has one - booleans,
 * integers, reals - and is text everywhere else, which is what libpq hands
 * over anyway. `numeric` stays text on purpose: a decimal that went through a
 * double is a different number.
 */
static json_t rowsOf(PGresult *result) {

  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *rows = yyjson_mut_arr(doc);
  int columns = PQnfields(result);

  for (int row = 0; row < PQntuples(result); ++row) {

    yyjson_mut_val *object = yyjson_mut_obj(doc);

    for (int column = 0; column < columns; ++column) {

      const char *name = PQfname(result, column);
      const char *value = PQgetvalue(result, row, column);
      yyjson_mut_val *held;

      if (PQgetisnull(result, row, column)) {
        held = yyjson_mut_null(doc);
      } else {

        /* the OIDs of pg_type, which have not changed since they were given */
        switch (PQftype(result, column)) {
        case 16:
          held = yyjson_mut_bool(doc, value[0] == 't');
          break;
        case 20:
        case 21:
        case 23:
          held = yyjson_mut_sint(doc, strtoll(value, NULL, 10));
          break;
        case 700:
        case 701:
          held = yyjson_mut_real(doc, strtod(value, NULL));
          break;
        default:
          held = yyjson_mut_strcpy(doc, value);
          break;
        }
      }

      yyjson_mut_obj_add(object, yyjson_mut_strcpy(doc, name), held);
    }

    yyjson_mut_arr_append(rows, object);
  }

  yyjson_mut_doc_set_root(doc, rows);

  return meta_jsonFromMut(doc);
}

static int pgQuery(driver_t *self, const sql_t *query, json_t *rows) {

  PGresult *result = connectionOf(self).ask(query);
  int answer = judged(self, result);

  if (answer == 0 && rows != NULL)
    *rows = rowsOf(result);

  PQclear(result);
  return answer;
}

static void pgClose(driver_t *self) {
  connectionOf(self).close();
}

/* ------------------------------------------------------------------ */
/* where PostgreSQL says it differently                               */
/* ------------------------------------------------------------------ */

const char *dbmPgMapDataType(driver_t *self, const char *type) {

  if (type in {"json", "jsonb", "uuid", "timestamptz", "timetz", "inet",
               "cidr", "bytea"})
    return NULL; /* PostgreSQL's own, written as they are */

  if (strcmp(type, "string") == 0)
    return "VARCHAR";

  if (strcmp(type, "datetime") == 0)
    return "TIMESTAMP";

  if (strcmp(type, "blob") == 0)
    return "BYTEA";

  return dbmBaseMapDataType(self, type);
}

/**
 * A key that counts itself up has no type of its own here: `SERIAL` and
 * `BIGSERIAL` are the type. So for that column the type is replaced rather
 * than followed by a constraint.
 */
int dbmPgColumnDef(driver_t *self, dbm_text_t *out, const char *table,
                   const char *column, json_t spec,
                   const dbm_column_options_t *options) {

  if (!spec.autoIncrement.truth())
    return dbmBaseColumnDef(self, out, table, column, spec, options);

  const char *type = dbmColumnType(spec);

  self->quoteName(self, out, column);
  out->put(type in {"bigint", "BIGINT"} ? " BIGSERIAL" : " SERIAL");

  return self->columnConstraint(self, out, table, column, spec, options);
}

/**
 * ALTER COLUMN, one clause per thing the spec says, all in one statement.
 * What it does not mention it leaves alone - `{notNull: false}` drops a NOT
 * NULL, and a spec without `notNull` keeps whatever was there.
 */
int dbmPgChangeColumn(driver_t *self, const char *table, const char *column,
                      json_t spec) {

  dbm_text_t sql = {0};
  defer sql.release();

  bool first = true;

  sql.put("ALTER TABLE ");
  self->quoteName(self, &sql, table);

  if (!spec.type.isNothing()) {

    const char *type = spec.type;
    const char *mapped = self->mapDataType(self, type);
    long length = spec.length;

    sql.put(first ? " ALTER COLUMN " : ", ALTER COLUMN ");
    self->quoteName(self, &sql, column);
    sql.put(" TYPE ");
    sql.put(mapped != NULL ? mapped : type);

    if (length > 0)
      sql.append(TEXT`(${length})`);

    first = false;
  }

  if (!spec.notNull.isNothing()) {
    sql.put(first ? " ALTER COLUMN " : ", ALTER COLUMN ");
    self->quoteName(self, &sql, column);
    sql.put(spec.notNull.truth() ? " SET NOT NULL" : " DROP NOT NULL");
    first = false;
  }

  if (!spec.defaultValue.isNothing()) {

    sql.put(first ? " ALTER COLUMN " : ", ALTER COLUMN ");
    self->quoteName(self, &sql, column);

    if (strcmp(spec.defaultValue.kind(), "null") == 0) {
      sql.put(" DROP DEFAULT");
    } else {
      sql.put(" SET DEFAULT ");

      if (self->valueLiteral(self, &sql, spec.defaultValue))
        return -1;
    }

    first = false;
  }

  if (first)
    return dbmFail(self, TEXT`changeColumn on ${table}.${column} that says nothing to change`);

  return dbmSend(self, &sql);
}

/**
 * PostgreSQL has no CREATE DATABASE IF NOT EXISTS, so it is asked first.
 * CockroachDB has it, and keeps the generic version.
 */
static int pgCreateDatabase(driver_t *self, const char *name, bool ifNotExists) {

  if (ifNotExists && !self->dryRun) {

    json_t found = {0};
    sql_t query = SQL`SELECT 1 FROM pg_database WHERE datname = ${name}`;
    defer query.release();

    if (dbmQuery(self, &query, &found))
      return -1;

    int there = found.count();

    found.release();

    if (there > 0)
      return 0;
  }

  return dbmBaseCreateDatabase(self, name, false);
}

/* ------------------------------------------------------------------ */
/* what only PostgreSQL has                                           */
/* ------------------------------------------------------------------ */

/**
 * The methods `db_migrate/pg.h` adds to `migrator_t`. They exist because the
 * header was included, and they say so when the database is not this one.
 */
static int onlyHere(migrator_t *self, const char *what) {

  if (self->failed)
    return -1;

  if (!(self->driver->name in {"pg", "cockroachdb"}))
    return self->fail(
        TEXT`${what} is PostgreSQL's, and this is ${self->driver->name}`);

  return 0;
}

static int sendOrFail(migrator_t *self, dbm_text_t *sql) {

  if (dbmSend(self->driver, sql) == 0)
    return 0;

  return self->fail(TEXT`${self->driver->error}`);
}

int migrator_t__createSequence(migrator_t *self, const char *name,
                               json_t spec) {

  dbm_text_t sql = {0};
  defer sql.release();

  if (onlyHere(self, "createSequence"))
    return -1;

  sql.put("CREATE SEQUENCE ");
  self->driver->quoteName(self->driver, &sql, name);

  long start = spec.start;
  long increment = spec.increment;

  if (!spec.start.isNothing())
    sql.append(TEXT` START WITH ${start}`);

  if (!spec.increment.isNothing())
    sql.append(TEXT` INCREMENT BY ${increment}`);

  return sendOrFail(self, &sql);
}

int migrator_t__dropSequence(migrator_t *self, const char *name) {

  dbm_text_t sql = {0};
  defer sql.release();

  if (onlyHere(self, "dropSequence"))
    return -1;

  sql.put("DROP SEQUENCE ");
  self->driver->quoteName(self->driver, &sql, name);

  return sendOrFail(self, &sql);
}

/**
 * Enums, which PostgreSQL has and CockroachDB inherited. `DROP VALUE` is
 * CockroachDB's alone, and PostgreSQL says so itself when asked.
 */
int migrator_t__createEnum(migrator_t *self, const char *name, json_t values) {

  dbm_text_t sql = {0};
  defer sql.release();

  if (onlyHere(self, "createEnum"))
    return -1;

  if (strcmp(values.kind(), "array") != 0 || values.count() == 0)
    return self->fail(
        TEXT`createEnum takes the values as an array: ["small", "large"]`);

  sql.put("CREATE TYPE ");
  self->driver->quoteName(self->driver, &sql, name);
  sql.put(" AS ENUM (");

  for (int i = 0; i < values.count(); ++i) {

    if (i > 0)
      sql.put(", ");

    self->driver->quoteText(self->driver, &sql, values[i].text());
  }

  sql.put(")");
  return sendOrFail(self, &sql);
}

/** `ALTER TYPE "name" <what> '<value>'`, the shape three of them share. */
static int alterType(migrator_t *self, const char *method, const char *name,
                     const char *what, const char *value, bool quoted) {

  dbm_text_t sql = {0};
  defer sql.release();

  if (onlyHere(self, method))
    return -1;

  sql.put("ALTER TYPE ");
  self->driver->quoteName(self->driver, &sql, name);
  sql.put(what);

  if (quoted)
    self->driver->quoteText(self->driver, &sql, value);
  else
    self->driver->quoteName(self->driver, &sql, value);

  return sendOrFail(self, &sql);
}

int migrator_t__renameEnum(migrator_t *self, const char *name,
                           const char *to) {
  return alterType(self, "renameEnum", name, " RENAME TO ", to, false);
}

int migrator_t__addEnumType(migrator_t *self, const char *name,
                            const char *value) {
  return alterType(self, "addEnumType", name, " ADD VALUE ", value, true);
}

int migrator_t__dropEnumType(migrator_t *self, const char *name,
                             const char *value) {
  return alterType(self, "dropEnumType", name, " DROP VALUE ", value, true);
}

int migrator_t__dropEnum(migrator_t *self, const char *name) {

  dbm_text_t sql = {0};
  defer sql.release();

  if (onlyHere(self, "dropEnum"))
    return -1;

  sql.put("DROP TYPE ");
  self->driver->quoteName(self->driver, &sql, name);

  return sendOrFail(self, &sql);
}

/* ------------------------------------------------------------------ */
/* opening one                                                        */
/* ------------------------------------------------------------------ */

/** `key='value'`, quoted the way libpq's connection strings quote. */
static void putSetting(dbm_text_t *out, const char *key, const char *value) {

  if (value == NULL || value[0] == '\0')
    return;

  if (out->length > 0)
    out->put(" ");

  out->put(key);
  out->put("='");

  for (const char *at = value; *at != '\0'; ++at) {

    if (*at in {'\'', '\\'})
      out->put("\\");

    out->putn(at, 1);
  }

  out->put("'");
}

/**
 * From a database.json entry. Either a URL - `{driver: "pg", url: ...}`, or
 * a string on its own - or the fields node db-migrate reads: host, port,
 * user, password, database. A port written as a number is fine.
 */
driver_t *dbmPgConnect(json_t config, char *why, size_t room, const char *name,
                       size_t size) {

  dbm_text_t settings = {0};
  defer settings.release();

  char port[16] = "";
  const char *url = config.url;

  if (strcmp(config.kind(), "string") == 0)
    url = config.text();

  if (url[0] != '\0') {
    settings.put(url);
  } else {

    if (strcmp(config.port.kind(), "number") == 0)
      dbmWrite(port, sizeof port, TEXT`${(long)config.port}`);
    else
      dbmWrite(port, sizeof port, TEXT`${(const char *)config.port}`);

    putSetting(&settings, "host", config.host);
    putSetting(&settings, "port", port);
    putSetting(&settings, "user", config.user);
    putSetting(&settings, "password", config.password);
    /**
     * No database is how `db:create` asks to be connected: to the one every
     * server has, from which another can be made. libpq's own default - a
     * database named after the user - is there on a developer's machine and
     * nowhere else.
     */
    const char *database = config.database;

    if (database[0] == '\0')
      database = strcmp(name, "cockroachdb") == 0 ? "defaultdb" : "postgres";

    putSetting(&settings, "dbname", database);

    /* node reads it as `ssl.sslmode`; a plain `sslmode` is accepted too */
    const char *sslmode = config.sslmode;

    putSetting(&settings, "sslmode",
               sslmode[0] != '\0' ? sslmode : config.ssl.sslmode.text());
  }

  if (settings.failed) {
    dbmWrite(why, room, TEXT`out of memory`);
    return NULL;
  }

  PGconn *conn = meta_pgOpen(settings.text != NULL ? settings.text : "");

  if (conn == NULL) {
    dbmWrite(why, room, TEXT`could not connect to PostgreSQL with what the configuration says`);
    return NULL;
  }

  if (PQstatus(conn) != CONNECTION_OK) {
    dbmWrite(why, room, TEXT`${PQerrorMessage(conn)}`);
    PQfinish(conn);
    return NULL;
  }

  dbm_pg_t *pg = (dbm_pg_t *)dbmDriverNew(
      name, size < sizeof(dbm_pg_t) ? sizeof(dbm_pg_t) : size);

  if (pg == NULL) {
    PQfinish(conn);
    dbmWrite(why, room, TEXT`out of memory`);
    return NULL;
  }

  pg->conn = conn;

  driver_t *self = &pg->base;

  /* what every driver has to bring */
  self->runSql = pgRunSql;
  self->query = pgQuery;
  self->close = pgClose;

  /* and what PostgreSQL says differently */
  self->mapDataType = dbmPgMapDataType;
  self->columnDef = dbmPgColumnDef;
  self->changeColumn = dbmPgChangeColumn;

  /* `delay` and `defaultValue` for a NOT NULL column v2 drops, as node's pg */
  self->columnStrategies = true;

  if (strcmp(name, "cockroachdb") != 0)
    self->createDatabase = pgCreateDatabase;

  /* the notices about implicit indexes are noise on every CREATE TABLE */
  pgRunSql(self, "SET client_min_messages TO WARNING");

  /* `schema`: where its tables are and where it makes them, as node's pg */
  const char *schema = config.schema;

  if (schema[0] != '\0') {

    dbm_text_t sql = {0};
    defer sql.release();

    sql.put("SET search_path TO ");
    self->quoteName(self, &sql, schema);

    if (sql.failed || pgRunSql(self, sql.text)) {
      dbmWrite(why, room, TEXT`could not use the schema ${schema}: ${self->error}`);
      dbmClose(self);
      return NULL;
    }
  }

  return self;
}

static driver_t *pgOpen(json_t config, char *why, size_t room) {
  return dbmPgConnect(config, why, room, "pg", sizeof(dbm_pg_t));
}

__attribute__((constructor)) static void registerPg(void) {
  dbmRegisterDriver("pg", pgOpen);
}
