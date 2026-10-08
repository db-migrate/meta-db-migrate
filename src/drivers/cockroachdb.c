/**
 * CockroachDB: PostgreSQL's driver, with CockroachDB's differences.
 *
 * node's db-migrate-cockroachdb extends db-migrate-pg's `base`, and this
 * does the same: `dbmPgConnect` hands back a driver with every PostgreSQL
 * slot set, and the slots below are replaced. What PostgreSQL does here
 * differently is reached as its own function, the way `_super` was.
 *
 * What differs:
 *
 * - types: `uuid`, `jsonb`, `float` and `timestamptz` are CockroachDB's own
 *   words, an `enum` column names its type, and a `computed` column is an
 *   expression rather than a type
 * - a key that counts itself up may be a UUID, filled by gen_random_uuid()
 * - `CURRENT_TIMESTAMP()` and `NOW()` are function calls, and a column may
 *   say what it becomes `ON UPDATE`
 * - an index belongs to its table: `DROP INDEX "t"@"i"`
 * - a table may expire its rows: `{columns: ..., expireAfter: "30 days"}`
 * - `changePrimaryKey`, which PostgreSQL has no single statement for
 *
 * And one thing that is the database's, not the driver's. A table created in
 * a transaction can be changed and written in it straight away. A table that
 * already existed cannot: a column added to it is not usable until the
 * transaction commits - `column "slug" does not exist` - and a column dropped
 * from it still holds on to its type. Measured on v24.3 with both schema
 * changers. So adding a column to an existing table and filling it are two
 * migrations, and so are dropping a column and dropping the enum it used.
 */
#include "pg_driver.h"

#include <db_migrate/cockroachdb.h>

#include <stdio.h>
#include <string.h>

static const char *crdbMapDataType(driver_t *self, const char *type) {

  /* CockroachDB's own, written as they are */
  if (type in {"float", "uuid", "jsonb", "timestamptz", "FLOAT", "UUID",
               "JSONB", "TIMESTAMPTZ"})
    return NULL;

  return dbmPgMapDataType(self, type);
}

/**
 * The expression a special default or ON UPDATE value stands for here.
 * `CURRENT_TIMESTAMP` is a function in CockroachDB, and `NOW` was always
 * one.
 */
static int crdbExpression(driver_t *self, dbm_text_t *out, json_t value) {

  if (strcmp(value.kind(), "object") == 0) {

    const char *special = value.special;

    if (strcmp(special, "CURRENT_TIMESTAMP") == 0) {
      out->put("CURRENT_TIMESTAMP()");
      return 0;
    }

    if (strcmp(special, "NOW") == 0) {
      out->put("NOW()");
      return 0;
    }

    const char *raw = value.raw;

    if (raw[0] != '\0') {
      out->put(raw);
      return 0;
    }

    return dbmFail(self, TEXT`a value that is an object but says neither \`special\` nor \`raw\``);
  }

  return self->valueLiteral(self, out, value);
}

static int crdbColumnConstraint(driver_t *self, dbm_text_t *out,
                                const char *table, const char *column,
                                json_t spec,
                                const dbm_column_options_t *options) {

  (void)table;
  (void)column;

  if (spec.primaryKey.truth() && options->soleKey)
    out->put(" PRIMARY KEY");

  if (spec.notNull.truth())
    out->put(" NOT NULL");

  if (spec.unique.truth())
    out->put(" UNIQUE");

  /* a uuid key fills itself, which is what `autoIncrement` means for one */
  if (spec.autoIncrement.truth() &&
      dbmColumnType(spec) in {"uuid", "UUID"}) {
    out->put(" DEFAULT gen_random_uuid()");
  } else if (!spec.defaultValue.isNothing() ||
             strcmp(spec.defaultValue.kind(), "null") == 0) {
    out->put(" DEFAULT ");

    if (crdbExpression(self, out, spec.defaultValue))
      return -1;
  }

  if (!spec.onUpdate.isNothing()) {
    out->put(" ON UPDATE ");

    if (crdbExpression(self, out, spec.onUpdate))
      return -1;
  }

  return 0;
}

static int crdbColumnDef(driver_t *self, dbm_text_t *out, const char *table,
                         const char *column, json_t spec,
                         const dbm_column_options_t *options) {

  const char *type = dbmColumnType(spec);

  /* `{type: "enum", enumName: "size"}` - the column's type is the enum */
  if (type in {"enum", "ENUM"}) {

    const char *named = spec.enumName;

    if (named[0] == '\0')
      return dbmFail(self, TEXT`the enum column ${table}.${column} does not say which enum - \`enumName\``);

    self->quoteName(self, out, column);
    out->put(" ");
    self->quoteName(self, out, named);
    return self->columnConstraint(self, out, table, column, spec, options);
  }

  /**
   * `{type: "computed", computedType: "int", function: "a + b", stored:
   * true}` - a column that is an expression over the others. The function
   * is the migration author's own SQL, trusted the way `runSql` is.
   */
  if (type in {"computed", "COMPUTED"}) {

    const char *of = spec.computedType;
    const char *function = spec.function;
    const char *mapped = self->mapDataType(self, of);

    if (of[0] == '\0' || function[0] == '\0')
      return dbmFail(self, TEXT`the computed column ${table}.${column} needs \`computedType\` and \`function\``);

    self->quoteName(self, out, column);
    out->put(" ");
    out->put(mapped != NULL ? mapped : of);
    out->put(" AS (");
    out->put(function);
    out->put(spec.stored.truth() ? ") STORED" : ")");
    return self->columnConstraint(self, out, table, column, spec, options);
  }

  /* a uuid key is a uuid, not a SERIAL */
  if (spec.autoIncrement.truth() && type in {"uuid", "UUID"}) {
    self->quoteName(self, out, column);
    out->put(" UUID");
    return self->columnConstraint(self, out, table, column, spec, options);
  }

  return dbmPgColumnDef(self, out, table, column, spec, options);
}

/** An index is reached through its table: `"t"@"i"`. */
static int crdbRemoveIndex(driver_t *self, const char *table,
                           const char *name) {

  dbm_text_t sql = {0};
  defer sql.release();

  sql.put("DROP INDEX ");

  if (table != NULL && table[0] != '\0') {
    self->quoteName(self, &sql, table);
    sql.put("@");
  }

  self->quoteName(self, &sql, name);

  return dbmSend(self, &sql);
}

/**
 * PostgreSQL's ALTER COLUMN, and then `unique`, which CockroachDB keeps as a
 * constraint named `<table>_<column>_key` - added, or dropped with its index.
 */
static int crdbChangeColumn(driver_t *self, const char *table,
                            const char *column, json_t spec) {

  bool unique = !spec.unique.isNothing();
  char key[256];

  /* everything but `unique`, if there is anything but `unique` */
  if (spec.count() > (unique ? 1 : 0) &&
      dbmPgChangeColumn(self, table, column, spec))
    return -1;

  if (!unique)
    return 0;

  dbm_text_t sql = {0};
  defer sql.release();

  dbmWrite(key, sizeof key, TEXT`${table}_${column}_key`);

  if (spec.unique.truth()) {
    sql.put("ALTER TABLE ");
    self->quoteName(self, &sql, table);
    sql.put(" ADD CONSTRAINT ");
    self->quoteName(self, &sql, key);
    sql.put(" UNIQUE (");
    self->quoteName(self, &sql, column);
    sql.put(")");
  } else {
    sql.put("DROP INDEX ");
    self->quoteName(self, &sql, table);
    sql.put("@");
    self->quoteName(self, &sql, key);
    sql.put(" CASCADE");
  }

  return dbmSend(self, &sql);
}

/**
 * Row expiry: `expire` is an expression saying when a row is due,
 * `expireAfter` an interval after which every row is, and `ttlJobCron` when
 * the job that removes them runs.
 */
static int crdbTableOptions(driver_t *self, dbm_text_t *out, const char *table,
                            json_t spec) {

  const char *expire = spec.expire;
  const char *after = spec.expireAfter;
  const char *cron = spec.ttlJobCron;
  bool first = true;

  (void)table;

  if (expire[0] == '\0' && after[0] == '\0' && cron[0] == '\0')
    return 0;

  out->put(" WITH (");

  if (expire[0] != '\0') {
    out->put("ttl_expiration_expression = ");
    self->quoteText(self, out, expire);
    first = false;
  }

  if (after[0] != '\0') {
    out->put(first ? "ttl_expire_after = " : ", ttl_expire_after = ");
    self->quoteText(self, out, after);
    first = false;
  }

  if (cron[0] != '\0') {
    out->put(first ? "ttl_job_cron = " : ", ttl_job_cron = ");
    self->quoteText(self, out, cron);
  }

  out->put(")");
  return 0;
}

/* ------------------------------------------------------------------ */
/* what only CockroachDB has                                          */
/* ------------------------------------------------------------------ */

/**
 * `db->changePrimaryKey("pets", ["owner_id", "id"])`. CockroachDB says this
 * in one statement and rebuilds the table behind it; PostgreSQL has no such
 * statement, so it is refused there by name.
 */
int migrator_t__changePrimaryKey(migrator_t *self, const char *table,
                                 json_t columns) {

  dbm_text_t sql = {0};
  defer sql.release();

  if (self->failed)
    return -1;

  if (strcmp(self->driver->name, "cockroachdb") != 0)
    return self->fail(TEXT`changePrimaryKey is CockroachDB's, and this is ${
        self->driver->name}`);

  if (strcmp(columns.kind(), "array") != 0 || columns.count() == 0)
    return self->fail(
        TEXT`changePrimaryKey takes the new key's columns as an array`);

  sql.put("ALTER TABLE ");
  self->driver->quoteName(self->driver, &sql, table);
  sql.put(" ALTER PRIMARY KEY USING COLUMNS (");

  for (int i = 0; i < columns.count(); ++i) {

    if (i > 0)
      sql.put(", ");

    self->driver->quoteName(self->driver, &sql, columns[i].text());
  }

  sql.put(")");

  if (dbmSend(self->driver, &sql) == 0)
    return 0;

  return self->fail(TEXT`${self->driver->error}`);
}

static driver_t *crdbOpen(json_t config, char *why, size_t room) {

  driver_t *self =
      dbmPgConnect(config, why, room, "cockroachdb", sizeof(dbm_pg_t));

  if (self == NULL)
    return NULL;

  self->mapDataType = crdbMapDataType;
  self->columnDef = crdbColumnDef;
  self->columnConstraint = crdbColumnConstraint;
  self->removeIndex = crdbRemoveIndex;
  self->changeColumn = crdbChangeColumn;
  self->tableOptions = crdbTableOptions;

  return self;
}

__attribute__((constructor)) static void registerCockroach(void) {
  dbmRegisterDriver("cockroachdb", crdbOpen);
}
