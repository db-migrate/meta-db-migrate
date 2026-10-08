/**
 * db-migrate-base: the SQL every database agrees on.
 *
 * Every function here is a slot default. They reach each other through
 * `self`, never by name, so a driver that overrides one piece - how a type is
 * spelled, how a column is written - changes every statement built from it.
 * That is the part of the JavaScript class worth keeping; the rest of it was
 * arguments-sniffing and callbacks, and neither survives the trip.
 */
#include <db_migrate_driver.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* text                                                               */
/* ------------------------------------------------------------------ */

void dbm_text_t__putn(dbm_text_t *self, const char *text, size_t length) {

  if (self->failed || text == NULL)
    return;

  if (self->length + length + 1 > self->room) {

    size_t room = self->room == 0 ? 256 : self->room;

    while (self->length + length + 1 > room)
      room *= 2;

    char *grown = realloc(self->text, room);

    if (grown == NULL) {
      self->failed = true;
      return;
    }

    self->text = grown;
    self->room = room;
  }

  memcpy(self->text + self->length, text, length);
  self->length += length;
  self->text[self->length] = '\0';
}

void dbm_text_t__put(dbm_text_t *self, const char *text) {

  if (text != NULL)
    self->putn(text, strlen(text));
}

/**
 * Measured first, then written in place - the template formats its values
 * once into the room it asked for, with no buffer guessed in between.
 */
void dbm_text_t__append(dbm_text_t *self, text_t piece) {

  size_t need = piece.length();

  if (self->failed)
    return;

  if (self->length + need + 1 > self->room) {

    size_t room = self->room == 0 ? 256 : self->room;

    while (self->length + need + 1 > room)
      room *= 2;

    char *grown = realloc(self->text, room);

    if (grown == NULL) {
      self->failed = true;
      return;
    }

    self->text = grown;
    self->room = room;
  }

  piece.into(self->text + self->length, need + 1);
  self->length += need;
}

size_t dbmWrite(char *into, size_t room, text_t text) {
  return text.into(into, room);
}

static int logLevel = DBM_LOG_INFO | DBM_LOG_WARN | DBM_LOG_ERROR | DBM_LOG_SQL;

void dbmSetLogLevel(const char *levels) {

  const char *at = levels;

  logLevel = 0;

  while (*at != '\0') {

    size_t length = strcspn(at, "|");

    if (length == 4 && strncmp(at, "info", 4) == 0)
      logLevel |= DBM_LOG_INFO;
    else if (length == 4 && strncmp(at, "warn", 4) == 0)
      logLevel |= DBM_LOG_WARN;
    else if (length == 5 && strncmp(at, "error", 5) == 0)
      logLevel |= DBM_LOG_ERROR;
    else if (length == 3 && strncmp(at, "sql", 3) == 0)
      logLevel |= DBM_LOG_SQL;

    at += length + (at[length] == '|');
  }
}

bool dbmLogs(int level) {
  return (logLevel & level) != 0;
}

/** The level a line is at, by its mark; 0 for one that always goes out. */
static int levelOf(const char *text) {

  if (strncmp(text, "[INFO]", 6) == 0)
    return DBM_LOG_INFO;

  if (strncmp(text, "[WARN]", 6) == 0)
    return DBM_LOG_WARN;

  if (strncmp(text, "[ERROR]", 7) == 0)
    return DBM_LOG_ERROR;

  if (strncmp(text, "[SQL]", 5) == 0)
    return DBM_LOG_SQL;

  return 0;
}

void dbmSay(FILE *to, text_t line) {

  char *text = line.owned();

  /* flushed: piped into a CI log, an error stays after the line it is about */
  if (text != NULL && (levelOf(text) == 0 || dbmLogs(levelOf(text)))) {
    fputs(text, to);
    fflush(to);
  }

  free(text);
}

void dbm_text_t__release(dbm_text_t *self) {

  free(self->text);
  self->text = NULL;
  self->length = 0;
  self->room = 0;
  self->failed = false;
}

/* ------------------------------------------------------------------ */
/* failing                                                            */
/* ------------------------------------------------------------------ */

int dbmFail(driver_t *self, text_t why) {

  why.into(self->error, sizeof self->error);
  return -1;
}

static int notBrought(driver_t *self, const char *what) {
  return dbmFail(self, TEXT`the ${self->name} driver does not bring ${what}`);
}

static int missingRunSql(driver_t *self, const char *text) {
  (void)text;
  return notBrought(self, "runSql");
}

static int missingQuery(driver_t *self, const sql_t *query, json_t *rows) {
  (void)query;
  (void)rows;
  return notBrought(self, "query");
}

static void missingClose(driver_t *self) { (void)self; }

static int missingChangeColumn(driver_t *self, const char *table,
                               const char *column, json_t spec) {
  (void)table;
  (void)column;
  (void)spec;
  return notBrought(self, "changeColumn");
}

/**
 * Runs, or on a dry run says what it would have run. Every generic version
 * sends through here, so a dry run is one line rather than one per statement.
 */
/** Whether SQL ends in its own `;` - an .sql file's text usually does. */
static bool terminated(const char *sql) {

  size_t length = strlen(sql);

  while (length > 0 && strchr(" \t\r\n", sql[length - 1]) != NULL)
    --length;

  return length > 0 && sql[length - 1] == ';';
}

int dbmSend(driver_t *self, dbm_text_t *sql) {

  if (sql->failed)
    return dbmFail(self, TEXT`out of memory writing a statement`);

  /* a dry run's statements are its output, and node let --log-level hide them */
  if (self->dryRun) {
    if (dbmLogs(DBM_LOG_SQL))
      dbmSay(stdout, TEXT`${sql->text}${terminated(sql->text) ? "" : ";"}\n`);
    return 0;
  }

  if (self->verbose)
    dbmSay(stdout, TEXT`[SQL] ${sql->text}\n`);

  return self->runSql(self, sql->text);
}

int dbmQuery(driver_t *self, const sql_t *query, json_t *rows) {

  if (self->dryRun) {
    if (dbmLogs(DBM_LOG_SQL))
      dbmSay(stdout, TEXT`${query->text}; -- ${query->count} parameter(s)\n`);
    return 0;
  }

  if (self->verbose)
    dbmSay(stdout, TEXT`[SQL] ${query->text} -- ${query->count} parameter(s)\n`);

  return self->query(self, query, rows);
}

/* ------------------------------------------------------------------ */
/* the pieces SQL is made of                                          */
/* ------------------------------------------------------------------ */

/**
 * db-migrate's own type names, which a migration uses so it does not have to
 * know the database. A name that is not one of them is the database's own
 * and goes through as written, upper-cased the way base always did.
 */
typedef enum dbmType {
  DBM_STRING "string",
  DBM_TEXT "text",
  DBM_CHAR "char",
  DBM_INT "int",
  DBM_INTEGER "integer",
  DBM_SMALLINT "smallint",
  DBM_BIGINT "bigint",
  DBM_REAL "real",
  DBM_DECIMAL "decimal",
  DBM_BOOLEAN "boolean",
  DBM_DATE "date",
  DBM_DATETIME "datetime",
  DBM_TIME "time",
  DBM_TIMESTAMP "timestamp",
  DBM_BLOB "blob",
  DBM_BINARY "binary",
} dbm_type_t;

const char *dbmBaseMapDataType(driver_t *self, const char *type) {

  (void)self;

  match (enum dbmType, fromspelling(enum dbmType, type)) {
  case DBM_STRING:
    return "VARCHAR";
  case DBM_TEXT:
    return "TEXT";
  case DBM_CHAR:
    return "CHAR";
  case DBM_INT, DBM_INTEGER:
    return "INTEGER";
  case DBM_SMALLINT:
    return "SMALLINT";
  case DBM_BIGINT:
    return "BIGINT";
  case DBM_REAL:
    return "REAL";
  case DBM_DECIMAL:
    return "DECIMAL";
  case DBM_BOOLEAN:
    return "BOOLEAN";
  case DBM_DATE:
    return "DATE";
  case DBM_DATETIME, DBM_TIMESTAMP:
    return "TIMESTAMP";
  case DBM_TIME:
    return "TIME";
  case DBM_BLOB:
    return "BLOB";
  case DBM_BINARY:
    return "BINARY";
  default:
    return NULL;
  }
}

/** What a type name becomes when nobody recognised it: the name, shouted. */
static void putType(driver_t *self, dbm_text_t *out, const char *type) {

  const char *mapped = self->mapDataType(self, type);

  if (mapped != NULL) {
    out->put(mapped);
    return;
  }

  /* only letters, digits, spaces and parentheses - a type is not a statement */
  for (const char *at = type; *at != '\0'; ++at) {

    char upper = (char)toupper((unsigned char)*at);

    if (isalnum((unsigned char)*at) || *at in {' ', '_', '(', ')', ','})
      out->putn(&upper, 1);
  }
}

void dbmBaseQuoteName(driver_t *self, dbm_text_t *out, const char *name) {

  (void)self;
  out->put("\"");

  for (const char *at = name; *at != '\0'; ++at)
    out->put(*at == '"' ? "\"\"" : (char[]){*at, 0});

  out->put("\"");
}

void dbmBaseQuoteText(driver_t *self, dbm_text_t *out, const char *text) {

  (void)self;
  out->put("'");

  for (const char *at = text; *at != '\0'; ++at)
    out->put(*at == '\'' ? "''" : (char[]){*at, 0});

  out->put("'");
}

/**
 * A value from a document as a SQL literal, for the few places that cannot
 * take a parameter: a column's DEFAULT is part of the DDL, and DDL has no
 * parameters in any database this speaks to.
 */
int dbmBaseValueLiteral(driver_t *self, dbm_text_t *out, json_t value) {

  const char *kind = value.kind();

  if (strcmp(kind, "string") == 0) {
    self->quoteText(self, out, value.text());
    return 0;
  }

  if (strcmp(kind, "bool") == 0) {
    out->put(value.truth() ? "TRUE" : "FALSE");
    return 0;
  }

  if (strcmp(kind, "null") == 0) {
    out->put("NULL");
    return 0;
  }

  if (strcmp(kind, "number") == 0) {

    /* the shortest form that reads back as the same number, which TEXT writes */
    if (yyjson_is_real(value.node))
      out->append(TEXT`${value.real()}`);
    else
      out->append(TEXT`${value.number()}`);

    return 0;
  }

  /* a document as a value is its text, which is what a json column takes */
  char *text = yyjson_val_write(value.node, 0, NULL);

  if (text == NULL)
    return dbmFail(self, TEXT`a value that could not be written as JSON`);

  self->quoteText(self, out, text);
  free(text);

  return 0;
}

const char *dbmColumnType(json_t spec) {

  if (strcmp(spec.kind(), "string") == 0)
    return spec.text();

  return spec.type.text();
}

/**
 * `DEFAULT 'x'`, `DEFAULT 4`, `DEFAULT CURRENT_TIMESTAMP`.
 *
 * `{special: "CURRENT_TIMESTAMP"}` is db-migrate's spelling for a default
 * that is an expression rather than a value, and `{raw: "now()"}` says the
 * same for anything else - written into the DDL as it is, which is the
 * migration author's own SQL and is trusted the way `runSql` is.
 */
int dbmBaseDefault(driver_t *self, dbm_text_t *out, json_t value) {

  out->put(" DEFAULT ");

  if (strcmp(value.kind(), "object") == 0) {

    const char *special = value.special;
    const char *raw = value.raw;

    if (strcmp(special, "CURRENT_TIMESTAMP") == 0) {
      out->put("CURRENT_TIMESTAMP");
      return 0;
    }

    if (raw[0] != '\0') {
      out->put(raw);
      return 0;
    }

    return dbmFail(self, TEXT`a default that is an object but says neither \`special\` nor \`raw\``);
  }

  return self->valueLiteral(self, out, value);
}

int dbmBaseColumnConstraint(driver_t *self, dbm_text_t *out, const char *table,
                            const char *column, json_t spec,
                            const dbm_column_options_t *options) {

  (void)table;
  (void)column;

  if (spec.primaryKey.truth() && options->soleKey)
    out->put(" PRIMARY KEY");

  if (spec.notNull.truth())
    out->put(" NOT NULL");

  if (spec.unique.truth())
    out->put(" UNIQUE");

  if ((!spec.defaultValue.isNothing() ||
       strcmp(spec.defaultValue.kind(), "null") == 0) &&
      dbmBaseDefault(self, out, spec.defaultValue))
    return -1;

  if (self->inlineForeignKeys &&
      strcmp(spec.foreignKey.kind(), "object") == 0)
    return dbmBaseReferences(self, out, table, column, spec.foreignKey);

  return 0;
}

int dbmBaseColumnDef(driver_t *self, dbm_text_t *out, const char *table,
                     const char *column, json_t spec,
                     const dbm_column_options_t *options) {

  long length = spec.length;

  self->quoteName(self, out, column);
  out->put(" ");
  putType(self, out, dbmColumnType(spec));

  if (length > 0)
    out->append(TEXT`(${length})`);

  return self->columnConstraint(self, out, table, column, spec, options);
}

/* ------------------------------------------------------------------ */
/* schema                                                             */
/* ------------------------------------------------------------------ */

int dbmBaseStartMigration(driver_t *self) {
  dbm_text_t sql = {0};
  defer sql.release();
  sql.put("BEGIN");
  return dbmSend(self, &sql);
}

int dbmBaseEndMigration(driver_t *self) {
  dbm_text_t sql = {0};
  defer sql.release();
  sql.put("COMMIT");
  return dbmSend(self, &sql);
}

int dbmBaseAbortMigration(driver_t *self) {
  dbm_text_t sql = {0};
  defer sql.release();
  sql.put("ROLLBACK");
  return dbmSend(self, &sql);
}

/** The referential actions SQL has. Anything else is not one, and not SQL we write. */
static bool isRule(const char *rule) {

  static const char *const rules[] = {"CASCADE", "RESTRICT", "SET NULL",
                                      "SET DEFAULT", "NO ACTION"};

  for (size_t i = 0; i < countof(rules); ++i)
    if (strcasecmp(rule, rules[i]) == 0)
      return true;

  return false;
}

/** `"a", "b"` - a list of names, quoted. */
static void putNames(driver_t *self, dbm_text_t *out, const char *const *names,
                     size_t count) {

  for (size_t i = 0; i < count; ++i) {

    if (i > 0)
      out->put(", ");

    self->quoteName(self, out, names[i]);
  }
}

/**
 * The statement both kinds of foreign key come down to: the ones a column
 * spec declares and the ones `addForeignKey` is asked for.
 */
static int foreignKey(driver_t *self, const char *table, const char *referenced,
                      const char *name, const char *const *locals,
                      const char *const *foreigns, size_t count, json_t rules) {

  dbm_text_t sql = {0};
  defer sql.release();

  const char *onDelete = rules.onDelete;
  const char *onUpdate = rules.onUpdate;

  if (count == 0)
    return dbmFail(self, TEXT`a foreign key on ${table} with no columns in it`);

  sql.put("ALTER TABLE ");
  self->quoteName(self, &sql, table);
  sql.put(" ADD CONSTRAINT ");
  self->quoteName(self, &sql, name);
  sql.put(" FOREIGN KEY (");
  putNames(self, &sql, locals, count);
  sql.put(") REFERENCES ");
  self->quoteName(self, &sql, referenced);
  sql.put(" (");
  putNames(self, &sql, foreigns, count);
  sql.put(")");

  if (onDelete[0] != '\0') {

    if (!isRule(onDelete))
      return dbmFail(self, TEXT`\`${onDelete}\` is not something a foreign key can do on delete`);

    sql.put(" ON DELETE ");
    sql.put(onDelete);
  }

  if (onUpdate[0] != '\0') {

    if (!isRule(onUpdate))
      return dbmFail(self, TEXT`\`${onUpdate}\` is not something a foreign key can do on update`);

    sql.put(" ON UPDATE ");
    sql.put(onUpdate);
  }

  return dbmSend(self, &sql);
}

int dbmBaseAddForeignKey(driver_t *self, const char *table,
                         const char *referenced, const char *name,
                         json_t mapping, json_t rules) {

  const char *[] locals;
  const char *[] foreigns;

  /* handed on as `.items`, so meta cannot prove they stay here; said instead */
  defer locals.release();
  defer foreigns.release();

  if (strcmp(mapping.kind(), "object") != 0)
    return dbmFail(self, TEXT`the mapping of foreign key ${name} has to say \`{local: "foreign"}\``);

  if (self->inlineForeignKeys)
    return dbmFail(self, TEXT`${self->name} cannot add foreign key ${name} to a table that exists - declare it on the column with \`foreignKey\``);

  for (int i = 0; i < mapping.count(); ++i) {

    const char *local = mapping.keyAt(i);

    locals.push(local);
    foreigns.push(mapping.get(local).text());
  }

  return foreignKey(self, table, referenced, name, locals.items,
                    foreigns.items, locals.count, rules);
}

/** A column spec's `foreignKey: {name, table, mapping, rules}`, once the table exists. */
static int columnForeignKey(driver_t *self, const char *table,
                            const char *column, json_t key) {

  char named[256];
  const char *name = key.name;
  const char *referenced = key.table;
  json_t mapping = key.mapping;

  if (referenced[0] == '\0')
    return dbmFail(self, TEXT`the foreign key on ${table}.${column} does not say which table it references`);

  if (name[0] == '\0') {
    dbmWrite(named, sizeof named, TEXT`${table}_${column}_fkey`);
    name = named;
  }

  /* `mapping: "id"` is the column there that this one points at */
  if (strcmp(mapping.kind(), "string") == 0) {

    const char *local[] = {column};
    const char *foreign[] = {mapping.text()};

    return foreignKey(self, table, referenced, name, local, foreign, 1,
                      key.rules);
  }

  return self->addForeignKey(self, table, referenced, name, mapping,
                             key.rules);
}

/**
 * The same foreign key, said inside the column: `REFERENCES "owners" ("id")
 * ON DELETE CASCADE`. For a database that cannot add one afterwards, which is
 * SQLite - node db-migrate's driver for it dropped the key without a word,
 * and a schema without the key it was written with is a different schema.
 */
int dbmBaseReferences(driver_t *self, dbm_text_t *out, const char *table,
                      const char *column, json_t key) {

  const char *referenced = key.table;
  json_t mapping = key.mapping;
  const char *onDelete = key.rules.onDelete;
  const char *onUpdate = key.rules.onUpdate;
  const char *foreign = NULL;

  if (referenced[0] == '\0')
    return dbmFail(self, TEXT`the foreign key on ${table}.${column} does not say which table it references`);

  if (strcmp(mapping.kind(), "string") == 0)
    foreign = mapping.text();
  else if (strcmp(mapping.kind(), "object") == 0 && mapping.count() == 1)
    foreign = mapping.get(mapping.keyAt(0)).text();
  else
    return dbmFail(self, TEXT`the foreign key on ${table}.${column} maps more than this one column, which only a table-level key can say`);

  out->put(" REFERENCES ");
  self->quoteName(self, out, referenced);
  out->put(" (");
  self->quoteName(self, out, foreign);
  out->put(")");

  if (onDelete[0] != '\0') {

    if (!isRule(onDelete))
      return dbmFail(self, TEXT`\`${onDelete}\` is not something a foreign key can do on delete`);

    out->put(" ON DELETE ");
    out->put(onDelete);
  }

  if (onUpdate[0] != '\0') {

    if (!isRule(onUpdate))
      return dbmFail(self, TEXT`\`${onUpdate}\` is not something a foreign key can do on update`);

    out->put(" ON UPDATE ");
    out->put(onUpdate);
  }

  return 0;
}

int dbmBaseTableOptions(driver_t *self, dbm_text_t *out, const char *table,
                        json_t spec) {
  (void)self;
  (void)out;
  (void)table;
  (void)spec;
  return 0;
}

int dbmBaseCreateTable(driver_t *self, const char *table, json_t spec) {

  dbm_text_t sql = {0};
  defer sql.release();

  const char *[] keys;
  defer keys.release();

  json_t columns = spec;
  bool ifNotExists = false;
  bool withOptions = false;

  /**
   * `createTable(name, {columns: {...}, ifNotExists: true})` or the columns
   * straight away - base told the two apart by whether there was a
   * `columns`, and so does this. A table with a column called `columns` has
   * to use the first form.
   */
  if (strcmp(spec.columns.kind(), "object") == 0) {
    columns = spec.columns;
    ifNotExists = spec.ifNotExists.truth();
    withOptions = true;
  }

  if (columns.count() == 0)
    return dbmFail(self, TEXT`table ${table} with no columns`);

  for (int i = 0; i < columns.count(); ++i) {

    const char *name = columns.keyAt(i);

    if (columns.get(name).primaryKey.truth())
      keys.push(name);
  }

  dbm_column_options_t options = {keys.count == 1};

  sql.put("CREATE TABLE ");

  if (ifNotExists)
    sql.put("IF NOT EXISTS ");

  self->quoteName(self, &sql, table);
  sql.put(" (");

  for (int i = 0; i < columns.count(); ++i) {

    const char *name = columns.keyAt(i);

    if (i > 0)
      sql.put(", ");

    if (self->columnDef(self, &sql, table, name, columns.get(name), &options))
      return -1;
  }

  /* more than one key column is a key of its own, after the columns */
  if (keys.count > 1) {
    sql.put(", PRIMARY KEY (");
    putNames(self, &sql, keys.items, keys.count);
    sql.put(")");
  }

  sql.put(")");

  if (withOptions && self->tableOptions(self, &sql, table, spec))
    return -1;

  if (dbmSend(self, &sql))
    return -1;

  /* said inside the columns already, where the database wanted them */
  if (self->inlineForeignKeys)
    return 0;

  for (int i = 0; i < columns.count(); ++i) {

    const char *name = columns.keyAt(i);
    json_t key = columns.get(name).foreignKey;

    if (strcmp(key.kind(), "object") == 0 &&
        columnForeignKey(self, table, name, key))
      return -1;
  }

  return 0;
}

int dbmBaseDropTable(driver_t *self, const char *table, bool ifExists) {

  dbm_text_t sql = {0};
  defer sql.release();

  sql.put(ifExists ? "DROP TABLE IF EXISTS " : "DROP TABLE ");
  self->quoteName(self, &sql, table);

  return dbmSend(self, &sql);
}

int dbmBaseRenameTable(driver_t *self, const char *from, const char *to) {

  dbm_text_t sql = {0};
  defer sql.release();

  sql.put("ALTER TABLE ");
  self->quoteName(self, &sql, from);
  sql.put(" RENAME TO ");
  self->quoteName(self, &sql, to);

  return dbmSend(self, &sql);
}

int dbmBaseAddColumn(driver_t *self, const char *table, const char *column,
                     json_t spec) {

  dbm_text_t sql = {0};
  defer sql.release();

  dbm_column_options_t options = {spec.primaryKey.truth()};

  sql.put("ALTER TABLE ");
  self->quoteName(self, &sql, table);
  sql.put(" ADD COLUMN ");

  if (self->columnDef(self, &sql, table, column, spec, &options) ||
      dbmSend(self, &sql))
    return -1;

  if (!self->inlineForeignKeys &&
      strcmp(spec.foreignKey.kind(), "object") == 0)
    return columnForeignKey(self, table, column, spec.foreignKey);

  return 0;
}

int dbmBaseRemoveColumn(driver_t *self, const char *table, const char *column) {

  dbm_text_t sql = {0};
  defer sql.release();

  sql.put("ALTER TABLE ");
  self->quoteName(self, &sql, table);
  sql.put(" DROP COLUMN ");
  self->quoteName(self, &sql, column);

  return dbmSend(self, &sql);
}

int dbmBaseRenameColumn(driver_t *self, const char *table, const char *from,
                        const char *to) {

  dbm_text_t sql = {0};
  defer sql.release();

  sql.put("ALTER TABLE ");
  self->quoteName(self, &sql, table);
  sql.put(" RENAME COLUMN ");
  self->quoteName(self, &sql, from);
  sql.put(" TO ");
  self->quoteName(self, &sql, to);

  return dbmSend(self, &sql);
}

int dbmBaseAddIndex(driver_t *self, const char *table, const char *name,
                    json_t columns, bool unique) {

  dbm_text_t sql = {0};
  defer sql.release();

  const char *[] names;
  defer names.release();

  /* `"name"` and `["name"]` are the same index */
  if (strcmp(columns.kind(), "string") == 0)
    names.push(columns.text());
  else
    for (int i = 0; i < columns.count(); ++i)
      names.push(columns[i].text());

  if (names.count == 0)
    return dbmFail(self, TEXT`index ${name} on ${table} with no columns`);

  sql.put(unique ? "CREATE UNIQUE INDEX " : "CREATE INDEX ");
  self->quoteName(self, &sql, name);
  sql.put(" ON ");
  self->quoteName(self, &sql, table);
  sql.put(" (");
  putNames(self, &sql, names.items, names.count);
  sql.put(")");

  return dbmSend(self, &sql);
}

int dbmBaseRemoveIndex(driver_t *self, const char *table, const char *name) {

  dbm_text_t sql = {0};
  defer sql.release();

  (void)table;

  sql.put("DROP INDEX ");
  self->quoteName(self, &sql, name);

  return dbmSend(self, &sql);
}

int dbmBaseRemoveForeignKey(driver_t *self, const char *table,
                            const char *name) {

  dbm_text_t sql = {0};
  defer sql.release();

  if (self->inlineForeignKeys)
    return dbmFail(self, TEXT`${self->name} cannot remove foreign key ${name} from a table that exists`);

  sql.put("ALTER TABLE ");
  self->quoteName(self, &sql, table);
  sql.put(" DROP CONSTRAINT ");
  self->quoteName(self, &sql, name);

  return dbmSend(self, &sql);
}

/** Text made for a parameter, held until the statement has been sent. */
typedef char *[] texts_t;

/** A value from a document as a parameter. Objects and arrays go as their text. */
static sql_value_t parameterOf(json_t value, texts_t *texts) {

  const char *kind = value.kind();

  if (strcmp(kind, "string") == 0)
    return sqlText(value.text());

  if (strcmp(kind, "bool") == 0)
    return sqlTruth(value.truth());

  if (strcmp(kind, "number") == 0)
    return yyjson_is_real(value.node) ? sqlReal(value.real())
                                      : sqlNumber(value.number());

  if (strcmp(kind, "object") == 0 || strcmp(kind, "array") == 0) {

    char *text = yyjson_val_write(value.node, 0, NULL);

    texts->push(text);
    return sqlText(text);
  }

  return sqlNothing();
}

/**
 * `INSERT INTO t (a, b) VALUES ($1, $2)`, with the values as parameters.
 *
 * The statement is built the way the SQL literal builds one - chunks around
 * holes - because the names are known only now and a literal is written
 * before the program runs. The values still never reach the text.
 */
int dbmBaseInsert(driver_t *self, const char *table, json_t row) {

  dbm_text_t head = {0};
  defer head.release();

  const char *[] chunks;
  sql_value_t[] values;
  texts_t texts;

  defer chunks.release();
  defer values.release();
  defer texts.release();

  int count = row.count();

  if (strcmp(row.kind(), "object") != 0 || count == 0)
    return dbmFail(self, TEXT`an insert into ${table} that is not an object of columns`);

  head.put("INSERT INTO ");
  self->quoteName(self, &head, table);
  head.put(" (");

  for (int i = 0; i < count; ++i) {

    if (i > 0)
      head.put(", ");

    self->quoteName(self, &head, row.keyAt(i));
  }

  head.put(") VALUES (");

  if (head.failed)
    return dbmFail(self, TEXT`out of memory writing an insert`);

  chunks.push(head.text);

  for (int i = 0; i < count; ++i) {
    values.push(parameterOf(row.get(row.keyAt(i)), &texts));
    chunks.push(i + 1 < count ? ", " : ")");
  }

  sql_t query = SQL(chunks.items, chunks.count, values.items, values.count);
  int answer = dbmQuery(self, &query, NULL);

  query.release();

  for (text in texts)
    free(*text);

  return answer;
}

/* ------------------------------------------------------------------ */
/* databases                                                          */
/* ------------------------------------------------------------------ */

int dbmBaseCreateDatabase(driver_t *self, const char *name, bool ifNotExists) {

  dbm_text_t sql = {0};
  defer sql.release();

  sql.put(ifNotExists ? "CREATE DATABASE IF NOT EXISTS " : "CREATE DATABASE ");
  self->quoteName(self, &sql, name);

  return dbmSend(self, &sql);
}

int dbmBaseDropDatabase(driver_t *self, const char *name, bool ifExists) {

  dbm_text_t sql = {0};
  defer sql.release();

  sql.put(ifExists ? "DROP DATABASE IF EXISTS " : "DROP DATABASE ");
  self->quoteName(self, &sql, name);

  return dbmSend(self, &sql);
}

/* ------------------------------------------------------------------ */
/* bookkeeping                                                        */
/* ------------------------------------------------------------------ */

/**
 * The table node db-migrate keeps, column for column, so a database migrated
 * by one can be carried on by the other.
 */
int dbmBaseCreateMigrationsTable(driver_t *self) {

  return self->createTable(self, self->migrationTable, {
    columns: {
      id: {type: "int", notNull: true, primaryKey: true, autoIncrement: true},
      name: {type: "string", length: 255, notNull: true},
      run_on: {type: "datetime", notNull: true},
    },
    ifNotExists: true,
  });
}

/** The table name as a piece of SQL a literal can splice in. */
static sql_t tableName(driver_t *self) {

  dbm_text_t quoted = {0};

  self->quoteName(self, &quoted, self->migrationTable);

  sql_t raw = sqlRaw(quoted.failed ? "" : quoted.text);

  quoted.release();
  return raw;
}

int dbmBaseLoadedMigrations(driver_t *self, json_t *names) {

  sql_t table = tableName(self);
  defer table.release();

  sql_t query = SQL`SELECT name FROM ${&table} ORDER BY run_on ASC, id ASC`;
  defer query.release();

  /**
   * Read in a dry run too - what would run depends on what has - unless the
   * table is not there yet: then the dry run printed its CREATE instead of
   * sending it, and nothing has run.
   */
  int answer = self->query(self, &query, names);

  if (answer != 0 && self->dryRun) {
    self->error[0] = '\0';
    *names = meta_toJSON("[]");
    return 0;
  }

  return answer;
}

int dbmBaseAddMigrationRecord(driver_t *self, const char *name) {

  char stamp[32];
  time_t now = time(NULL);
  struct tm utc;

  gmtime_r(&now, &utc);
  strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &utc);

  sql_t table = tableName(self);
  defer table.release();

  sql_t query =
      SQL`INSERT INTO ${&table} (name, run_on) VALUES (${name}, ${stamp})`;
  defer query.release();

  return dbmQuery(self, &query, NULL);
}

int dbmBaseDeleteMigrationRecord(driver_t *self, const char *name) {

  sql_t table = tableName(self);
  defer table.release();

  sql_t query = SQL`DELETE FROM ${&table} WHERE name = ${name}`;
  defer query.release();

  return dbmQuery(self, &query, NULL);
}

/* ------------------------------------------------------------------ */
/* a driver, filled with the generic versions                         */
/* ------------------------------------------------------------------ */

driver_t *dbmDriverNew(const char *name, size_t size) {

  driver_t *self = calloc(1, size < sizeof(driver_t) ? sizeof(driver_t) : size);

  if (self == NULL)
    return NULL;

  self->name = name;
  self->migrationTable = "migrations";

  self->runSql = missingRunSql;
  self->query = missingQuery;
  self->close = missingClose;

  self->startMigration = dbmBaseStartMigration;
  self->endMigration = dbmBaseEndMigration;
  self->abortMigration = dbmBaseAbortMigration;

  self->mapDataType = dbmBaseMapDataType;
  self->quoteName = dbmBaseQuoteName;
  self->quoteText = dbmBaseQuoteText;
  self->columnDef = dbmBaseColumnDef;
  self->columnConstraint = dbmBaseColumnConstraint;
  self->valueLiteral = dbmBaseValueLiteral;
  self->tableOptions = dbmBaseTableOptions;

  self->createTable = dbmBaseCreateTable;
  self->dropTable = dbmBaseDropTable;
  self->renameTable = dbmBaseRenameTable;
  self->addColumn = dbmBaseAddColumn;
  self->removeColumn = dbmBaseRemoveColumn;
  self->renameColumn = dbmBaseRenameColumn;
  self->changeColumn = missingChangeColumn;
  self->addIndex = dbmBaseAddIndex;
  self->removeIndex = dbmBaseRemoveIndex;
  self->addForeignKey = dbmBaseAddForeignKey;
  self->removeForeignKey = dbmBaseRemoveForeignKey;
  self->insert = dbmBaseInsert;
  self->createDatabase = dbmBaseCreateDatabase;
  self->dropDatabase = dbmBaseDropDatabase;

  self->createMigrationsTable = dbmBaseCreateMigrationsTable;
  self->loadedMigrations = dbmBaseLoadedMigrations;
  self->addMigrationRecord = dbmBaseAddMigrationRecord;
  self->deleteMigrationRecord = dbmBaseDeleteMigrationRecord;

  return self;
}
