/**
 * The key-value table node db-migrate keeps its state in.
 *
 *   key      VARCHAR, the primary key
 *   value    TEXT, a JSON document
 *   run_on   DATETIME, set by the database's clock
 *
 * The same table, the same columns and the same rows node writes, so a
 * database whose state node kept can be carried on here and the other way
 * round. `run_on` comes from CURRENT_TIMESTAMP rather than from this process,
 * because the processes that share a lock do not share a clock.
 *
 * Every name is quoted by the driver - `key` is a word MySQL keeps for
 * itself - and spliced into the SQL literal as a nested raw piece; the values
 * stay parameters.
 */
#include <db_migrate_driver.h>

#include <stdlib.h>
#include <string.h>

/**
 * One statement on the connection, alone on it while it runs - the state's
 * connection is shared with the lock's heartbeat.
 */
static int asked(driver_t *self, const sql_t *query, json_t *rows) {

  if (self->serial != NULL)
    pthread_mutex_lock(self->serial);

  int answer = self->query(self, query, rows);

  if (self->serial != NULL)
    pthread_mutex_unlock(self->serial);

  return answer;
}

/** A name, quoted the way this driver quotes them, as a piece of SQL. */
static sql_t quoted(driver_t *self, const char *name) {

  dbm_text_t text = {0};

  self->quoteName(self, &text, name);

  sql_t raw = sqlRaw(text.failed || text.text == NULL ? "" : text.text);

  text.release();
  return raw;
}

int dbmKvCreate(driver_t *self, const char *table) {

  return self->createTable(self, table, {
    columns: {
      key: {type: "string", notNull: true, primaryKey: true, unique: true},
      value: {type: "text", notNull: true},
      run_on: {type: "datetime", notNull: true},
    },
    ifNotExists: true,
  });
}

/**
 * The value under a key, which the caller frees, or NULL when there is no
 * such row - which is not a failure. Answers -1 only when asking failed.
 */
int dbmKvGet(driver_t *self, const char *table, const char *key,
             char **value) {

  sql_t t = quoted(self, table);
  sql_t k = quoted(self, "key");
  sql_t v = quoted(self, "value");
  defer t.release();
  defer k.release();
  defer v.release();

  sql_t query = SQL`SELECT ${&v} FROM ${&t} WHERE ${&k} = ${key}`;
  defer query.release();

  json_t rows = {0};

  *value = NULL;

  if (asked(self, &query, &rows))
    return -1;

  if (rows.count() > 0) {
    const char *found = rows[0].value;
    *value = strdup(found);
  }

  rows.release();
  return 0;
}

int dbmKvInsert(driver_t *self, const char *table, const char *key,
                const char *value) {

  sql_t t = quoted(self, table);
  sql_t k = quoted(self, "key");
  sql_t v = quoted(self, "value");
  sql_t r = quoted(self, "run_on");
  defer t.release();
  defer k.release();
  defer v.release();
  defer r.release();

  sql_t query = SQL`INSERT INTO ${&t} (${&k}, ${&v}, ${&r})
                    VALUES (${key}, ${value}, CURRENT_TIMESTAMP)`;
  defer query.release();

  return asked(self, &query, NULL);
}

int dbmKvUpdate(driver_t *self, const char *table, const char *key,
                const char *value) {

  sql_t t = quoted(self, table);
  sql_t k = quoted(self, "key");
  sql_t v = quoted(self, "value");
  sql_t r = quoted(self, "run_on");
  defer t.release();
  defer k.release();
  defer v.release();
  defer r.release();

  sql_t query = SQL`UPDATE ${&t} SET ${&v} = ${value},
                    ${&r} = CURRENT_TIMESTAMP WHERE ${&k} = ${key}`;
  defer query.release();

  return asked(self, &query, NULL);
}

/**
 * The update that only happens when the row still holds `expected` - the
 * compare and swap the migration lock is built on. Whether it happened is
 * the caller's to find out by reading the row again, as node does: not every
 * driver says how many rows an update touched.
 */
int dbmKvSwap(driver_t *self, const char *table, const char *key,
              const char *value, const char *expected) {

  sql_t t = quoted(self, table);
  sql_t k = quoted(self, "key");
  sql_t v = quoted(self, "value");
  sql_t r = quoted(self, "run_on");
  defer t.release();
  defer k.release();
  defer v.release();
  defer r.release();

  sql_t query = SQL`UPDATE ${&t} SET ${&v} = ${value},
                    ${&r} = CURRENT_TIMESTAMP
                    WHERE ${&k} = ${key} AND ${&v} = ${expected}`;
  defer query.release();

  return asked(self, &query, NULL);
}

int dbmKvDelete(driver_t *self, const char *table, const char *key) {

  sql_t t = quoted(self, table);
  sql_t k = quoted(self, "key");
  defer t.release();
  defer k.release();

  sql_t query = SQL`DELETE FROM ${&t} WHERE ${&k} = ${key}`;
  defer query.release();

  return asked(self, &query, NULL);
}
