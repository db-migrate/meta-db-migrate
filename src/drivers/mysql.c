/**
 * MySQL, and MariaDB, which speaks the same.
 *
 * Where it differs from the generic SQL:
 *
 * - identifiers are quoted with backticks, and a string constant escapes a
 *   backslash as well as a quote
 * - `text` and `blob` pick their size from the length asked for, a boolean
 *   is `TINYINT(1)`, a datetime is `DATETIME`, and a VARCHAR has to say how
 *   long - 255 when nobody did
 * - a column may say UNSIGNED, CHARACTER SET, COLLATE, ON UPDATE, AFTER and
 *   COMMENT, and a table ENGINE, ROW_FORMAT, COLLATE and CHARACTER SET
 * - `changeColumn` restates the whole column, which is what MySQL's CHANGE
 *   COLUMN takes, and an index and a foreign key are dropped by their own
 *   statements
 *
 * And one thing no driver can change: MySQL commits before every piece of
 * DDL, so a migration that fails halfway keeps the DDL it already ran. The
 * record of it is still rolled back, so the next `up` runs it again - a
 * migration meant for MySQL should be written so that is safe, which is the
 * advice node db-migrate's documentation gives as well.
 *
 * libmysqlclient blocks; there is no descriptor to park on. A migration is
 * the one place that does not matter.
 */
#include <db_migrate_driver.h>

#include <meta_mysql.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  driver_t base;
  MYSQL *conn;
} mysql_driver_t;

static MYSQL *connectionOf(driver_t *self) {
  return ((mysql_driver_t *)self)->conn;
}

/**
 * Text with any number of statements in it. Every result is read, because
 * the server answers one per statement and stops at the first that fails -
 * and that one is the answer.
 */
/** The server's words, and its error number and SQL state beside them. */
static int failed(driver_t *self, MYSQL *db) {

  char number[16];

  dbmWrite(number, sizeof number, TEXT`${(long)mysql_errno(db)}`);

  if (mysql_errno(db) != 0) {
    dbmFailedField(self, "errno", number);
    dbmFailedField(self, "sqlState", mysql_sqlstate(db));
  }

  return dbmFail(self, TEXT`${mysql_error(db)}`);
}

static int myRunSql(driver_t *self, const char *text) {

  MYSQL *db = connectionOf(self);

  if (mysql_real_query(db, text, (unsigned long)strlen(text)) != 0)
    return failed(self, db);

  for (;;) {

    MYSQL_RES *result = mysql_store_result(db);

    if (result != NULL)
      mysql_free_result(result);
    else if (mysql_field_count(db) != 0)
      return failed(self, db);

    int more = mysql_next_result(db);

    if (more > 0)
      return failed(self, db);

    if (more < 0)
      return 0;
  }
}

/**
 * One column's value as JSON, by what the server says the column is.
 * `TINYINT(1)` is MySQL's boolean, so it comes back as one; a DECIMAL stays
 * text, because a decimal that went through a double is a different number.
 */
static yyjson_mut_val *valueOf(yyjson_mut_doc *doc, const MYSQL_FIELD *field,
                               const char *text, bool null) {

  if (null)
    return yyjson_mut_null(doc);

  switch (field->type) {

  case MYSQL_TYPE_TINY:

    if (field->length == 1)
      return yyjson_mut_bool(doc, text[0] != '0');

    return yyjson_mut_sint(doc, strtoll(text, NULL, 10));

  case MYSQL_TYPE_SHORT:
  case MYSQL_TYPE_LONG:
  case MYSQL_TYPE_INT24:
  case MYSQL_TYPE_LONGLONG:
  case MYSQL_TYPE_YEAR:
    return (field->flags & UNSIGNED_FLAG)
               ? yyjson_mut_uint(doc, strtoull(text, NULL, 10))
               : yyjson_mut_sint(doc, strtoll(text, NULL, 10));

  case MYSQL_TYPE_FLOAT:
  case MYSQL_TYPE_DOUBLE:
    return yyjson_mut_real(doc, strtod(text, NULL));

  default:
    return yyjson_mut_strcpy(doc, text);
  }
}

/**
 * The rows of an executed statement, every column fetched as text into a
 * buffer as long as the longest value - which the server only says once
 * asked to measure, before the rows are stored.
 */
static int rowsOf(driver_t *self, MYSQL_STMT *statement, json_t *rows) {

  MYSQL_RES *meta = mysql_stmt_result_metadata(statement);
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *array = yyjson_mut_arr(doc);
  int answer = 0;

  if (meta != NULL) {

    bool measure = true;

    mysql_stmt_attr_set(statement, STMT_ATTR_UPDATE_MAX_LENGTH, &measure);

    if (mysql_stmt_store_result(statement) != 0) {
      answer = dbmFail(self, TEXT`${mysql_stmt_error(statement)}`);
    } else {

      unsigned int count = mysql_num_fields(meta);
      MYSQL_FIELD *fields = mysql_fetch_fields(meta);
      MYSQL_BIND *binds = calloc(count, sizeof(MYSQL_BIND));
      char **buffers = calloc(count, sizeof(char *));
      unsigned long *lengths = calloc(count, sizeof(unsigned long));
      char *nulls = calloc(count, 1);

      for (unsigned int i = 0; i < count; ++i) {
        buffers[i] = calloc(1, fields[i].max_length + 1);
        binds[i].buffer_type = MYSQL_TYPE_STRING;
        binds[i].buffer = buffers[i];
        binds[i].buffer_length = fields[i].max_length + 1;
        binds[i].length = &lengths[i];
        binds[i].is_null = (void *)&nulls[i];
      }

      if (mysql_stmt_bind_result(statement, binds) != 0)
        answer = dbmFail(self, TEXT`${mysql_stmt_error(statement)}`);

      while (answer == 0) {

        int fetched = mysql_stmt_fetch(statement);

        if (fetched == MYSQL_NO_DATA)
          break;

        if (fetched == 1) {
          answer = dbmFail(self, TEXT`${mysql_stmt_error(statement)}`);
          break;
        }

        yyjson_mut_val *row = yyjson_mut_obj(doc);

        for (unsigned int i = 0; i < count; ++i) {
          buffers[i][lengths[i] < fields[i].max_length + 1
                         ? lengths[i]
                         : fields[i].max_length] = '\0';
          yyjson_mut_obj_add(row, yyjson_mut_strcpy(doc, fields[i].name),
                             valueOf(doc, &fields[i], buffers[i], nulls[i]));
        }

        yyjson_mut_arr_append(array, row);
      }

      for (unsigned int i = 0; i < count; ++i)
        free(buffers[i]);

      free(buffers);
      free(binds);
      free(lengths);
      free(nulls);
    }

    mysql_free_result(meta);
  }

  yyjson_mut_doc_set_root(doc, array);

  if (answer != 0) {
    yyjson_mut_doc_free(doc);
    return answer;
  }

  *rows = meta_jsonFromMut(doc);
  return 0;
}

/**
 * Prepared, bound by kind, executed - meta_mysql.h's binding, with the
 * statement kept open long enough to say why when it fails.
 */
static int myQuery(driver_t *self, const sql_t *query, json_t *rows) {

  MYSQL *db = connectionOf(self);
  MYSQL_BIND binds[META_MYSQL_PARAMS];
  unsigned long lengths[META_MYSQL_PARAMS];
  char nulls[META_MYSQL_PARAMS];
  MYSQL_STMT *statement;
  int answer = 0;

  if (query->count > META_MYSQL_PARAMS)
    return dbmFail(self, TEXT`${query->count} parameters, and this takes ${META_MYSQL_PARAMS}`);

  statement = mysql_stmt_init(db);

  if (statement == NULL)
    return dbmFail(self, TEXT`${mysql_error(db)}`);

  if (mysql_stmt_prepare(statement, query->sql,
                         (unsigned long)strlen(query->sql)) != 0) {
    answer = dbmFail(self, TEXT`${mysql_stmt_error(statement)}`);
  } else {

    for (size_t at = 0; at < query->count; ++at)
      metaMysqlBind(&binds[at], &query->typed[at], &lengths[at], &nulls[at]);

    if (query->count > 0 && mysql_stmt_bind_param(statement, binds) != 0)
      answer = dbmFail(self, TEXT`${mysql_stmt_error(statement)}`);
    else if (mysql_stmt_execute(statement) != 0)
      answer = dbmFail(self, TEXT`${mysql_stmt_error(statement)}`);
    else if (rows != NULL)
      answer = rowsOf(self, statement, rows);
  }

  mysql_stmt_close(statement);
  return answer;
}

static void myClose(driver_t *self) {

  if (connectionOf(self) != NULL)
    mysql_close(connectionOf(self));
}

/* ------------------------------------------------------------------ */
/* where MySQL says it differently                                    */
/* ------------------------------------------------------------------ */

static void myQuoteName(driver_t *self, dbm_text_t *out, const char *name) {

  (void)self;
  out->put("`");

  for (const char *at = name; *at != '\0'; ++at)
    out->put(*at == '`' ? "``" : (char[]){*at, 0});

  out->put("`");
}

/** A backslash is an escape in a MySQL string unless the server is told not. */
static void myQuoteText(driver_t *self, dbm_text_t *out, const char *text) {

  (void)self;
  out->put("'");

  for (const char *at = text; *at != '\0'; ++at) {

    if (*at == '\'')
      out->put("''");
    else if (*at == '\\')
      out->put("\\\\");
    else
      out->putn(at, 1);
  }

  out->put("'");
}

static const char *myMapDataType(driver_t *self, const char *type) {

  if (type in {"datetime", "DATETIME"})
    return "DATETIME";

  if (type in {"boolean", "BOOLEAN"})
    return "TINYINT(1)";

  if (type in {"json", "JSON"})
    return "JSON";

  return dbmBaseMapDataType(self, type);
}

/** `text` and `blob` by size, the way node's driver picked them. */
static const char *sized(const char *type, long length) {

  bool blob = type in {"blob", "BLOB"};

  if (length <= 0)
    length = 1000;

  if (length > 16777216)
    return blob ? "LONGBLOB" : "LONGTEXT";

  if (length > 65536)
    return blob ? "MEDIUMBLOB" : "MEDIUMTEXT";

  if (length > 256)
    return blob ? "BLOB" : "TEXT";

  return blob ? "TINYBLOB" : "TINYTEXT";
}

static int myColumnConstraint(driver_t *self, dbm_text_t *out,
                              const char *table, const char *column,
                              json_t spec,
                              const dbm_column_options_t *options) {

  const char *charset = spec.characterSet;
  const char *collation = spec.collation;
  const char *onUpdate = spec.onUpdate;
  const char *after = spec.after;
  const char *comment = spec.comment;

  (void)table;
  (void)column;

  /* `unsigned` is a word of C, so it is asked for by name */
  if (spec.get("unsigned").truth())
    out->put(" UNSIGNED");

  if (charset[0] != '\0') {
    out->put(" CHARACTER SET ");
    myQuoteName(self, out, charset);
  }

  if (collation[0] != '\0') {
    out->put(" COLLATE ");
    myQuoteName(self, out, collation);
  }

  if (spec.primaryKey.truth() && options->soleKey)
    out->put(" PRIMARY KEY");

  if (spec.autoIncrement.truth())
    out->put(" AUTO_INCREMENT");

  if (spec.notNull.truth())
    out->put(" NOT NULL");
  else if (strcmp(spec.notNull.kind(), "bool") == 0)
    out->put(" NULL");

  if (spec.unique.truth())
    out->put(" UNIQUE");

  if (strncmp(onUpdate, "CURRENT_TIMESTAMP", 17) == 0 &&
      strspn(onUpdate + 17, "()0123456789") == strlen(onUpdate + 17)) {
    out->put(" ON UPDATE ");
    out->put(onUpdate);
  }

  if ((!spec.defaultValue.isNothing() ||
       strcmp(spec.defaultValue.kind(), "null") == 0) &&
      dbmBaseDefault(self, out, spec.defaultValue))
    return -1;

  if (after[0] != '\0') {
    out->put(" AFTER ");
    myQuoteName(self, out, after);
  }

  if (comment[0] != '\0') {
    out->put(" COMMENT ");
    myQuoteText(self, out, comment);
  }

  return 0;
}

static int myColumnDef(driver_t *self, dbm_text_t *out, const char *table,
                       const char *column, json_t spec,
                       const dbm_column_options_t *options) {

  const char *type = dbmColumnType(spec);
  long length = spec.length;

  self->quoteName(self, out, column);
  out->put(" ");

  if (type in {"text", "TEXT", "blob", "BLOB"}) {
    out->put(sized(type, length));
  } else if (type in {"decimal", "DECIMAL"}) {

    long precision = spec.precision;
    long scale = spec.scale;

    out->put("DECIMAL");

    if (precision > 0)
      out->append(TEXT`(${precision},${scale})`);
  } else {

    const char *mapped = self->mapDataType(self, type);

    out->put(mapped != NULL ? mapped : type);

    if (length > 0)
      out->append(TEXT`(${length})`);
    else if (mapped != NULL && strcmp(mapped, "VARCHAR") == 0) {
      out->put("(255)");
    }
  }

  return self->columnConstraint(self, out, table, column, spec, options);
}

static int myTableOptions(driver_t *self, dbm_text_t *out, const char *table,
                          json_t spec) {

  const char *engine = spec.engine;
  const char *rowFormat = spec.rowFormat;
  const char *collate = spec.collate;
  const char *charset = spec.charset;

  (void)table;

  const char *words[] = {engine, rowFormat, collate, charset};

  /* words, not expressions: anything else is not an option of a table */
  for (size_t i = 0; i < countof(words); ++i)
    if (strspn(words[i], "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
                         "0123456789_") != strlen(words[i]))
      return dbmFail(self, TEXT`\`${words[i]}\` is not a table option MySQL has`);

  if (engine[0] != '\0') {
    out->put(" ENGINE ");
    out->put(engine);
  }

  if (rowFormat[0] != '\0') {
    out->put(" ROW_FORMAT ");
    out->put(rowFormat);
  }

  if (collate[0] != '\0') {
    out->put(" COLLATE ");
    out->put(collate);
  }

  if (charset[0] != '\0') {
    out->put(" CHARACTER SET ");
    out->put(charset);
  }

  return 0;
}

/** CHANGE COLUMN restates the column whole, under the same name. */
static int myChangeColumn(driver_t *self, const char *table,
                          const char *column, json_t spec) {

  dbm_text_t sql = {0};
  defer sql.release();

  dbm_column_options_t options = {false};

  if (dbmColumnType(spec)[0] == '\0')
    return dbmFail(self, TEXT`changeColumn on MySQL restates the whole column, so ${table}.${column} needs its \`type\``);

  sql.put("ALTER TABLE ");
  self->quoteName(self, &sql, table);
  sql.put(" CHANGE COLUMN ");
  self->quoteName(self, &sql, column);
  sql.put(" ");

  if (self->columnDef(self, &sql, table, column, spec, &options))
    return -1;

  return dbmSend(self, &sql);
}

static int myRemoveIndex(driver_t *self, const char *table, const char *name) {

  dbm_text_t sql = {0};
  defer sql.release();

  sql.put("DROP INDEX ");
  self->quoteName(self, &sql, name);
  sql.put(" ON ");
  self->quoteName(self, &sql, table);

  return dbmSend(self, &sql);
}

static int myRemoveForeignKey(driver_t *self, const char *table,
                              const char *name) {

  dbm_text_t sql = {0};
  defer sql.release();

  sql.put("ALTER TABLE ");
  self->quoteName(self, &sql, table);
  sql.put(" DROP FOREIGN KEY ");
  self->quoteName(self, &sql, name);

  return dbmSend(self, &sql);
}

/** START TRANSACTION, which is what MySQL calls BEGIN in every mode. */
static int myStartMigration(driver_t *self) {

  dbm_text_t sql = {0};
  defer sql.release();

  sql.put("START TRANSACTION");
  return dbmSend(self, &sql);
}

/* ------------------------------------------------------------------ */
/* opening one                                                        */
/* ------------------------------------------------------------------ */

/**
 * From a database.json entry: host, port, user, password, database, or
 * `socketPath` for a local socket. Several statements per call are allowed,
 * because a `runSql` with two in it is ordinary and the server reports on
 * each.
 */
static driver_t *myOpen(json_t config, char *why, size_t room) {

  const char *host = config.host;
  const char *user = config.user;
  const char *password = config.password;
  const char *database = config.database;
  const char *socket = config.socketPath;
  long port = config.port;

  MYSQL *db = mysql_init(NULL);

  if (db == NULL) {
    dbmWrite(why, room, TEXT`out of memory`);
    return NULL;
  }

  if (mysql_real_connect(db, host[0] != '\0' ? host : NULL, user, password,
                         database[0] != '\0' ? database : NULL,
                         (unsigned int)port,
                         socket[0] != '\0' ? socket : NULL,
                         CLIENT_MULTI_STATEMENTS) == NULL) {
    dbmWrite(why, room, TEXT`${mysql_error(db)}`);
    mysql_close(db);
    return NULL;
  }

  mysql_set_character_set(db, "utf8mb4");

  mysql_driver_t *driver =
      (mysql_driver_t *)dbmDriverNew("mysql", sizeof(mysql_driver_t));

  if (driver == NULL) {
    mysql_close(db);
    dbmWrite(why, room, TEXT`out of memory`);
    return NULL;
  }

  driver->conn = db;

  driver_t *self = &driver->base;

  self->runSql = myRunSql;
  self->query = myQuery;
  self->close = myClose;

  self->startMigration = myStartMigration;
  self->quoteName = myQuoteName;
  self->quoteText = myQuoteText;
  self->mapDataType = myMapDataType;
  self->columnDef = myColumnDef;
  self->columnConstraint = myColumnConstraint;
  self->tableOptions = myTableOptions;
  self->changeColumn = myChangeColumn;
  self->removeIndex = myRemoveIndex;
  self->removeForeignKey = myRemoveForeignKey;

  return self;
}

__attribute__((constructor)) static void registerMysql(void) {
  dbmRegisterDriver("mysql", myOpen);
  dbmRegisterDriver("mariadb", myOpen);
}
