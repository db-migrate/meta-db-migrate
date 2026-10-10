/**
 * dml migrations: node db-migrate's lib/dml.js, since 1.4.0.
 *
 * A dml migration changes data instead of the schema. Every instruction is a
 * step, recorded with what reverts it in the migration's record - `{i, c, f,
 * s}`, as a v2 migration's, with `i`, `c` and `f` empty - before it runs:
 *
 *   {"t":4,"a":"insert","c":["pets","<migration>#1"],"n":1}
 *   {"t":4,"a":"update","c":["pets","__dbm_backup_<16 hex>",["id"],["kind"]],
 *    "n":2,"b":1,"o":300}
 *   {"t":4,"a":"delete","c":["pets","__dbm_backup_<16 hex>",["id"]],"n":3}
 *   {"t":4,"a":"soft","c":["pets","deleted_at",["id"],"|del:<migration>#4"]}
 *   {"t":4,"a":"runSql","c":["<revert sql>",[params]],"n":5}
 *   {"t":3,"a":"purge","c":["pets"],"n":6}           can not be reverted
 *
 * update and delete copy the rows into a backup table first, and change them
 * in batches by their key; `b` says the backup is complete (1) or restored
 * (2), `o` how many of its rows were changed - an interrupted run continues
 * after them. The backups are registered in `__dbmigrate_backups__`, the soft
 * deletes to purge with a later release in `__dbmigrate_purges__`, both
 * written by compare and swap through the state's own connection.
 *
 * Where node has a bug the data would feel, this does what node meant, and
 * the records stay node's:
 *
 * - the mark of a soft delete `|del:m#1` is found as itself, not inside
 *   `|del:m#10` as well, when reverting it and forgetting its purge
 * - on MySQL, whose flag column holds 255 characters, a soft delete that
 *   would make a flag longer is refused instead of cut off
 * - --non-transactional turns the transactions off here too
 */
#include <db_migrate_driver.h>

#include <stdlib.h>
#include <string.h>

#define FLAG "__dbmigrate__flag"
#define BATCH 1000
#define BACKUPS "__dbmigrate_backups__"
#define PURGES "__dbmigrate_purges__"

struct dml_t {
  driver_t *driver;
  dbm_state_t *state;

  /** The key of its record, the migration's file name. */
  const char *name;
  yyjson_mut_doc *record;

  /** An interrupted run being resumed: what of it is done already. */
  const dbm_interrupted_t *recovery;

  bool dry;
  bool transactional;

  /** The step counted last - node's op - and the last one done. */
  int op;
  int done;

  /** The step being run, as the log says it: update("pets", {"kind":"dog"}). */
  char current[600];

  bool failed;
  char error[600];

  /** Run by a worker: its job, and whether it stopped, to go on later. */
  dbm_job_t *job;
  bool stopped;

  /** What a failure did was reverted. */
  bool rolledBack;
};

static int between(dml_t *self);

/* ------------------------------------------------------------------ */
/* failing                                                            */
/* ------------------------------------------------------------------ */

static int fail(dml_t *self, text_t why) {

  if (!self->failed) {
    self->failed = true;
    dbmWrite(self->error, sizeof self->error, why);
  }

  return -1;
}

static int failedDriver(dml_t *self) {
  return fail(self, TEXT`${self->driver->error}`);
}

static int failedState(dml_t *self) {
  return fail(self, TEXT`could not write the state: ${self->state->db->error}`);
}

int dml_t__fail(dml_t *self, text_t why) { return fail(self, why); }

bool dml_t__hasFailed(dml_t *self) { return self->failed; }

/* ------------------------------------------------------------------ */
/* JSON, the mutable kind                                             */
/* ------------------------------------------------------------------ */

static yyjson_mut_val *rootOf(yyjson_mut_doc *doc) {
  return yyjson_mut_doc_get_root(doc);
}

static const char *textOf(yyjson_mut_val *value) {
  return yyjson_mut_is_str(value) ? yyjson_mut_get_str(value) : "";
}

static bool isText(json_t value) {
  return value.node != NULL && strcmp(value.kind(), "string") == 0;
}

static bool isObject(json_t value) {
  return value.node != NULL && strcmp(value.kind(), "object") == 0;
}

static bool isArray(json_t value) {
  return value.node != NULL && strcmp(value.kind(), "array") == 0;
}

static bool isTrue(json_t options, const char *key) {
  json_t value = options.get(key);
  return value.node != NULL && strcmp(value.kind(), "bool") == 0 &&
         value.truth();
}

static yyjson_mut_val *copyIn(yyjson_mut_doc *doc, json_t value) {
  return value.node != NULL ? yyjson_mut_val_mut_copy(doc, value.node)
                            : yyjson_mut_null(doc);
}

/** A field of a step's entry set, where it is or added at the end - as JS. */
static void setNumber(yyjson_mut_doc *doc, yyjson_mut_val *entry,
                      const char *key, long value) {

  yyjson_mut_val *field = yyjson_mut_obj_get(entry, key);

  if (field != NULL)
    yyjson_mut_set_sint(field, value);
  else
    yyjson_mut_obj_add_int(doc, entry, key, value);
}

static long numberOf(yyjson_mut_val *entry, const char *key) {
  yyjson_mut_val *field = yyjson_mut_obj_get(entry, key);
  return field != NULL && yyjson_mut_is_int(field)
             ? (long)yyjson_mut_get_sint(field)
             : 0;
}

/* ------------------------------------------------------------------ */
/* statements                                                         */
/* ------------------------------------------------------------------ */

/** SQL with `?` for its parameters, and the parameters. */
typedef struct {
  dbm_text_t sql;
  yyjson_mut_doc *doc;
  yyjson_mut_val *params;
} statement_t;

static void opened(statement_t *s) {
  memset(s, 0, sizeof *s);
  s->doc = yyjson_mut_doc_new(NULL);
  s->params = yyjson_mut_arr(s->doc);
  yyjson_mut_doc_set_root(s->doc, s->params);
}

static void closed(statement_t *s) {
  s->sql.release();
  yyjson_mut_doc_free(s->doc);
}

static void quoted(dml_t *self, dbm_text_t *out, const char *name) {
  self->driver->quoteName(self->driver, out, name);
}

static void param(statement_t *s, json_t value) {
  yyjson_mut_arr_append(s->params, copyIn(s->doc, value));
}

static void paramText(statement_t *s, const char *text) {
  yyjson_mut_arr_add_strcpy(s->doc, s->params, text);
}

static void paramMut(statement_t *s, yyjson_mut_val *value) {
  yyjson_mut_arr_append(s->params, yyjson_mut_val_mut_copy(s->doc, value));
}

/** One statement after another: the text and the parameters of `from`. */
static void followedBy(statement_t *to, statement_t *from) {

  if (from->sql.text != NULL)
    to->sql.put(from->sql.text);

  for (size_t i = 0; i < yyjson_mut_arr_size(from->params); ++i)
    paramMut(to, yyjson_mut_arr_get(from->params, i));
}

/** Sent - or, `read`, also in a dry run - with the rows wanted or not. */
static int sent(dml_t *self, statement_t *s, json_t *rows, bool read) {

  if (s->sql.failed || s->sql.text == NULL)
    return fail(self, TEXT`out of memory writing a statement`);

  char *text = yyjson_mut_write(s->doc, 0, NULL);
  json_t params = meta_toJSON(text != NULL ? text : "[]");

  free(text);

  int answer = dbmQueryParams(self->driver, s->sql.text, params, rows, read);

  params.release();
  return answer != 0 ? failedDriver(self) : 0;
}

/** A statement without parameters. */
static int plain(dml_t *self, text_t text) {

  statement_t s;
  opened(&s);
  s.sql.append(text);

  int answer = sent(self, &s, NULL, false);

  closed(&s);
  return answer;
}

/* ------------------------------------------------------------------ */
/* transactions                                                       */
/* ------------------------------------------------------------------ */

static bool transacting(dml_t *self) {
  return self->transactional && !self->dry;
}

static int begun(dml_t *self) {
  return transacting(self) ? plain(self, TEXT`BEGIN`) : 0;
}

/**
 * COMMIT when `answer` says it worked, ROLLBACK when not - what the failure
 * said kept, the rollback's own failure only told with --verbose.
 */
static int ended(dml_t *self, int answer) {

  if (!transacting(self))
    return answer;

  if (answer == 0)
    return plain(self, TEXT`COMMIT`);

  driver_t *driver = self->driver;
  char *failedSql = driver->failedSql;
  long failedPosition = driver->failedPosition;
  char fields[768];
  char error[512];

  memcpy(fields, driver->failedFields, sizeof fields);
  memcpy(error, driver->error, sizeof error);
  driver->failedSql = NULL;

  dbm_text_t rollback = {0};
  rollback.put("ROLLBACK");

  if (dbmSend(driver, &rollback) != 0 && driver->verbose)
    dbmSay(stderr, TEXT`[dml] rollback failed ${driver->error}\n`);

  rollback.release();
  free(driver->failedSql);
  driver->failedSql = failedSql;
  driver->failedPosition = failedPosition;
  memcpy(driver->failedFields, fields, sizeof fields);
  memcpy(driver->error, error, sizeof error);

  return answer;
}

/** One statement in a transaction of its own. */
static int alone(dml_t *self, statement_t *s) {

  if (begun(self))
    return -1;

  return ended(self, sent(self, s, NULL, false));
}

/* ------------------------------------------------------------------ */
/* the schema, and what the rows are found by                         */
/* ------------------------------------------------------------------ */

static yyjson_mut_val *columnsOf(dml_t *self, const char *table) {

  yyjson_mut_val *spec = yyjson_mut_obj_get(
      yyjson_mut_obj_get(rootOf(self->state->schema), "c"), table);
  yyjson_mut_val *columns = yyjson_mut_obj_get(spec, "columns");

  return columns != NULL ? columns : spec;
}

static bool flagged(dml_t *self, const char *table) {
  return yyjson_mut_obj_get(columnsOf(self, table), FLAG) != NULL;
}

/**
 * The key of a table, to change and restore its rows by: { key } as given,
 * or the primary key the schema knows - an array in the record's document.
 */
static yyjson_mut_val *keyOf(dml_t *self, const char *table, json_t options,
                             const char *action) {

  yyjson_mut_val *key = yyjson_mut_arr(self->record);
  json_t given = options.get("key");

  if (isText(given)) {
    yyjson_mut_arr_add_strcpy(self->record, key, given.text());
    return key;
  }

  if (isArray(given) && given.count() > 0) {
    for (int i = 0; i < given.count(); ++i)
      yyjson_mut_arr_add_strcpy(self->record, key, given.at(i).text());
    return key;
  }

  yyjson_mut_val *columns = columnsOf(self, table);
  yyjson_mut_obj_iter walk;
  yyjson_mut_val *name;

  if (columns != NULL && yyjson_mut_is_obj(columns)) {

    yyjson_mut_obj_iter_init(columns, &walk);

    while ((name = yyjson_mut_obj_iter_next(&walk)) != NULL) {
      yyjson_mut_val *primary = yyjson_mut_obj_get(
          yyjson_mut_obj_iter_get_val(name), "primaryKey");
      if (primary != NULL && yyjson_mut_is_true(primary))
        yyjson_mut_arr_add_strcpy(self->record, key, yyjson_mut_get_str(name));
    }
  }

  if (yyjson_mut_arr_size(key) == 0) {
    fail(self, TEXT`${action}("${table}") needs the primary key of "${table}", which is unknown to the schema of db-migrate. Pass it with { key: 'id' }${strcmp(action, "purge") == 0 ? "." : ", or pass { irreversible: true } to change the rows without being able to revert it."}`);
    return NULL;
  }

  return key;
}

/** `"a", "b"` for a list of names. */
static void names(dml_t *self, dbm_text_t *out, yyjson_mut_val *list) {

  for (size_t i = 0; i < yyjson_mut_arr_size(list); ++i) {
    if (i > 0)
      out->put(", ");
    quoted(self, out, textOf(yyjson_mut_arr_get(list, i)));
  }
}

/** The rows `where` matches: an object, a SQL text, or [sql, params]. */
static int whereOf(dml_t *self, statement_t *into, json_t where) {

  if (isText(where)) {
    into->sql.put(where.text());
    return 0;
  }

  if (isArray(where) && where.count() > 0 && isText(where.at(0))) {

    json_t params = where.at(1);

    into->sql.put(where.at(0).text());

    if (isArray(params))
      for (int i = 0; i < params.count(); ++i)
        param(into, params.at(i));

    return 0;
  }

  if (!isObject(where))
    return fail(self, TEXT`where is an object like { status: 'old' }, a SQL string or [sql, params]`);

  if (where.count() == 0) {
    into->sql.put("1 = 1");
    return 0;
  }

  for (int i = 0; i < where.count(); ++i) {

    const char *column = where.keyAt(i);
    json_t value = where.get(column);

    if (i > 0)
      into->sql.put(" AND ");

    if (strcmp(value.kind(), "null") == 0) {
      quoted(self, &into->sql, column);
      into->sql.put(" IS NULL");
    } else if (isArray(value)) {

      if (value.count() == 0) {
        into->sql.put("1 = 0");
        continue;
      }

      quoted(self, &into->sql, column);
      into->sql.put(" IN (");

      for (int j = 0; j < value.count(); ++j) {
        into->sql.put(j > 0 ? ", ?" : "?");
        param(into, value.at(j));
      }

      into->sql.put(")");
    } else {
      quoted(self, &into->sql, column);
      into->sql.put(" = ?");
      param(into, value);
    }
  }

  return 0;
}

/** The rows, matched by their key. */
static void byKey(dml_t *self, statement_t *into, json_t rows,
                  yyjson_mut_val *key) {

  if (yyjson_mut_arr_size(key) == 1) {

    const char *k = textOf(yyjson_mut_arr_get(key, 0));

    quoted(self, &into->sql, k);
    into->sql.put(" IN (");

    for (int i = 0; i < rows.count(); ++i) {
      into->sql.put(i > 0 ? ", ?" : "?");
      param(into, rows.at(i).get(k));
    }

    into->sql.put(")");
    return;
  }

  for (int i = 0; i < rows.count(); ++i) {

    into->sql.put(i > 0 ? " OR (" : "(");

    for (size_t j = 0; j < yyjson_mut_arr_size(key); ++j) {

      const char *k = textOf(yyjson_mut_arr_get(key, j));

      if (j > 0)
        into->sql.put(" AND ");

      quoted(self, &into->sql, k);
      into->sql.put(" = ?");
      param(into, rows.at(i).get(k));
    }

    into->sql.put(")");
  }
}

static long batchOf(dml_t *self, json_t options) {

  json_t batch = options.get("batch");

  if (batch.node != NULL && strcmp(batch.kind(), "number") == 0 &&
      batch.number() > 0)
    return (long)batch.number();

  if (isText(batch) && atol(batch.text()) > 0)
    return atol(batch.text());

  /* the worker's, for a job */
  if (self->job != NULL && self->job->batch > 0)
    return self->job->batch;

  return BATCH;
}

/**
 * Changes the rows matching `condition` in batches by their key, until none
 * matches any more: `change` - "UPDATE t SET ... WHERE " - followed by the
 * rows' keys. Changing a row has to make it stop matching, so this carries
 * on by itself after an interruption.
 */
static int untilDone(dml_t *self, const char *table, yyjson_mut_val *key,
                     statement_t *condition, long size, statement_t *change) {

  for (;;) {

    statement_t select;
    json_t rows;

    opened(&select);
    select.sql.put("SELECT ");
    names(self, &select.sql, key);
    select.sql.put(" FROM ");
    quoted(self, &select.sql, table);
    select.sql.put(" WHERE ");
    followedBy(&select, condition);
    select.sql.put(" ORDER BY ");
    names(self, &select.sql, key);
    select.sql.append(TEXT` LIMIT ${size}`);

    int answer = sent(self, &select, &rows, true);

    closed(&select);

    if (answer != 0)
      return -1;

    if (rows.count() == 0) {
      rows.release();
      return 0;
    }

    statement_t batch;

    opened(&batch);
    followedBy(&batch, change);
    byKey(self, &batch, rows, key);
    rows.release();

    answer = alone(self, &batch);
    closed(&batch);

    if (answer != 0 || between(self))
      return -1;
  }
}

/* ------------------------------------------------------------------ */
/* the state: the record, the lock row, the registries                */
/* ------------------------------------------------------------------ */

static yyjson_mut_val *stepsOf(dml_t *self) {
  return yyjson_mut_obj_get(rootOf(self->record), "s");
}

/** The record of the migration, as node writes it after every change. */
static int save(dml_t *self) {

  if (self->dry)
    return 0;

  char *text = yyjson_mut_write(self->record, 0, NULL);
  int answer = text == NULL ||
               dbmKvUpdate(self->state->db, self->state->table, self->name,
                           text);

  free(text);
  return answer ? failedState(self) : 0;
}

/** A field of the lock row set to a step - "step", "learned", "done". */
/** A worker stops: not a failure, the job goes on with the next run. */
static int stopping(dml_t *self) {
  self->stopped = true;
  return fail(self, TEXT`stopped`);
}

/** A field of the lock row set to a step - "step", "learned", "done" -
    or of the job, for one run by a worker. */
static int mark(dml_t *self, const char *field, long op) {

  char changes[64];

  if (self->dry)
    return 0;

  dbmWrite(changes, sizeof changes, TEXT`{"${field}":${op}}`);

  if (self->job != NULL) {

    char why[300];

    if (dbmJobUpdate(self->job, changes, why, sizeof why))
      return fail(self, TEXT`${why}`);

    return self->job->lost ? stopping(self) : 0;
  }

  return dbmStateMark(self->state, changes) ? failedState(self) : 0;
}

/**
 * Between two steps or batches, a worker stops when it is asked to, gives
 * way to migrations, and pauses.
 */
static int between(dml_t *self) {

  dbm_job_t *job = self->job;

  if (job == NULL)
    return 0;

  if (job->lost || (job->stopping != NULL && *job->stopping))
    return stopping(self);

  if (dbmJobsPaused(self->state, job->seen)) {
    dbmSay(stdout, TEXT`[INFO] [jobs] ${self->name} pauses for migrations\n`);
    return stopping(self);
  }

  if (job->pause > 0)
    dbmSleep(job->pause);

  return 0;
}

typedef bool (*mutation_t)(yyjson_mut_doc *doc, yyjson_mut_val *value,
                           void *context);

/**
 * A row of the state changed by compare and swap, tried again until nobody
 * wrote in between - node's changeState. `mutate` answers false to leave it.
 */
static int changeState(dml_t *self, const char *key, mutation_t mutate,
                       void *context) {

  dbm_state_t *state = self->state;

  if (self->dry)
    return 0;

  for (int attempt = 0; attempt < 50; ++attempt) {

    char *row = NULL;

    if (dbmKvGet(state->db, state->table, key, &row))
      return failedState(self);

    yyjson_mut_doc *doc = dbmRecordFrom(NULL);
    yyjson_doc *read = row != NULL ? yyjson_read(row, strlen(row), 0) : NULL;

    /* the record's parts are not this row's: an object, as it was */
    yyjson_mut_doc_set_root(doc, read != NULL && yyjson_is_obj(yyjson_doc_get_root(read))
                                     ? yyjson_val_mut_copy(doc, yyjson_doc_get_root(read))
                                     : yyjson_mut_obj(doc));
    yyjson_doc_free(read);

    if (!mutate(doc, rootOf(doc), context)) {
      yyjson_mut_doc_free(doc);
      free(row);
      return 0;
    }

    char *next = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);

    if (row == NULL)
      dbmKvInsert(state->db, state->table, key, next);
    else
      dbmKvSwap(state->db, state->table, key, next, row);

    free(row);

    char *after = NULL;
    bool done = dbmKvGet(state->db, state->table, key, &after) == 0 &&
                after != NULL && strcmp(after, next) == 0;

    free(after);
    free(next);

    if (done)
      return 0;
  }

  return fail(self, TEXT`[release] could not change ${key}, too much contention`);
}

/** A key set to a value, where it is if it is there - as JS assigns. */
static void assign(yyjson_mut_doc *doc, yyjson_mut_val *object,
                   const char *key, yyjson_mut_val *value) {

  if (!yyjson_mut_obj_replace(object, yyjson_mut_str(doc, key), value))
    yyjson_mut_obj_add(object, yyjson_mut_strcpy(doc, key), value);
}

typedef struct {
  dml_t *self;
  const char *backup;
  const char *table;
} backup_change_t;

static bool registeringBackup(yyjson_mut_doc *doc, yyjson_mut_val *value,
                              void *context) {

  backup_change_t *change = context;
  const char *release = change->self->state->releaseLabel;
  yyjson_mut_val *entry = yyjson_mut_obj(doc);

  yyjson_mut_obj_add_strcpy(doc, entry, "m", change->self->name);
  yyjson_mut_obj_add_strcpy(doc, entry, "t", change->table);

  if (release != NULL)
    yyjson_mut_obj_add_strcpy(doc, entry, "r", release);
  else
    yyjson_mut_obj_add_null(doc, entry, "r");

  assign(doc, value, change->backup, entry);
  return true;
}

static int registerBackup(dml_t *self, const char *backup, const char *table) {
  backup_change_t change = {self, backup, table};
  return changeState(self, BACKUPS, registeringBackup, &change);
}

static bool forgettingBackups(yyjson_mut_doc *doc, yyjson_mut_val *value,
                              void *context) {

  yyjson_mut_val *backups = context;
  bool any = false;

  (void)doc;

  for (size_t i = 0; i < yyjson_mut_arr_size(backups); ++i)
    any = yyjson_mut_obj_remove_key(
              value, textOf(yyjson_mut_arr_get(backups, i))) != NULL ||
          any;

  return any;
}

/** `backups`: an array of their names. */
static int forgetBackups(dml_t *self, yyjson_mut_val *backups) {
  return changeState(self, BACKUPS, forgettingBackups, backups);
}

/** { releases, drop } as node checks them, into `into`. */
static int purgeOptions(dml_t *self, json_t options, yyjson_mut_doc *doc,
                        yyjson_mut_val *into) {

  char why[300];

  if (dbmReleaseOptions(options, doc, into, why, sizeof why))
    return fail(self, TEXT`${why}`);

  return 0;
}

typedef struct {
  dml_t *self;
  const char *mark;
  const char *table;
  yyjson_mut_val *key;
  json_t purge;
} purge_change_t;

static bool schedulingPurge(yyjson_mut_doc *doc, yyjson_mut_val *value,
                            void *context) {

  purge_change_t *change = context;
  const char *release = change->self->state->releaseLabel;
  yyjson_mut_val *entry = yyjson_mut_obj(doc);
  yyjson_mut_val *options = yyjson_mut_obj(doc);

  yyjson_mut_obj_add_strcpy(doc, entry, "t", change->table);
  yyjson_mut_obj_add_val(doc, entry, "key",
                         yyjson_mut_val_mut_copy(doc, change->key));

  if (release != NULL)
    yyjson_mut_obj_add_strcpy(doc, entry, "r", release);
  else
    yyjson_mut_obj_add_null(doc, entry, "r");

  if (isObject(change->purge))
    purgeOptions(change->self, change->purge, doc, options);

  if (yyjson_mut_obj_size(options) > 0)
    yyjson_mut_obj_add_val(doc, entry, "o", options);

  assign(doc, value, change->mark, entry);
  return true;
}

typedef struct {
  const char *table;
  const char *prefix;
  bool exact;
} purges_gone_t;

static bool forgettingPurges(yyjson_mut_doc *doc, yyjson_mut_val *value,
                             void *context) {

  purges_gone_t *gone = context;
  yyjson_mut_obj_iter walk;
  yyjson_mut_val *mark;
  bool any = false;

  (void)doc;
  yyjson_mut_obj_iter_init(value, &walk);

  while ((mark = yyjson_mut_obj_iter_next(&walk)) != NULL) {

    const char *text = yyjson_mut_get_str(mark);
    yyjson_mut_val *entry = yyjson_mut_obj_iter_get_val(mark);
    bool matches = gone->exact ? strcmp(text, gone->prefix) == 0
                               : strncmp(text, gone->prefix,
                                         strlen(gone->prefix)) == 0;

    if (matches &&
        strcmp(textOf(yyjson_mut_obj_get(entry, "t")), gone->table) == 0) {
      yyjson_mut_obj_iter_remove(&walk);
      any = true;
    }
  }

  return any;
}

/**
 * The purges of `table` forgotten: every mark starting with `prefix`, or
 * the one mark that is `prefix` when `exact` - node takes `|del:m#1` as
 * the prefix of `|del:m#10` too.
 */
static int forgetPurges(dml_t *self, const char *table, const char *prefix,
                        bool exact) {
  purges_gone_t gone = {table, prefix, exact};
  return changeState(self, PURGES, forgettingPurges, &gone);
}

/* ------------------------------------------------------------------ */
/* backups, and when they are due                                     */
/* ------------------------------------------------------------------ */

/** `__dbm_backup_` and 16 hex of the sha256 of `<migration>#<op>`. */
static void backupName(dml_t *self, int op, char into[32]) {

  char seed[600];
  char hex[65];
  size_t length = dbmWrite(seed, sizeof seed, TEXT`${self->name}#${(long)op}`);

  dbmSha256(seed, length, hex);
  hex[16] = '\0';
  dbmWrite(into, 32, TEXT`__dbm_backup_${hex}`);
}

/**
 * The migrations with backups - `{m: {age, releases, drop, backups}}` - those
 * due only, unless `all`. Their age counts the releases since the one they
 * were made in; the options are the project's `deprecation`, else node's
 * defaults, 4 releases and dropping by hand.
 */
static yyjson_mut_doc *backupsOf(dml_t *self, bool all) {

  dbm_state_t *state = self->state;
  yyjson_mut_doc *groups = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root = yyjson_mut_obj(groups);
  char *row = NULL;
  long releases;
  char drop[8];

  yyjson_mut_doc_set_root(groups, root);

  if (dbmKvGet(state->db, state->table, BACKUPS, &row) || row == NULL)
    return groups;

  /* backups carry no options of their own: the project's, or node's */
  if (dbmReleaseSettings(state, NULL, &releases, drop, self->error,
                         sizeof self->error)) {
    self->failed = true;
    free(row);
    return groups;
  }

  json_t registry = meta_toJSON(row);

  free(row);

  for (int i = 0; i < registry.count(); ++i) {

    const char *backup = registry.keyAt(i);
    json_t entry = registry.get(backup);
    const char *migration = entry.get("m").text();
    yyjson_mut_val *group = yyjson_mut_obj_get(root, migration);

    if (group == NULL) {

      json_t release = entry.get("r");

      group = yyjson_mut_obj(groups);
      yyjson_mut_obj_add_int(groups, group, "age",
                             dbmReleaseAge(state, state->releaseCurrent,
                                           isText(release) ? release.text()
                                                           : NULL));
      yyjson_mut_obj_add_int(groups, group, "releases", releases);
      yyjson_mut_obj_add_strcpy(groups, group, "drop", drop);
      yyjson_mut_obj_add_val(groups, group, "backups", yyjson_mut_arr(groups));
      yyjson_mut_obj_add(root, yyjson_mut_strcpy(groups, migration), group);
    }

    yyjson_mut_arr_add_strcpy(groups, yyjson_mut_obj_get(group, "backups"),
                              backup);
  }

  registry.release();

  if (all)
    return groups;

  yyjson_mut_obj_iter walk;
  yyjson_mut_val *key;

  yyjson_mut_obj_iter_init(root, &walk);

  while ((key = yyjson_mut_obj_iter_next(&walk)) != NULL) {
    yyjson_mut_val *group = yyjson_mut_obj_iter_get_val(key);
    if (numberOf(group, "age") < numberOf(group, "releases"))
      yyjson_mut_obj_iter_remove(&walk);
  }

  return groups;
}

/** Its steps with one of `backups` recorded as irreversible: {t:3, a, c:[t], n}. */
static void finalSteps(yyjson_mut_doc *doc, yyjson_mut_val *steps,
                       yyjson_mut_val *backups) {

  for (size_t i = 0; i < yyjson_mut_arr_size(steps); ++i) {

    yyjson_mut_val *entry = yyjson_mut_arr_get(steps, i);
    yyjson_mut_val *args = yyjson_mut_obj_get(entry, "c");
    const char *backup = textOf(yyjson_mut_arr_get(args, 1));
    bool listed = false;

    if (numberOf(entry, "t") != 4)
      continue;

    for (size_t j = 0; j < yyjson_mut_arr_size(backups); ++j)
      listed = listed ||
               strcmp(textOf(yyjson_mut_arr_get(backups, j)), backup) == 0;

    if (!listed)
      continue;

    yyjson_mut_val *final = yyjson_mut_obj(doc);
    yyjson_mut_val *table = yyjson_mut_arr(doc);

    yyjson_mut_arr_append(table, yyjson_mut_val_mut_copy(
                                     doc, yyjson_mut_arr_get(args, 0)));
    yyjson_mut_obj_add_int(doc, final, "t", 3);
    yyjson_mut_obj_add_strcpy(doc, final, "a",
                              textOf(yyjson_mut_obj_get(entry, "a")));
    yyjson_mut_obj_add_val(doc, final, "c", table);
    yyjson_mut_obj_add_int(doc, final, "n", numberOf(entry, "n"));
    yyjson_mut_arr_replace(steps, i, final);
  }
}

/**
 * The backups of a migration dropped, which can not be reverted afterwards:
 * its steps are recorded as irreversible first.
 */
static int dropGroup(dml_t *self, const char *migration,
                     yyjson_mut_val *backups) {

  dbm_state_t *state = self->state;

  dbmSay(stdout, TEXT`[INFO] [release] dropping the backups of ${migration}, it can not be reverted anymore\n`);

  if (self->dry)
    return 0;

  if (strcmp(migration, self->name) == 0) {
    finalSteps(self->record, stepsOf(self), backups);
    if (save(self))
      return -1;
  } else {

    char *row = NULL;

    if (dbmKvGet(state->db, state->table, migration, &row))
      return failedState(self);

    if (row != NULL) {

      yyjson_mut_doc *record = dbmRecordFrom(row);

      free(row);
      finalSteps(record, yyjson_mut_obj_get(rootOf(record), "s"), backups);

      char *text = yyjson_mut_write(record, 0, NULL);
      int answer = text == NULL ||
                   dbmKvUpdate(state->db, state->table, migration, text);

      free(text);
      yyjson_mut_doc_free(record);

      if (answer)
        return failedState(self);
    }
  }

  for (size_t i = 0; i < yyjson_mut_arr_size(backups); ++i) {

    dbm_text_t name = {0};
    quoted(self, &name, textOf(yyjson_mut_arr_get(backups, i)));

    int answer = plain(self, TEXT`DROP TABLE IF EXISTS ${name.text}`);

    name.release();

    if (answer)
      return -1;
  }

  return forgetBackups(self, backups);
}

/* ------------------------------------------------------------------ */
/* steps                                                              */
/* ------------------------------------------------------------------ */

/**
 * The next step counted and named; 1 when an interrupted run did it
 * already, so it is skipped - node's step, up to its record.
 */
static int starting(dml_t *self, text_t current) {

  int op = ++self->op;

  dbmWrite(self->current, sizeof self->current, current);

  if (self->recovery != NULL && op <= self->recovery->done) {
    dbmSay(stdout, TEXT`[INFO] [recovery] ${self->name}: skipping already executed step ${(long)op}/${self->recovery->done} ${self->current}\n`);
    return 1;
  }

  return mark(self, "step", op);
}

/**
 * The step's entry: the one an interrupted run recorded already, or `fresh`,
 * recorded - written before the step runs, so it can always be reverted.
 */
static yyjson_mut_val *recorded(dml_t *self, yyjson_mut_val *fresh) {

  int op = self->op;
  yyjson_mut_val *steps = stepsOf(self);

  if (self->recovery != NULL && op <= self->recovery->learned) {
    for (size_t i = 0; i < yyjson_mut_arr_size(steps); ++i) {
      yyjson_mut_val *entry = yyjson_mut_arr_get(steps, i);
      if (numberOf(entry, "n") == op) {
        dbmSay(stdout, TEXT`[INFO] [recovery] ${self->name}: continuing interrupted step ${(long)op} ${self->current}\n`);
        return entry;
      }
    }
  }

  for (size_t i = yyjson_mut_arr_size(steps); i > 0; --i)
    if (numberOf(yyjson_mut_arr_get(steps, i - 1), "n") == op)
      yyjson_mut_arr_remove(steps, i - 1);

  yyjson_mut_obj_add_int(self->record, fresh, "n", op);
  yyjson_mut_arr_append(steps, fresh);

  if (save(self) || mark(self, "learned", op))
    return NULL;

  return fresh;
}

static int finished(dml_t *self, int answer) {

  if (answer != 0) {
    if (!self->failed)
      failedDriver(self);
    return -1;
  }

  self->done = self->op;
  return mark(self, "done", self->op) || between(self) ? -1 : 0;
}

/** A new entry: {t, a, c: []}, c to fill. */
static yyjson_mut_val *entryOf(dml_t *self, int t, const char *action,
                               yyjson_mut_val **args) {

  yyjson_mut_val *entry = yyjson_mut_obj(self->record);

  *args = yyjson_mut_arr(self->record);
  yyjson_mut_obj_add_int(self->record, entry, "t", t);
  yyjson_mut_obj_add_strcpy(self->record, entry, "a", action);
  yyjson_mut_obj_add_val(self->record, entry, "c", *args);
  return entry;
}

/** A value as JSON.stringify writes it, for the log. */
static void said(json_t value, char *into, size_t room) {

  char *text = value.node != NULL ? yyjson_mut_val_write(value.node, 0, NULL) : NULL;

  dbmWrite(into, room, TEXT`${text != NULL ? text : "undefined"}`);
  free(text);
}

/* ------------------------------------------------------------------ */
/* copies of the rows                                                 */
/* ------------------------------------------------------------------ */

/**
 * The rows about to change copied into the step's backup table, once: one
 * an interrupted run completed holds the rows as they were before the step.
 * `columns` NULL copies the whole rows.
 */
static int backup(dml_t *self, yyjson_mut_val *entry, const char *table,
                  yyjson_mut_val *columns, statement_t *condition) {

  if (numberOf(entry, "b") != 0)
    return 0;

  const char *name = textOf(yyjson_mut_arr_get(yyjson_mut_obj_get(entry, "c"), 1));
  dbm_text_t into = {0};
  dbm_text_t from = {0};
  dbm_text_t select = {0};

  quoted(self, &into, name);
  quoted(self, &from, table);

  if (columns != NULL)
    names(self, &select, columns);
  else
    select.put("*");

  /* outside of transactions: creating a table commits one in MySQL */
  int answer = plain(self, TEXT`DROP TABLE IF EXISTS ${into.text}`) ||
               plain(self, TEXT`CREATE TABLE ${into.text} AS SELECT ${select.text} FROM ${from.text} WHERE 1 = 0`);

  if (answer == 0) {

    statement_t copy;

    opened(&copy);
    copy.sql.append(TEXT`INSERT INTO ${into.text} SELECT ${select.text} FROM ${from.text} WHERE `);
    followedBy(&copy, condition);
    answer = alone(self, &copy);
    closed(&copy);
  }

  into.release();
  from.release();
  select.release();

  if (answer != 0)
    return -1;

  setNumber(self->record, entry, "b", 1);
  setNumber(self->record, entry, "o", 0);

  if (save(self))
    return -1;

  /* dropped once the migration is final, after enough releases */
  return registerBackup(self, name, table);
}

/**
 * The rows of the backup changed in batches, ordered by their key - `change`
 * followed by their keys. The progress is saved after each batch; a batch
 * changed again after an interruption is set to the same values.
 */
static int batches(dml_t *self, yyjson_mut_val *entry, long size,
                   statement_t *change) {

  yyjson_mut_val *args = yyjson_mut_obj_get(entry, "c");
  const char *name = textOf(yyjson_mut_arr_get(args, 1));
  yyjson_mut_val *key = yyjson_mut_arr_get(args, 2);

  for (;;) {

    statement_t select;
    json_t rows;

    opened(&select);
    select.sql.put("SELECT ");
    names(self, &select.sql, key);
    select.sql.put(" FROM ");
    quoted(self, &select.sql, name);
    select.sql.put(" ORDER BY ");
    names(self, &select.sql, key);
    select.sql.append(TEXT` LIMIT ${size} OFFSET ${numberOf(entry, "o")}`);

    int answer = sent(self, &select, &rows, true);

    closed(&select);

    if (answer != 0)
      return -1;

    int count = rows.count();

    if (count == 0) {
      rows.release();
      return 0;
    }

    statement_t batch;

    opened(&batch);
    followedBy(&batch, change);
    byKey(self, &batch, rows, key);
    rows.release();

    answer = alone(self, &batch);
    closed(&batch);

    if (answer != 0)
      return -1;

    setNumber(self->record, entry, "o", numberOf(entry, "o") + count);

    if (self->driver->verbose)
      dbmSay(stdout, TEXT`[dml] ${self->name}: ${numberOf(entry, "o")} rows ${textOf(yyjson_mut_obj_get(entry, "a"))}d\n`);

    if (save(self) || between(self))
      return -1;
  }
}

/* ------------------------------------------------------------------ */
/* the instructions                                                   */
/* ------------------------------------------------------------------ */

static int insertRows(dml_t *self, const char *table, json_t rows,
                      json_t options) {

  bool irreversible = isTrue(options, "irreversible");

  if (!irreversible && !flagged(self, table))
    return fail(self, TEXT`insert("${table}") can only be reverted for tables created by v2 migrations, by their ${FLAG} column${columnsOf(self, table) != NULL ? ", which \"" : ""}${columnsOf(self, table) != NULL ? table : ""}${columnsOf(self, table) != NULL ? "\" does not have" : ""}. Pass { irreversible: true } to insert the rows without being able to revert it.`);

  int skip = starting(self, TEXT`insert("${table}")`);

  if (skip != 0)
    return skip > 0 ? 0 : -1;

  yyjson_mut_val *args;
  yyjson_mut_val *entry = entryOf(self, irreversible ? 3 : 4, "insert", &args);
  char flag[600];

  yyjson_mut_arr_add_strcpy(self->record, args, table);
  dbmWrite(flag, sizeof flag, TEXT`${self->name}#${(long)self->op}`);

  if (!irreversible)
    yyjson_mut_arr_add_strcpy(self->record, args, flag);

  entry = recorded(self, entry);

  if (entry == NULL)
    return -1;

  if (rows.count() == 0)
    return finished(self, 0);

  if (begun(self))
    return finished(self, -1);

  int answer = 0;

  if (!irreversible) {

    /* the rows of an interrupted run of this step */
    statement_t again;

    dbmWrite(flag, sizeof flag, TEXT`${textOf(yyjson_mut_arr_get(yyjson_mut_obj_get(entry, "c"), 1))}`);
    opened(&again);
    again.sql.put("DELETE FROM ");
    quoted(self, &again.sql, table);
    again.sql.put(" WHERE ");
    quoted(self, &again.sql, FLAG);
    again.sql.put(" = ?");
    paramText(&again, flag);
    answer = sent(self, &again, NULL, false);
    closed(&again);
  }

  for (int i = 0; answer == 0 && i < rows.count(); ++i) {

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *row = yyjson_mut_val_mut_copy(doc, rows.at(i).node);

    if (!irreversible) {
      yyjson_mut_obj_remove_key(row, FLAG);
      yyjson_mut_obj_add_strcpy(doc, row, FLAG, flag);
    }

    yyjson_mut_doc_set_root(doc, row);

    json_t marked = meta_jsonFromMut(doc);

    answer = self->driver->insert(self->driver, table, marked);
    marked.release();
  }

  return finished(self, ended(self, answer));
}

int dml_t__insertWith(dml_t *self, const char *table, json_t rows,
                      json_t options) {

  if (self->failed)
    return -1;

  json_t list = dbmRowsOf(rows, meta_toJSON("null"), self->error,
                          sizeof self->error);
  defer list.release();

  if (self->error[0] != '\0') {
    self->failed = true;
    return -1;
  }

  return insertRows(self, table, list, options);
}

int dml_t__insert(dml_t *self, const char *table, json_t rows) {
  return dml_t__insertWith(self, table, rows, meta_toJSON("{}"));
}

int dml_t__insertColumnsWith(dml_t *self, const char *table, json_t columns,
                             json_t values, json_t options) {

  if (self->failed)
    return -1;

  json_t list = dbmRowsOf(columns, values, self->error, sizeof self->error);
  defer list.release();

  if (self->error[0] != '\0') {
    self->failed = true;
    return -1;
  }

  return insertRows(self, table, list, options);
}

int dml_t__insertColumns(dml_t *self, const char *table, json_t columns,
                         json_t values) {
  return dml_t__insertColumnsWith(self, table, columns, values,
                                  meta_toJSON("{}"));
}

int dml_t__updateWith(dml_t *self, const char *table, json_t set, json_t where,
                      json_t options) {

  if (self->failed)
    return -1;

  if (!isObject(set) || set.count() == 0)
    return fail(self, TEXT`update("${table}") needs the values to set`);

  bool irreversible = isTrue(options, "irreversible");
  statement_t condition;

  opened(&condition);

  if (whereOf(self, &condition, where)) {
    closed(&condition);
    return -1;
  }

  yyjson_mut_val *key = irreversible ? NULL
                                     : keyOf(self, table, options, "update");

  if (!irreversible && key == NULL) {
    closed(&condition);
    return -1;
  }

  /* the key is what the rows are restored by */
  dbm_text_t changed = {0};

  for (size_t i = 0; key != NULL && i < yyjson_mut_arr_size(key); ++i) {
    const char *k = textOf(yyjson_mut_arr_get(key, i));
    if (set.get(k).node != NULL)
      changed.append(TEXT`${changed.length > 0 ? ", " : ""}${k}`);
  }

  if (changed.length > 0) {
    fail(self, TEXT`update("${table}") can not change the key ${changed.text}, by which it reverts the rows`);
    changed.release();
    closed(&condition);
    return -1;
  }

  changed.release();

  char shown[400];
  said(set, shown, sizeof shown);

  int skip = starting(self, TEXT`update("${table}", ${shown})`);

  if (skip != 0) {
    closed(&condition);
    return skip > 0 ? 0 : -1;
  }

  /* SET "a" = ?, "b" = ?, with the values */
  statement_t change;

  opened(&change);
  change.sql.put("UPDATE ");
  quoted(self, &change.sql, table);
  change.sql.put(" SET ");

  for (int i = 0; i < set.count(); ++i) {
    if (i > 0)
      change.sql.put(", ");
    quoted(self, &change.sql, set.keyAt(i));
    change.sql.put(" = ?");
    param(&change, set.get(set.keyAt(i)));
  }

  change.sql.put(" WHERE ");

  yyjson_mut_val *args;
  yyjson_mut_val *entry = entryOf(self, irreversible ? 3 : 4, "update", &args);
  yyjson_mut_val *columns = yyjson_mut_arr(self->record);
  char name[32];

  for (int i = 0; i < set.count(); ++i)
    yyjson_mut_arr_add_strcpy(self->record, columns, set.keyAt(i));

  yyjson_mut_arr_add_strcpy(self->record, args, table);

  if (!irreversible) {
    backupName(self, self->op, name);
    yyjson_mut_arr_add_strcpy(self->record, args, name);
    yyjson_mut_arr_append(args, key);
    yyjson_mut_arr_append(args, columns);
  }

  entry = recorded(self, entry);

  int answer = entry == NULL ? -1 : 0;

  if (answer == 0 && (irreversible || self->dry)) {
    followedBy(&change, &condition);
    answer = alone(self, &change);
  } else if (answer == 0) {

    /* the key and the columns about to change */
    yyjson_mut_val *kept = yyjson_mut_arr(self->record);

    for (size_t i = 0; i < yyjson_mut_arr_size(key); ++i)
      yyjson_mut_arr_append(kept, yyjson_mut_val_mut_copy(self->record, yyjson_mut_arr_get(key, i)));
    for (size_t i = 0; i < yyjson_mut_arr_size(columns); ++i)
      yyjson_mut_arr_append(kept, yyjson_mut_val_mut_copy(self->record, yyjson_mut_arr_get(columns, i)));

    answer = backup(self, entry, table, kept, &condition) ||
             batches(self, entry, batchOf(self, options), &change);
  }

  closed(&change);
  closed(&condition);
  return finished(self, answer);
}

int dml_t__update(dml_t *self, const char *table, json_t set, json_t where) {
  return dml_t__updateWith(self, table, set, where, meta_toJSON("{}"));
}

/** node's concat: the flag followed by a text parameter. */
static void concatenated(dml_t *self, dbm_text_t *out) {

  dbm_text_t flag = {0};
  quoted(self, &flag, FLAG);

  if (strcmp(self->driver->name, "mysql") == 0)
    out->append(TEXT`CONCAT(COALESCE(${flag.text}, ''), ?)`);
  else
    out->append(TEXT`(COALESCE(${flag.text}, '') || CAST(? AS TEXT))`);

  flag.release();
}

/** `!` escapes what LIKE would read: `!`, `%` and `_`. */
static void liked(dbm_text_t *out, const char *text) {
  for (const char *at = text; *at != '\0'; ++at) {
    if (*at == '!' || *at == '%' || *at == '_')
      out->put("!");
    out->putn(at, 1);
  }
}

/**
 * The rows carrying `mark` in their flag: as a prefix of what follows it
 * (`|del:m#`), or as the whole of one mark - followed by the end or by the
 * next mark's `|`, so `|del:m#1` is not found in `|del:m#10`.
 */
static void markedBy(dml_t *self, statement_t *into, const char *mark,
                     bool whole) {

  dbm_text_t pattern = {0};

  pattern.put("%");
  liked(&pattern, mark);

  if (whole) {
    into->sql.put("(");
    quoted(self, &into->sql, FLAG);
    into->sql.put(" LIKE ? ESCAPE '!' OR ");
    quoted(self, &into->sql, FLAG);
    into->sql.put(" LIKE ? ESCAPE '!')");
    paramText(into, pattern.text);
    pattern.put("|%");
    paramText(into, pattern.text);
  } else {
    pattern.put("%");
    quoted(self, &into->sql, FLAG);
    into->sql.put(" LIKE ? ESCAPE '!'");
    paramText(into, pattern.text);
  }

  pattern.release();
}

static int softDelete(dml_t *self, const char *table, json_t where,
                      json_t options) {

  json_t column = options.get("column");
  json_t purge = options.get("purge");
  bool time = options.get("value").node == NULL;

  if (!isText(column))
    return fail(self, TEXT`delete("${table}") in soft mode needs the column marking the rows as deleted, { mode: 'soft', column: 'deleted_at' }`);

  if (!flagged(self, table))
    return fail(self, TEXT`delete("${table}") in soft mode marks the rows in their ${FLAG} column, which only tables created by v2 migrations have`);

  statement_t condition;

  opened(&condition);

  if (whereOf(self, &condition, where)) {
    closed(&condition);
    return -1;
  }

  yyjson_mut_val *key = keyOf(self, table, options, "delete");

  if (key == NULL) {
    closed(&condition);
    return -1;
  }

  if (purge.node != NULL && !isTrue(options, "purge") && !isObject(purge)) {
    closed(&condition);
    return fail(self, TEXT`delete("${table}") takes purge: true or { releases, drop }, to purge the rows with a later release`);
  }

  if (isObject(purge)) {
    yyjson_mut_doc *checked = yyjson_mut_doc_new(NULL);
    int wrong = purgeOptions(self, purge, checked, yyjson_mut_obj(checked));
    yyjson_mut_doc_free(checked);
    if (wrong) {
      closed(&condition);
      return -1;
    }
  }

  int skip = starting(self, TEXT`delete("${table}")`);

  if (skip != 0) {
    closed(&condition);
    return skip > 0 ? 0 : -1;
  }

  yyjson_mut_val *args;
  yyjson_mut_val *entry = entryOf(self, 4, "soft", &args);
  char mark[600];

  dbmWrite(mark, sizeof mark, TEXT`|del:${self->name}#${(long)self->op}`);
  yyjson_mut_arr_add_strcpy(self->record, args, table);
  yyjson_mut_arr_add_strcpy(self->record, args, column.text());
  yyjson_mut_arr_append(args, key);
  yyjson_mut_arr_add_strcpy(self->record, args, mark);

  entry = recorded(self, entry);

  if (entry == NULL) {
    closed(&condition);
    return finished(self, -1);
  }

  dbmWrite(mark, sizeof mark, TEXT`${textOf(yyjson_mut_arr_get(yyjson_mut_obj_get(entry, "c"), 3))}`);

  /* rows deleted already, by the application or before, stay as they are */
  statement_t active;

  opened(&active);
  active.sql.put("(");
  followedBy(&active, &condition);
  active.sql.put(") AND ");
  quoted(self, &active.sql, column.text());
  active.sql.put(" IS NULL");
  closed(&condition);

  statement_t change;

  opened(&change);
  change.sql.put("UPDATE ");
  quoted(self, &change.sql, table);
  change.sql.put(" SET ");
  quoted(self, &change.sql, column.text());
  change.sql.put(time ? " = CURRENT_TIMESTAMP, " : " = ?, ");
  quoted(self, &change.sql, FLAG);
  change.sql.put(" = ");
  concatenated(self, &change.sql);
  change.sql.put(" WHERE ");

  if (!time)
    param(&change, options.get("value"));

  paramText(&change, mark);

  int answer = 0;

  /* MySQL keeps 255 characters of a flag: more is refused, not cut off */
  if (strcmp(self->driver->name, "mysql") == 0) {

    statement_t longer;
    json_t rows;

    opened(&longer);
    longer.sql.put("SELECT COUNT(*) AS n FROM ");
    quoted(self, &longer.sql, table);
    longer.sql.put(" WHERE ");
    followedBy(&longer, &active);
    longer.sql.put(" AND CHAR_LENGTH(COALESCE(");
    quoted(self, &longer.sql, FLAG);
    longer.sql.append(TEXT`, '')) + ${(long)strlen(mark)} > 255`);
    answer = sent(self, &longer, &rows, true);
    closed(&longer);

    if (answer == 0) {
      long many = (long)rows.at(0).get("n").number();
      rows.release();
      if (many > 0)
        answer = fail(self, TEXT`delete("${table}") in soft mode can not mark ${many} row(s), their ${FLAG} would be longer than the 255 characters MySQL keeps of it`);
    }
  }

  if (answer == 0 && self->dry) {
    followedBy(&change, &active);
    answer = sent(self, &change, NULL, false);
  } else if (answer == 0) {

    if (purge.node != NULL) {
      purge_change_t scheduled = {self, mark, table, key, purge};
      answer = changeState(self, PURGES, schedulingPurge, &scheduled);
    }

    if (answer == 0)
      answer = untilDone(self, table, key, &active, batchOf(self, options), &change);
  }

  closed(&change);
  closed(&active);
  return finished(self, answer);
}

int dml_t__deleteWith(dml_t *self, const char *table, json_t where,
                      json_t options) {

  if (self->failed)
    return -1;

  json_t mode = options.get("mode");
  const char *how = isText(mode) ? mode.text() : "copy";

  if (strcmp(how, "soft") == 0)
    return softDelete(self, table, where, options);

  if (strcmp(how, "copy") != 0)
    return fail(self, TEXT`delete("${table}") has no mode "${how}", use 'copy' or 'soft'`);

  bool irreversible = isTrue(options, "irreversible");
  statement_t condition;

  opened(&condition);

  if (whereOf(self, &condition, where)) {
    closed(&condition);
    return -1;
  }

  yyjson_mut_val *key = irreversible ? NULL
                                     : keyOf(self, table, options, "delete");

  if (!irreversible && key == NULL) {
    closed(&condition);
    return -1;
  }

  int skip = starting(self, TEXT`delete("${table}")`);

  if (skip != 0) {
    closed(&condition);
    return skip > 0 ? 0 : -1;
  }

  yyjson_mut_val *args;
  yyjson_mut_val *entry = entryOf(self, irreversible ? 3 : 4, "delete", &args);
  char name[32];

  yyjson_mut_arr_add_strcpy(self->record, args, table);

  if (!irreversible) {
    backupName(self, self->op, name);
    yyjson_mut_arr_add_strcpy(self->record, args, name);
    yyjson_mut_arr_append(args, key);
  }

  entry = recorded(self, entry);

  statement_t change;

  opened(&change);
  change.sql.put("DELETE FROM ");
  quoted(self, &change.sql, table);
  change.sql.put(" WHERE ");

  int answer = entry == NULL ? -1 : 0;

  if (answer == 0 && (irreversible || self->dry)) {
    followedBy(&change, &condition);
    answer = alone(self, &change);
  } else if (answer == 0) {
    answer = backup(self, entry, table, NULL, &condition) ||
             batches(self, entry, batchOf(self, options), &change);
  }

  closed(&change);
  closed(&condition);
  return finished(self, answer);
}

int dml_t__delete(dml_t *self, const char *table, json_t where) {
  return dml_t__deleteWith(self, table, where, meta_toJSON("{}"));
}

int dml_t__purgeWith(dml_t *self, const char *table, const char *migration,
                     json_t options) {

  if (self->failed)
    return -1;

  yyjson_mut_val *key = keyOf(self, table, options, "purge");

  if (key == NULL)
    return -1;

  char prefix[600];

  if (migration != NULL)
    dbmWrite(prefix, sizeof prefix, TEXT`|del:${migration}#`);
  else
    dbmWrite(prefix, sizeof prefix, TEXT`|del:`);

  int skip = migration != NULL
                 ? starting(self, TEXT`purge("${table}", "${migration}")`)
                 : starting(self, TEXT`purge("${table}")`);

  if (skip != 0)
    return skip > 0 ? 0 : -1;

  yyjson_mut_val *args;
  yyjson_mut_val *entry = entryOf(self, 3, "purge", &args);

  yyjson_mut_arr_add_strcpy(self->record, args, table);

  if (recorded(self, entry) == NULL)
    return finished(self, -1);

  /* nothing left to purge with a later release */
  if (forgetPurges(self, table, prefix, false))
    return finished(self, -1);

  statement_t condition;
  statement_t change;

  opened(&condition);
  markedBy(self, &condition, prefix, false);
  opened(&change);
  change.sql.put("DELETE FROM ");
  quoted(self, &change.sql, table);
  change.sql.put(" WHERE ");

  int answer;

  if (self->dry) {
    followedBy(&change, &condition);
    answer = sent(self, &change, NULL, false);
  } else {
    answer = untilDone(self, table, key, &condition, batchOf(self, options), &change);
  }

  closed(&change);
  closed(&condition);
  return finished(self, answer);
}

int dml_t__purge(dml_t *self, const char *table, const char *migration) {
  return dml_t__purgeWith(self, table, migration, meta_toJSON("{}"));
}

int dml_t__runSqlWith(dml_t *self, const char *sql, json_t params,
                      json_t options) {

  if (self->failed)
    return -1;

  bool irreversible = isTrue(options, "irreversible");
  json_t revert = options.get("revert");
  bool given = isText(revert) ||
               (isArray(revert) && revert.count() > 0 && isText(revert.at(0)));

  if (!irreversible && !given)
    return fail(self, TEXT`runSql in a dml migration needs the SQL reverting it, runSql(sql, { revert: 'sql' }), or { irreversible: true } to run it without being able to revert it.`);

  int skip = starting(self, TEXT`runSql("${sql}")`);

  if (skip != 0)
    return skip > 0 ? 0 : -1;

  yyjson_mut_val *args;
  yyjson_mut_val *entry = entryOf(self, irreversible ? 3 : 4, "runSql", &args);

  if (irreversible) {
    yyjson_mut_arr_add_strcpy(self->record, args, sql);
  } else if (isText(revert)) {
    yyjson_mut_arr_add_strcpy(self->record, args, revert.text());
    yyjson_mut_arr_append(args, yyjson_mut_arr(self->record));
  } else {
    json_t revertParams = revert.at(1);
    yyjson_mut_arr_add_strcpy(self->record, args, revert.at(0).text());
    yyjson_mut_arr_append(args, isArray(revertParams)
                                    ? copyIn(self->record, revertParams)
                                    : yyjson_mut_arr(self->record));
  }

  if (recorded(self, entry) == NULL)
    return finished(self, -1);

  statement_t s;

  opened(&s);
  s.sql.put(sql);

  if (isArray(params))
    for (int i = 0; i < params.count(); ++i)
      param(&s, params.at(i));

  int answer = alone(self, &s);

  closed(&s);
  return finished(self, answer);
}

int dml_t__runSql(dml_t *self, const char *sql, json_t options) {
  return dml_t__runSqlWith(self, sql, meta_toJSON("[]"), options);
}

int dml_t__dropBackups(dml_t *self, const char *migration) {

  if (self->failed)
    return -1;

  int skip = migration != NULL
                 ? starting(self, TEXT`dropBackups("${migration}")`)
                 : starting(self, TEXT`dropBackups()`);

  if (skip != 0)
    return skip > 0 ? 0 : -1;

  yyjson_mut_val *args;
  yyjson_mut_val *entry = entryOf(self, 3, "dropBackups", &args);

  if (migration != NULL)
    yyjson_mut_arr_add_strcpy(self->record, args, migration);
  else
    yyjson_mut_arr_add_null(self->record, args);

  if (recorded(self, entry) == NULL)
    return finished(self, -1);

  yyjson_mut_doc *groups = backupsOf(self, migration != NULL);
  yyjson_mut_obj_iter walk;
  yyjson_mut_val *key;
  int answer = 0;

  yyjson_mut_obj_iter_init(rootOf(groups), &walk);

  while (answer == 0 && (key = yyjson_mut_obj_iter_next(&walk)) != NULL) {

    const char *m = yyjson_mut_get_str(key);

    if (migration == NULL || strcmp(m, migration) == 0)
      answer = dropGroup(self, m,
                         yyjson_mut_obj_get(yyjson_mut_obj_iter_get_val(key),
                                            "backups"));
  }

  yyjson_mut_doc_free(groups);
  return finished(self, answer);
}

json_t dml_t__all(dml_t *self, sql_t query) {

  json_t rows;
  bool dry = self->driver->dryRun;

  if (self->failed)
    return meta_toJSON("[]");

  /* reading is no step, and a dry run reads too */
  self->driver->dryRun = false;

  int answer = dbmQuery(self->driver, &query, &rows);

  self->driver->dryRun = dry;

  if (answer != 0) {
    failedDriver(self);
    return meta_toJSON("[]");
  }

  return rows;
}

/* ------------------------------------------------------------------ */
/* reverting                                                          */
/* ------------------------------------------------------------------ */

static int revertInsert(dml_t *self, yyjson_mut_val *args) {

  statement_t s;

  opened(&s);
  s.sql.put("DELETE FROM ");
  quoted(self, &s.sql, textOf(yyjson_mut_arr_get(args, 0)));
  s.sql.put(" WHERE ");
  quoted(self, &s.sql, FLAG);
  s.sql.put(" = ?");
  paramText(&s, textOf(yyjson_mut_arr_get(args, 1)));

  int answer = alone(self, &s);

  closed(&s);
  return answer;
}

static int revertRunSql(dml_t *self, yyjson_mut_val *args) {

  statement_t s;
  yyjson_mut_val *params = yyjson_mut_arr_get(args, 1);

  opened(&s);
  s.sql.put(textOf(yyjson_mut_arr_get(args, 0)));

  for (size_t i = 0; i < yyjson_mut_arr_size(params); ++i)
    paramMut(&s, yyjson_mut_arr_get(params, i));

  int answer = alone(self, &s);

  closed(&s);
  return answer;
}

/** The columns of the rows put back, from the backup, a page at a time. */
static int revertUpdate(dml_t *self, yyjson_mut_val *args) {

  const char *table = textOf(yyjson_mut_arr_get(args, 0));
  const char *name = textOf(yyjson_mut_arr_get(args, 1));
  yyjson_mut_val *key = yyjson_mut_arr_get(args, 2);
  yyjson_mut_val *columns = yyjson_mut_arr_get(args, 3);

  for (long offset = 0;;) {

    statement_t select;
    json_t rows;

    opened(&select);
    select.sql.put("SELECT * FROM ");
    quoted(self, &select.sql, name);
    select.sql.put(" ORDER BY ");
    names(self, &select.sql, key);
    select.sql.append(TEXT` LIMIT ${(long)BATCH} OFFSET ${offset}`);

    int answer = sent(self, &select, &rows, true);

    closed(&select);

    if (answer != 0)
      return -1;

    int count = rows.count();

    if (count == 0) {
      rows.release();
      return 0;
    }

    answer = begun(self);

    for (int r = 0; answer == 0 && r < count; ++r) {

      json_t row = rows.at(r);
      statement_t s;

      opened(&s);
      s.sql.put("UPDATE ");
      quoted(self, &s.sql, table);
      s.sql.put(" SET ");

      for (size_t i = 0; i < yyjson_mut_arr_size(columns); ++i) {
        const char *c = textOf(yyjson_mut_arr_get(columns, i));
        if (i > 0)
          s.sql.put(", ");
        quoted(self, &s.sql, c);
        s.sql.put(" = ?");
        param(&s, row.get(c));
      }

      s.sql.put(" WHERE ");

      for (size_t i = 0; i < yyjson_mut_arr_size(key); ++i) {
        const char *k = textOf(yyjson_mut_arr_get(key, i));
        if (i > 0)
          s.sql.put(" AND ");
        quoted(self, &s.sql, k);
        s.sql.put(" = ?");
        param(&s, row.get(k));
      }

      answer = sent(self, &s, NULL, false);
      closed(&s);
    }

    rows.release();

    if (ended(self, answer) != 0)
      return -1;

    offset += count;
  }
}

/** The rows the step deleted put back, those still there left as they are. */
static int revertDelete(dml_t *self, yyjson_mut_val *args) {

  const char *table = textOf(yyjson_mut_arr_get(args, 0));
  const char *name = textOf(yyjson_mut_arr_get(args, 1));
  yyjson_mut_val *key = yyjson_mut_arr_get(args, 2);
  statement_t select;
  json_t rows;

  opened(&select);
  select.sql.put("SELECT * FROM ");
  quoted(self, &select.sql, name);
  select.sql.put(" LIMIT 1");

  int answer = sent(self, &select, &rows, true);

  closed(&select);

  if (answer != 0)
    return -1;

  if (rows.count() == 0) {
    rows.release();
    return 0;
  }

  json_t row = rows.at(0);
  dbm_text_t columns = {0};

  for (int i = 0; i < row.count(); ++i) {
    if (i > 0)
      columns.put(", ");
    quoted(self, &columns, row.keyAt(i));
  }

  rows.release();

  statement_t s;

  opened(&s);
  s.sql.put("INSERT INTO ");
  quoted(self, &s.sql, table);
  s.sql.append(TEXT` (${columns.text}) SELECT ${columns.text} FROM `);
  quoted(self, &s.sql, name);
  s.sql.put(" ");
  quoted(self, &s.sql, "__dbm_b");
  s.sql.put(" WHERE NOT EXISTS (SELECT 1 FROM ");
  quoted(self, &s.sql, table);
  s.sql.put(" ");
  quoted(self, &s.sql, "__dbm_t");
  s.sql.put(" WHERE ");

  for (size_t i = 0; i < yyjson_mut_arr_size(key); ++i) {
    const char *k = textOf(yyjson_mut_arr_get(key, i));
    if (i > 0)
      s.sql.put(" AND ");
    quoted(self, &s.sql, "__dbm_t");
    s.sql.put(".");
    quoted(self, &s.sql, k);
    s.sql.put(" = ");
    quoted(self, &s.sql, "__dbm_b");
    s.sql.put(".");
    quoted(self, &s.sql, k);
  }

  s.sql.put(")");
  answer = alone(self, &s);
  closed(&s);
  columns.release();
  return answer;
}

/** The column cleared and the mark taken out of the flag again. */
static int revertSoft(dml_t *self, yyjson_mut_val *args) {

  const char *table = textOf(yyjson_mut_arr_get(args, 0));
  const char *column = textOf(yyjson_mut_arr_get(args, 1));
  yyjson_mut_val *key = yyjson_mut_arr_get(args, 2);
  const char *mark = textOf(yyjson_mut_arr_get(args, 3));

  if (forgetPurges(self, table, mark, true))
    return -1;

  statement_t condition;
  statement_t change;

  opened(&condition);
  markedBy(self, &condition, mark, true);
  opened(&change);
  change.sql.put("UPDATE ");
  quoted(self, &change.sql, table);
  change.sql.put(" SET ");
  quoted(self, &change.sql, column);
  change.sql.put(" = NULL, ");
  quoted(self, &change.sql, FLAG);
  change.sql.put(" = NULLIF(REPLACE(");
  quoted(self, &change.sql, FLAG);
  change.sql.put(", ?, ''), '') WHERE ");
  paramText(&change, mark);

  int answer = untilDone(self, table, key, &condition, BATCH, &change);

  closed(&change);
  closed(&condition);
  return answer;
}

/** `step 2 purge("pets"), step 3 ...`, or "" when every step reverts. */
static void irreversibleSteps(dml_t *self, char *into, size_t room) {

  yyjson_mut_val *steps = stepsOf(self);
  dbm_text_t list = {0};

  into[0] = '\0';

  for (size_t i = 0; i < yyjson_mut_arr_size(steps); ++i) {

    yyjson_mut_val *entry = yyjson_mut_arr_get(steps, i);
    yyjson_mut_val *first = yyjson_mut_arr_get(yyjson_mut_obj_get(entry, "c"), 0);

    if (numberOf(entry, "t") != 3)
      continue;

    list.append(TEXT`${list.length > 0 ? ", " : ""}step ${numberOf(entry, "n")} ${textOf(yyjson_mut_obj_get(entry, "a"))}("${yyjson_mut_is_str(first) ? yyjson_mut_get_str(first) : "null"}")`);
  }

  if (list.text != NULL)
    dbmWrite(into, room, TEXT`${list.text}`);

  list.release();
}

/**
 * The recorded steps reverted, the last first, and the record deleted. A
 * step with a backup restores the rows from it and drops it afterwards.
 * Each is safe to run again, so a revert interrupted halfway goes on.
 */
static int revert(dml_t *self) {

  yyjson_mut_val *steps = stepsOf(self);

  while (yyjson_mut_arr_size(steps) > 0) {

    yyjson_mut_val *entry = yyjson_mut_arr_get(steps, yyjson_mut_arr_size(steps) - 1);
    yyjson_mut_val *args = yyjson_mut_obj_get(entry, "c");
    const char *action = textOf(yyjson_mut_obj_get(entry, "a"));
    bool copied = action in {"update", "delete"};
    int answer = 0;

    if (numberOf(entry, "t") != 4 ||
        !(action in {"insert", "runSql", "update", "delete", "soft"}))
      return fail(self, TEXT`Invalid state record of a dml migration, ${action}`);

    if (self->dry && (copied || strcmp(action, "soft") == 0)) {
      dbmSay(stdout, TEXT`[INFO] [dml] would revert ${action}("${textOf(yyjson_mut_arr_get(args, 0))}")${copied ? " from " : ""}${copied ? textOf(yyjson_mut_arr_get(args, 1)) : ""}\n`);
    } else {

      /* b: 1 the backup is complete, 2 the rows are restored from it */
      if (!copied || numberOf(entry, "b") == 1) {
        if (strcmp(action, "insert") == 0)
          answer = revertInsert(self, args);
        else if (strcmp(action, "runSql") == 0)
          answer = revertRunSql(self, args);
        else if (strcmp(action, "update") == 0)
          answer = revertUpdate(self, args);
        else if (strcmp(action, "delete") == 0)
          answer = revertDelete(self, args);
        else
          answer = revertSoft(self, args);
      }

      if (answer != 0)
        return -1;

      if (copied) {

        yyjson_mut_val *gone = yyjson_mut_arr(self->record);
        dbm_text_t name = {0};

        yyjson_mut_arr_append(gone, yyjson_mut_val_mut_copy(self->record, yyjson_mut_arr_get(args, 1)));
        quoted(self, &name, textOf(yyjson_mut_arr_get(args, 1)));
        setNumber(self->record, entry, "b", 2);

        answer = save(self) ||
                 plain(self, TEXT`DROP TABLE IF EXISTS ${name.text}`) ||
                 forgetBackups(self, gone);
        name.release();

        if (answer != 0)
          return -1;
      }
    }

    yyjson_mut_arr_remove_last(steps);

    if (save(self))
      return -1;
  }

  if (!self->dry && dbmStateForget(self->state, self->name))
    return failedState(self);

  return 0;
}

/* ------------------------------------------------------------------ */
/* running one                                                        */
/* ------------------------------------------------------------------ */

/** How an interrupted run is resumed, as a v2 migration's is. */
static const char *recoveryOf(const dbm_migration_t *migration, const char *name,
                              const dbm_interrupted_t *interrupted, char *why,
                              size_t room) {

  const char *mode = migration->recovery != NULL ? migration->recovery : "skip";

  if (!(mode in {"skip", "rollback"})) {
    dbmWrite(why, room, TEXT`Invalid recovery mode "${mode}" in migration "${name}", use one of skip, rollback`);
    return NULL;
  }

  if (interrupted->rollback) {

    if (strcmp(mode, "rollback") != 0)
      dbmSay(stderr, TEXT`[WARN] [recovery] ${name}: the previous run was interrupted while rolling back, continuing the rollback\n`);

    return "rollback";
  }

  if (strcmp(mode, "skip") == 0 && interrupted->changed) {
    dbmWrite(why, room, TEXT`Migration "${name}" was interrupted at step ${interrupted->step} and changed since, so the executed steps can not be skipped safely. Use DBM_MIGRATION_DML_WITH(migrate, {recovery: "rollback"}) to revert them, or repair the state manually.`);
    return NULL;
  }

  return mode;
}

/**
 * A run started over after its interrupted one was rolled back: in the
 * lock row, or for a worker in its job - with its record again, which
 * reverting deleted and node's worker never writes back.
 */
static int restart(dml_t *db, const char *hash) {

  dbm_state_t *state = db->state;

  if (db->job != NULL) {

    char changes[200];
    char why[300];
    char *row = NULL;

    dbmWrite(changes, sizeof changes, TEXT`{"step":0,"learned":0,"done":0,"rb":0,"h":"${hash != NULL ? hash : ""}"}`);

    if (dbmJobUpdate(db->job, changes, why, sizeof why))
      return fail(db, TEXT`${why}`);

    if (dbmKvGet(state->db, state->table, db->name, &row) ||
        (row == NULL && dbmKvInsert(state->db, state->table, db->name, "{}")))
      return failedState(db);

    free(row);
    return 0;
  }

  char *stored = dbmStateProgress(state, -1, 1) == 0
                     ? dbmStateBegin(state, db->name, "up", hash, NULL)
                     : NULL;

  if (stored == NULL)
    return fail(db, TEXT`could not begin the state of ${db->name}: ${state->db->error}`);

  free(stored);
  return 0;
}

/**
 * A dml migration run, its start recorded and its record read: an
 * interrupted run recovered first, its steps run and recorded, and on a
 * failure everything it did reverted from that record - unless one of its
 * steps can not be reverted: then the steps executed stay, and the next run
 * continues after them. 0 when it ran, 1 when a worker stopped it to go on
 * later, -1 when it failed; the reason in `why`, or said already.
 */
static int execute(dml_t *db, const dbm_migration_t *migration,
                   dbm_interrupted_t *interrupted, const char *hash,
                   char *why, size_t room) {

  if (interrupted->found) {

    const char *mode = recoveryOf(migration, db->name, interrupted, why, room);

    if (mode == NULL)
      return -1;

    dbmSay(stderr, TEXT`[WARN] [recovery] ${db->name}: the previous run was interrupted at step ${interrupted->step}, ${interrupted->done} steps were executed, recovering by ${mode}\n`);

    if (strcmp(mode, "rollback") == 0) {

      if (mark(db, "rb", 1) || revert(db)) {
        if (db->stopped)
          return 1;
        dbmWrite(why, room, TEXT`could not roll back the interrupted run: ${db->error}`);
        return -1;
      }

      yyjson_mut_doc_free(db->record);
      db->record = dbmRecordFrom(NULL);

      if (restart(db, hash)) {
        dbmWrite(why, room, TEXT`${db->error}`);
        return -1;
      }
    } else {
      db->recovery = interrupted;
      db->done = (int)interrupted->done;
    }
  }

  if (migration->dml(db) != 0 && !db->failed)
    fail(db, TEXT`the migration answered non-zero without saying why`);

  if (db->stopped)
    return 1;

  if (!db->failed)
    return 0;

  char reason[600];
  char instruction[700] = "";
  char irreversible[600];

  /* "at" the step that failed, "after" the last one when the code failed */
  if (db->op > 0)
    dbmWrite(instruction, sizeof instruction, TEXT`${db->done >= db->op ? "after" : "at"} step ${(long)db->op} ${db->current}`);

  dbmWrite(reason, sizeof reason, TEXT`${db->error}`);
  dbmSayFailure(db->driver, db->name,
                instruction[0] != '\0' ? instruction : NULL, reason);
  dbmWrite(why, room, TEXT`${reason}`);
  irreversibleSteps(db, irreversible, sizeof irreversible);

  /* the run stays unfinished, the next continues it */
  if (irreversible[0] != '\0') {
    dbmSay(stderr, TEXT`[ERROR] Migration "${db->name}" failed and can not be rolled back, it ran a step which can not be reverted. The steps executed stay, the next run continues after them.\n`);
    return -1;
  }

  /* the record of the failed step reverts what it changed, if anything */
  dbmSay(stderr, TEXT`[ERROR] Migration "${db->name}" failed, rolling back: ${reason}\n`);
  db->failed = false;
  db->error[0] = '\0';

  if (mark(db, "rb", 1) || revert(db) ||
      (db->job == NULL && !db->dry && dbmStateProgress(db->state, -1, 1))) {
    if (db->stopped)
      return 1;
    dbmSay(stderr, TEXT`[ERROR] and reverting it failed too: ${db->error[0] != '\0' ? db->error : db->state->db->error}\n`);
  } else {
    db->rolledBack = true;
  }

  return -1;
}

/**
 * A dml migration up, in the foreground: its start in the lock row, then
 * run. Answers 0 when it ran; the reason for anything else is in `why`, or
 * was said already.
 */
int dbmUpDml(driver_t *driver, dbm_state_t *state,
             const dbm_migration_t *migration, char *why, size_t room) {

  dml_t db = {.driver = driver, .state = state,
              .name = dbmKeyOf(migration->name), .dry = driver->dryRun,
              .transactional = !migration->noTransaction &&
                               !driver->noTransactions};
  dbm_interrupted_t interrupted = {0};
  char hex[65];
  const char *hash = dbmHashOf(migration, hex);
  char *stored = db.dry ? NULL
                        : dbmStateBegin(state, db.name, "up", hash, &interrupted);

  if (!db.dry && stored == NULL) {
    dbmWrite(why, room, TEXT`could not begin the state of ${db.name}: ${state->db->error}`);
    return -1;
  }

  db.record = dbmRecordFrom(stored);
  free(stored);

  int answer = execute(&db, migration, &interrupted, hash, why, room);

  /* a failure said with its statement leaves nothing for the walker */
  if (answer != 0 && db.op > 0)
    why[0] = '\0';

  yyjson_mut_doc_free(db.record);
  return answer == 0 ? 0 : -1;
}

int dbmRunJob(driver_t *driver, dbm_state_t *state,
              const dbm_migration_t *migration, dbm_job_t *job,
              bool *rolledBack, char *why, size_t room) {

  dml_t db = {.driver = driver, .state = state,
              .name = dbmKeyOf(migration->name), .dry = false,
              .transactional = !migration->noTransaction &&
                               !driver->noTransactions,
              .job = job};
  dbm_interrupted_t interrupted = {0};
  char hex[65];
  const char *hash = dbmHashOf(migration, hex);
  char *jobs = dbmJobsRead(state);
  char *stored = NULL;

  *rolledBack = false;

  if (jobs == NULL || dbmStateReloadSchema(state) ||
      dbmKvGet(state->db, state->table, db.name, &stored)) {
    free(jobs);
    dbmWrite(why, room, TEXT`${state->db->error}`);
    return -1;
  }

  /* how far an earlier run of it got, from the job */
  json_t read = meta_toJSON(jobs);
  json_t was = read.get("jobs").get(job->name);
  const char *h = strcmp(was.get("h").kind(), "string") == 0 ? was.get("h").text() : "";

  free(jobs);

  if (was.get("step").number() > 0) {
    interrupted.found = true;
    interrupted.step = was.get("step").number();
    interrupted.learned = was.get("learned").number();
    interrupted.done = was.get("done").number();
    interrupted.rollback = was.get("rb").number() == 1;
    interrupted.changed = h[0] != '\0' && hash != NULL && strcmp(h, hash) != 0;
  }

  read.release();
  db.record = dbmRecordFrom(stored);
  free(stored);

  if (!interrupted.found) {

    char changes[100];

    dbmWrite(changes, sizeof changes, TEXT`{"h":"${hash != NULL ? hash : ""}"}`);

    if (dbmJobUpdate(job, changes, why, room) || job->lost) {
      yyjson_mut_doc_free(db.record);
      return job->lost ? 1 : -1;
    }
  }

  dbmSay(stdout, TEXT`[INFO] [jobs] ${interrupted.found ? "continuing" : "running"} ${job->name}\n`);

  int answer = execute(&db, migration, &interrupted, hash, why, room);

  *rolledBack = db.rolledBack;
  yyjson_mut_doc_free(db.record);
  return answer;
}

/** A dml migration down: its record reverted, then forgotten. */
int dbmDownDml(driver_t *driver, dbm_state_t *state,
               const dbm_migration_t *migration, char *why, size_t room) {

  dml_t db = {.driver = driver, .state = state,
              .name = dbmKeyOf(migration->name), .dry = driver->dryRun,
              .transactional = !migration->noTransaction &&
                               !driver->noTransactions};
  char *stored = NULL;

  if (dbmKvGet(state->db, state->table, db.name, &stored)) {
    dbmWrite(why, room, TEXT`${state->db->error}`);
    return -1;
  }

  if (stored == NULL) {
    dbmWrite(why, room, TEXT`there is no state for ${db.name} in ${state->table} - it cannot be reverted from here`);
    return -1;
  }

  if (!db.dry) {

    char hex[65];
    char *begun = dbmStateBegin(state, db.name, "down",
                                dbmHashOf(migration, hex), NULL);

    if (begun == NULL) {
      free(stored);
      dbmWrite(why, room, TEXT`${state->db->error}`);
      return -1;
    }

    free(begun);
  }

  db.record = dbmRecordFrom(stored);
  free(stored);

  char irreversible[600];

  irreversibleSteps(&db, irreversible, sizeof irreversible);

  if (irreversible[0] != '\0') {
    if (!db.dry)
      dbmStateProgress(state, -1, 1);
    dbmWrite(why, room, TEXT`Migration "${db.name}" can not be reverted, it ran ${irreversible}, which can not be reverted.`);
    yyjson_mut_doc_free(db.record);
    return -1;
  }

  int answer = revert(&db);

  if (answer == 0 && !db.dry && dbmStateProgress(state, -1, 1))
    answer = failedState(&db);

  if (answer != 0)
    dbmWrite(why, room, TEXT`${db.error}`);

  yyjson_mut_doc_free(db.record);
  return answer != 0 ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* at the start of a release                                          */
/* ------------------------------------------------------------------ */

/**
 * The purges registered, with their age and what applies to them -
 * `{mark: {t, key, age, releases, drop}}` - those due only.
 */
static yyjson_mut_doc *duePurges(dml_t *self) {

  dbm_state_t *state = self->state;
  yyjson_mut_doc *due = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root = yyjson_mut_obj(due);
  char *row = NULL;

  yyjson_mut_doc_set_root(due, root);

  if (dbmKvGet(state->db, state->table, PURGES, &row) || row == NULL)
    return due;

  yyjson_mut_doc *registry = dbmRecordFrom(NULL);
  yyjson_doc *read = yyjson_read(row, strlen(row), 0);

  free(row);
  yyjson_mut_doc_set_root(registry, read != NULL
                                        ? yyjson_val_mut_copy(registry, yyjson_doc_get_root(read))
                                        : yyjson_mut_obj(registry));
  yyjson_doc_free(read);

  yyjson_mut_obj_iter walk;
  yyjson_mut_val *mark;

  yyjson_mut_obj_iter_init(rootOf(registry), &walk);

  while (!self->failed && (mark = yyjson_mut_obj_iter_next(&walk)) != NULL) {

    yyjson_mut_val *entry = yyjson_mut_obj_iter_get_val(mark);
    yyjson_mut_val *r = yyjson_mut_obj_get(entry, "r");
    long age = dbmReleaseAge(state, state->releaseCurrent,
                             r != NULL && yyjson_mut_is_str(r)
                                 ? yyjson_mut_get_str(r)
                                 : NULL);
    long releases;
    char drop[8];

    if (dbmReleaseSettings(state, yyjson_mut_obj_get(entry, "o"), &releases,
                           drop, self->error, sizeof self->error)) {
      self->failed = true;
      break;
    }

    if (age < releases)
      continue;

    yyjson_mut_val *item = yyjson_mut_val_mut_copy(due, entry);

    yyjson_mut_obj_put(item, yyjson_mut_str(due, "age"), yyjson_mut_sint(due, age));
    yyjson_mut_obj_put(item, yyjson_mut_str(due, "releases"), yyjson_mut_sint(due, releases));
    yyjson_mut_obj_put(item, yyjson_mut_strcpy(due, "drop"), yyjson_mut_strcpy(due, drop));
    yyjson_mut_obj_add(root, yyjson_mut_strcpy(due, yyjson_mut_get_str(mark)), item);
  }

  yyjson_mut_doc_free(registry);
  return due;
}

static bool forgettingMarks(yyjson_mut_doc *doc, yyjson_mut_val *value,
                            void *context) {

  yyjson_mut_val *marks = context;
  yyjson_mut_obj_iter walk;
  yyjson_mut_val *mark;

  (void)doc;
  yyjson_mut_obj_iter_init(marks, &walk);

  while ((mark = yyjson_mut_obj_iter_next(&walk)) != NULL)
    yyjson_mut_obj_remove_key(value, yyjson_mut_get_str(mark));

  return true;
}

/**
 * The rows deleted in soft mode whose purge with drop "auto" is due,
 * deleted for good - which makes the release irreversible - and the backups
 * whose drop "auto" is due, dropped.
 */
int dbmDmlReleaseStart(driver_t *driver, dbm_state_t *state, const char *label,
                       char *why, size_t room) {

  char name[300];

  dbmWrite(name, sizeof name, TEXT`${DBM_RELEASE_PREFIX}${label}`);

  dml_t self = {.driver = driver, .state = state, .name = name,
                .dry = driver->dryRun,
                .transactional = !driver->noTransactions};

  self.record = dbmRecordFrom(NULL);

  yyjson_mut_doc *due = duePurges(&self);
  yyjson_mut_val *marks = yyjson_mut_obj(due);
  yyjson_mut_obj_iter walk;
  yyjson_mut_val *mark;

  yyjson_mut_obj_iter_init(rootOf(due), &walk);

  while (!self.failed && (mark = yyjson_mut_obj_iter_next(&walk)) != NULL) {

    const char *text = yyjson_mut_get_str(mark);
    yyjson_mut_val *entry = yyjson_mut_obj_iter_get_val(mark);
    const char *table = textOf(yyjson_mut_obj_get(entry, "t"));

    if (strcmp(textOf(yyjson_mut_obj_get(entry, "drop")), "auto") != 0)
      continue;

    yyjson_mut_obj_add(marks, yyjson_mut_strcpy(due, text),
                       yyjson_mut_strcpy(due, table));
    dbmSay(stdout, TEXT`[INFO] [release] purging the rows of "${table}" deleted in soft mode by ${text + 5}\n`);

    if (self.dry)
      continue;

    statement_t condition;
    statement_t change;
    yyjson_mut_val *key = yyjson_mut_val_mut_copy(self.record, yyjson_mut_obj_get(entry, "key"));

    opened(&condition);
    markedBy(&self, &condition, text, false);
    opened(&change);
    change.sql.put("DELETE FROM ");
    quoted(&self, &change.sql, table);
    change.sql.put(" WHERE ");
    untilDone(&self, table, key, &condition, BATCH, &change);
    closed(&change);
    closed(&condition);
  }

  /* nothing to purge any more, and the release can not be reverted */
  if (!self.failed && !self.dry && yyjson_mut_obj_size(marks) > 0 &&
      changeState(&self, PURGES, forgettingMarks, marks) == 0) {

    char *row = NULL;

    if (dbmKvGet(state->db, state->table, name, &row)) {
      failedState(&self);
    } else {

      yyjson_mut_doc *record = dbmRecordFrom(row);
      yyjson_mut_val *steps = yyjson_mut_obj_get(rootOf(record), "s");

      yyjson_mut_obj_iter_init(marks, &walk);

      while ((mark = yyjson_mut_obj_iter_next(&walk)) != NULL) {

        yyjson_mut_val *purged = yyjson_mut_obj(record);
        yyjson_mut_val *args = yyjson_mut_arr(record);

        yyjson_mut_arr_add_strcpy(record, args, textOf(yyjson_mut_obj_iter_get_val(mark)));
        yyjson_mut_obj_add_int(record, purged, "t", 3);
        yyjson_mut_obj_add_str(record, purged, "a", "purge");
        yyjson_mut_obj_add_val(record, purged, "c", args);
        yyjson_mut_obj_add_int(record, purged, "n", 0);
        yyjson_mut_arr_append(steps, purged);
      }

      char *text = yyjson_mut_write(record, 0, NULL);
      int answer = text == NULL ||
                   (row != NULL ? dbmKvUpdate(state->db, state->table, name, text)
                                : dbmKvInsert(state->db, state->table, name, text));

      free(text);
      free(row);
      yyjson_mut_doc_free(record);

      if (answer)
        failedState(&self);
    }
  }

  yyjson_mut_doc_free(due);

  /* the backups of the migrations final now */
  if (!self.failed) {

    yyjson_mut_doc *groups = backupsOf(&self, false);
    yyjson_mut_val *key;

    yyjson_mut_obj_iter_init(rootOf(groups), &walk);

    while (!self.failed && (key = yyjson_mut_obj_iter_next(&walk)) != NULL) {
      yyjson_mut_val *group = yyjson_mut_obj_iter_get_val(key);
      if (strcmp(textOf(yyjson_mut_obj_get(group, "drop")), "auto") == 0)
        dropGroup(&self, yyjson_mut_get_str(key),
                  yyjson_mut_obj_get(group, "backups"));
    }

    yyjson_mut_doc_free(groups);
  }

  yyjson_mut_doc_free(self.record);

  if (self.failed) {
    dbmWrite(why, room, TEXT`${self.error}`);
    return -1;
  }

  return 0;
}

/** The purges and backups due with drop "manual", said, with what to do. */
void dbmDmlWarn(driver_t *driver, dbm_state_t *state) {

  dml_t self = {.driver = driver, .state = state, .name = "",
                .dry = driver->dryRun};

  self.record = dbmRecordFrom(NULL);

  yyjson_mut_doc *due = duePurges(&self);
  yyjson_mut_obj_iter walk;
  yyjson_mut_val *key;

  yyjson_mut_obj_iter_init(rootOf(due), &walk);

  while ((key = yyjson_mut_obj_iter_next(&walk)) != NULL) {

    const char *mark = yyjson_mut_get_str(key);
    yyjson_mut_val *entry = yyjson_mut_obj_iter_get_val(key);
    char migration[300];
    const char *hash = strrchr(mark, '#');

    if (strcmp(textOf(yyjson_mut_obj_get(entry, "drop")), "manual") != 0)
      continue;

    dbmWrite(migration, hash != NULL && (size_t)(hash - mark - 5) + 1 < sizeof migration
                            ? (size_t)(hash - mark - 5) + 1
                            : sizeof migration,
             TEXT`${mark + 5}`);
    dbmSay(stderr, TEXT`[WARN] [release] the rows of "${textOf(yyjson_mut_obj_get(entry, "t"))}" deleted in soft mode by ${mark + 5} are due for purging, deleted ${numberOf(entry, "age")} releases ago. Purge them in a dml migration with db.purge("${textOf(yyjson_mut_obj_get(entry, "t"))}", "${migration}").\n`);
  }

  yyjson_mut_doc_free(due);

  yyjson_mut_doc *groups = backupsOf(&self, false);

  yyjson_mut_obj_iter_init(rootOf(groups), &walk);

  while ((key = yyjson_mut_obj_iter_next(&walk)) != NULL) {
    yyjson_mut_val *group = yyjson_mut_obj_iter_get_val(key);
    if (strcmp(textOf(yyjson_mut_obj_get(group, "drop")), "manual") == 0)
      dbmSay(stderr, TEXT`[WARN] [release] the backups of ${yyjson_mut_get_str(key)} are due for dropping, it ran ${numberOf(group, "age")} releases ago. Drop them in a dml migration with db.dropBackups("${yyjson_mut_get_str(key)}"), it can not be reverted afterwards.\n`);
  }

  yyjson_mut_doc_free(groups);
  yyjson_mut_doc_free(self.record);

  if (self.failed)
    dbmSay(stderr, TEXT`[ERROR] ${self.error}\n`);
}
