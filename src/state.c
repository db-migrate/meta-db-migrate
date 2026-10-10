/**
 * node db-migrate's state: the migration lock, the schema v2 migrations
 * built, and each v2 migration's own record - in node's table, in node's
 * rows, in node's JSON, so either of the two can carry on what the other
 * started and neither migrates while the other holds the lock.
 *
 * The lock is a row, `__dbmigrate_state__`, taken by compare and swap: every
 * write puts this process's ID, the date and a fresh nonce into it, and only
 * happens where the row still holds what this process last read. A heartbeat
 * writes while the lock is held, so a waiter can tell a long migration from a
 * dead process: a lock whose row has not changed for the timeout, measured on
 * the waiter's own clock, is taken over.
 *
 * Everything goes through a connection of its own. A v1 migration runs in a
 * transaction on the other one, and what the lock says has to survive that
 * transaction being rolled back.
 */
#include <db_migrate_driver.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>

static const char *LOCK = "__dbmigrate_state__";
static const char *SCHEMA = "__dbmigrate_schema__";

/* ------------------------------------------------------------------ */
/* small things                                                       */
/* ------------------------------------------------------------------ */

static long long millisecondsNow(clockid_t clock) {

  struct timespec now;

  clock_gettime(clock, &now);
  return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void sleepMs(long ms) {

  struct timespec span = {ms / 1000, (ms % 1000) * 1000000};

  while (nanosleep(&span, &span) != 0 && errno == EINTR)
    ;
}

/** What `JSON.stringify(new Date())` writes: 2026-10-08T15:29:33.702Z. */
static void isoDate(char *into, size_t room) {

  long long ms = millisecondsNow(CLOCK_REALTIME);
  time_t seconds = (time_t)(ms / 1000);
  struct tm utc;
  char head[32];

  gmtime_r(&seconds, &utc);
  strftime(head, sizeof head, "%Y-%m-%dT%H:%M:%S", &utc);
  dbmWrite(into, room, TEXT`${head}.${zeroed(num(ms % 1000), 3)}Z`);
}

/** 32 random bytes in base64, as node's `crypto.randomBytes(32)` names a process. */
static void processId(char *into) {

  static const char *alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  unsigned char bytes[33] = {0};
  size_t at = 0;

  if (getrandom(bytes, 32, 0) != 32)
    for (size_t i = 0; i < 32; ++i)
      bytes[i] = (unsigned char)rand();

  for (size_t i = 0; i < 33; i += 3) {

    unsigned value = (unsigned)bytes[i] << 16 | (unsigned)bytes[i + 1] << 8 |
                     bytes[i + 2];

    into[at++] = alphabet[(value >> 18) & 63];
    into[at++] = alphabet[(value >> 12) & 63];
    into[at++] = i + 1 < 32 ? alphabet[(value >> 6) & 63] : '=';
    into[at++] = i + 2 < 32 ? alphabet[value & 63] : '=';
  }

  into[at] = '\0';
}

/** Eight random bytes in hex, node's nonce. */
static void nonce(char *into) {

  unsigned char bytes[8];

  if (getrandom(bytes, sizeof bytes, 0) != sizeof bytes)
    for (size_t i = 0; i < sizeof bytes; ++i)
      bytes[i] = (unsigned char)rand();

  for (size_t i = 0; i < sizeof bytes; ++i)
    dbmWrite(into + i * 2, 3, TEXT`${zeroed(hex(bytes[i]), 2)}`);
}

/** Who holds the lock a row describes - its `s.ID` - or "" for nobody. */
static const char *holderOf(json_t row) {

  const char *kind = row.s.ID.kind();

  /* released is `0`, a number; held is the holder's ID */
  if (strcmp(kind, "string") != 0)
    return "";

  return row.s.ID.text();
}

/* ------------------------------------------------------------------ */
/* the lock row                                                       */
/* ------------------------------------------------------------------ */

/** `s[key] = value`, in its place if it is there, at the end if not. */
static void setField(yyjson_mut_doc *doc, yyjson_mut_val *s, const char *key,
                     yyjson_mut_val *value) {

  if (yyjson_mut_obj_get(s, key) != NULL)
    yyjson_mut_obj_replace(s, yyjson_mut_str(doc, key), value);
  else
    yyjson_mut_obj_add(s, yyjson_mut_strcpy(doc, key), value);
}

/**
 * The row rewritten with this process's ID, the date and a fresh nonce, and
 * the fields `changes` names - a JSON object, `{"step":3}` - set. Every other
 * field stays as it was, those this does not know included: the row is
 * node's as much as this program's, and what node keeps in it to resume an
 * interrupted migration must survive a write from here. `release` writes the
 * ID node writes when it lets go, `0`.
 */
static char *rewritten(dbm_state_t *self, const char *value,
                       const char *changes, bool release) {

  json_t old = meta_toJSON(value);
  defer old.release();
  json_t change = meta_toJSON(changes);
  defer change.release();

  char date[40];
  char n[17];

  isoDate(date, sizeof date);
  nonce(n);

  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root = yyjson_mut_obj(doc);
  yyjson_mut_val *s = strcmp(old.s.kind(), "object") == 0
                          ? yyjson_mut_val_mut_copy(doc, old.s.node)
                          : yyjson_mut_obj(doc);

  /* node's first fields, in node's order, for a row that has none */
  if (yyjson_mut_obj_get(s, "step") == NULL)
    setField(doc, s, "step", yyjson_mut_int(doc, 0));

  if (yyjson_mut_obj_get(s, "fin") == NULL)
    setField(doc, s, "fin", yyjson_mut_int(doc, 0));

  for (int i = 0; i < change.count(); ++i)
    setField(doc, s, change.keyAt(i),
             yyjson_mut_val_mut_copy(doc, change.get(change.keyAt(i)).node));

  setField(doc, s, "ID", release ? yyjson_mut_int(doc, 0)
                                 : yyjson_mut_strcpy(doc, self->id));
  setField(doc, s, "date", yyjson_mut_strcpy(doc, date));
  setField(doc, s, "n", yyjson_mut_strcpy(doc, n));
  yyjson_mut_obj_add_val(doc, root, "s", s);
  yyjson_mut_doc_set_root(doc, root);

  char *text = yyjson_mut_write(doc, 0, NULL);

  yyjson_mut_doc_free(doc);
  return text;
}

static void stopHeartbeat(dbm_state_t *self);

/**
 * One write of the lock row while holding it: a swap against the row as this
 * process last saw it, then a read to see whose write won. Losing means
 * somebody took the lock over - this process is no longer the owner, and says
 * so rather than carrying on as if it were.
 *
 * Holds `writing`: the heartbeat and the walker write the same row.
 */
static int writeLock(dbm_state_t *self, const char *changes, bool release) {

  pthread_mutex_lock(&self->writing);

  int answer = 0;

  if (!self->owner) {
    answer = dbmFail(self->db, TEXT`the migration lock is not held by this process`);
  } else {

    char *value = rewritten(self, self->current, changes, release);
    char *read = NULL;

    if (value == NULL ||
        dbmKvSwap(self->db, self->table, LOCK, value, self->current) ||
        dbmKvGet(self->db, self->table, LOCK, &read)) {
      answer = -1;
    } else if (read == NULL || strcmp(read, value) != 0) {
      self->owner = false;
      answer = dbmFail(self->db, TEXT`lost the migration lock to another process`);
    } else {
      free(self->current);
      self->current = read;
      read = NULL;
    }

    free(value);
    free(read);
  }

  pthread_mutex_unlock(&self->writing);
  return answer;
}

/**
 * Writes every third of the timeout, so a waiter that sees the row unchanged
 * for the whole timeout knows the holder is gone rather than busy.
 */
static void *beat(void *raw) {

  dbm_state_t *self = raw;
  long every = self->timeoutMs / 3 > 0 ? self->timeoutMs / 3 : 1;

  pthread_mutex_lock(&self->writing);

  while (!self->stopping) {

    struct timespec until;

    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += every / 1000;
    until.tv_nsec += (every % 1000) * 1000000;

    if (until.tv_nsec >= 1000000000) {
      until.tv_sec += 1;
      until.tv_nsec -= 1000000000;
    }

    pthread_cond_timedwait(&self->wake, &self->writing, &until);

    if (self->stopping)
      break;

    pthread_mutex_unlock(&self->writing);

    if (writeLock(self, "{}", false))
      dbmSay(stderr, TEXT`[ERROR] [state] ${self->db->error}\n`);

    pthread_mutex_lock(&self->writing);
  }

  pthread_mutex_unlock(&self->writing);
  return NULL;
}

static void stopHeartbeat(dbm_state_t *self) {

  if (!self->beating)
    return;

  pthread_mutex_lock(&self->writing);
  self->stopping = true;
  pthread_cond_signal(&self->wake);
  pthread_mutex_unlock(&self->writing);

  pthread_join(self->heartbeat, NULL);
  self->beating = false;
  self->stopping = false;
}

/** The row says this process holds the lock: so it does, from now on. */
static bool claim(dbm_state_t *self, char *row) {

  if (row == NULL)
    return false;

  json_t parsed = meta_toJSON(row);
  bool mine = strcmp(holderOf(parsed), self->id) == 0;

  parsed.release();

  if (!mine) {
    free(row);
    return false;
  }

  self->active = true;
  self->owner = true;
  free(self->current);
  self->current = row;

  if (pthread_create(&self->heartbeat, NULL, beat, self) == 0)
    self->beating = true;

  return true;
}

/**
 * One try at the lock: 1 when this process holds it afterwards, 0 when
 * another one does, -1 when asking failed. A row another process holds is
 * taken over only when it is exactly the one `stale` names - the row
 * waitForRelease saw unchanged for the whole timeout.
 */
static int acquire(dbm_state_t *self, const char *stale, int retries) {

  char *row = NULL;

  if (self->owner)
    return 1;

  if (dbmKvGet(self->db, self->table, LOCK, &row))
    return -1;

  if (row == NULL) {

    char date[40];
    char n[17];

    isoDate(date, sizeof date);
    nonce(n);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_val *s = yyjson_mut_obj(doc);

    yyjson_mut_obj_add_int(doc, s, "step", 0);
    yyjson_mut_obj_add_int(doc, s, "fin", 1);
    yyjson_mut_obj_add_strcpy(doc, s, "ID", self->id);
    yyjson_mut_obj_add_strcpy(doc, s, "date", date);
    yyjson_mut_obj_add_strcpy(doc, s, "n", n);
    yyjson_mut_obj_add_val(doc, root, "s", s);
    yyjson_mut_doc_set_root(doc, root);

    char *value = yyjson_mut_write(doc, 0, NULL);

    yyjson_mut_doc_free(doc);

    /* somebody else may have inserted first; the read decides */
    dbmKvInsert(self->db, self->table, LOCK, value);
    free(value);

    if (dbmKvGet(self->db, self->table, LOCK, &row))
      return -1;

    return claim(self, row) ? 1 : 0;
  }

  json_t parsed = meta_toJSON(row);
  const char *holder = holderOf(parsed);
  bool other = holder[0] != '\0' && strcmp(holder, self->id) != 0;
  long fin = parsed.s.fin;
  long step = parsed.s.step;
  const char *since = parsed.s.date;
  char when[64];

  dbmWrite(when, sizeof when, TEXT`${since}`);
  parsed.release();

  if (other) {

    if (stale == NULL || strcmp(stale, row) != 0) {
      free(row);
      return 0;
    }

    if (fin == 0)
      dbmSay(stderr, TEXT`[WARN] [state] taking over a stale migration lock from ${when}, the previous run was interrupted during a migration at step ${step}\n`);
    else
      dbmSay(stderr, TEXT`[WARN] [state] taking over a stale migration lock from ${when}\n`);
  }

  char *value = rewritten(self, row, "{}", false);
  char *after = NULL;

  int swapped = dbmKvSwap(self->db, self->table, LOCK, value, row);

  free(value);
  free(row);

  if (swapped || dbmKvGet(self->db, self->table, LOCK, &after))
    return -1;

  /* claim takes `after` either way */
  char *kept = after != NULL ? strdup(after) : NULL;

  if (claim(self, after)) {
    free(kept);
    return 1;
  }

  /**
   * Nobody holds it and still the swap did not apply: another process took
   * and gave it back in between - or the driver cannot compare and swap at
   * all, which must not end in a loop that never stops.
   */
  bool free_ = false;

  if (kept != NULL) {
    json_t now = meta_toJSON(kept);
    free_ = holderOf(now)[0] == '\0';
    now.release();
  }

  free(kept);

  if (free_) {

    if (retries > 0)
      return acquire(self, NULL, retries - 1);

    dbmFail(self->db, TEXT`could not take the migration lock although it is free - the driver does not seem to compare and swap`);
    return -1;
  }

  return 0;
}

/**
 * Until the lock is let go, or its row has not changed for the timeout on
 * this process's own clock - then its holder is gone, and the row is handed
 * back as the one to take over. NULL when it was released properly.
 */
static char *waitForRelease(dbm_state_t *self) {

  char *seen = NULL;
  long long since = 0;

  for (;;) {

    char *row = NULL;

    if (dbmKvGet(self->db, self->table, LOCK, &row)) {
      free(seen);
      return NULL;
    }

    if (row == NULL) {
      free(seen);
      return NULL;
    }

    json_t parsed = meta_toJSON(row);
    const char *holder = holderOf(parsed);
    bool other = holder[0] != '\0' && strcmp(holder, self->id) != 0;

    parsed.release();

    if (!other) {
      free(row);
      free(seen);
      return NULL;
    }

    long long now = millisecondsNow(CLOCK_MONOTONIC);

    if (seen == NULL || strcmp(seen, row) != 0) {
      free(seen);
      seen = row;
      since = now;
    } else {
      free(row);

      if (now - since >= self->timeoutMs)
        return seen;
    }

    sleepMs(self->intervalMs);
  }
}

bool dbmStateLock(dbm_state_t *self) {

  char *stale = NULL;

  for (;;) {

    int got = acquire(self, stale, 3);

    free(stale);
    stale = NULL;

    if (got == 1)
      return true;

    if (got < 0) {
      dbmSay(stderr, TEXT`[ERROR] [state] ${self->db->error}\n`);
      return false;
    }

    dbmSay(stdout, TEXT`[INFO] Waiting for the migration lock of another process\n`);
    stale = waitForRelease(self);
  }
}

void dbmStateUnlock(dbm_state_t *self) {

  stopHeartbeat(self);

  if (self->owner && writeLock(self, "{}", true))
    dbmSay(stderr, TEXT`[WARN] [state] ${self->db->error}\n`);

  self->active = false;
  self->owner = false;
  free(self->current);
  self->current = NULL;
}

/**
 * How far the running migration got. Holding the lock, a swap that fails if
 * the lock was lost; without one - a driver or a run that does not lock -
 * the row is written as it is.
 */
int dbmStateMark(dbm_state_t *self, const char *changes) {

  if (self->active)
    return writeLock(self, changes, false);

  char *row = NULL;

  if (dbmKvGet(self->db, self->table, LOCK, &row))
    return -1;

  char *value = rewritten(self, row != NULL ? row : "{\"s\":{\"step\":0,\"fin\":0,\"ID\":0}}",
                          changes, true);
  int answer = row == NULL ? dbmKvInsert(self->db, self->table, LOCK, value)
                           : dbmKvUpdate(self->db, self->table, LOCK, value);

  free(value);
  free(row);
  return answer;
}

int dbmStateProgress(dbm_state_t *self, int step, int fin) {

  dbm_text_t changes = {0};
  defer changes.release();

  changes.put("{");

  if (step >= 0)
    changes.append(TEXT`"step":${(long)step}`);

  if (fin >= 0)
    changes.append(TEXT`${step >= 0 ? "," : ""}"fin":${(long)fin}`);

  /* a migration that ends is not rolling back any more, as node has it */
  if (fin == 1)
    changes.put(",\"rb\":0");

  changes.put("}");

  if (changes.failed)
    return dbmFail(self->db, TEXT`out of memory writing the lock row`);

  return dbmStateMark(self, changes.text);
}

/* ------------------------------------------------------------------ */
/* the schema, and one migration's record                             */
/* ------------------------------------------------------------------ */

/** `{i, c, f, e}` with whatever of it the stored text has. */
static void *schemaFrom(const char *text) {

  yyjson_doc *read = yyjson_read(text, strlen(text), 0);
  yyjson_mut_doc *doc = read != NULL ? yyjson_doc_mut_copy(read, NULL)
                                     : yyjson_mut_doc_new(NULL);

  yyjson_doc_free(read);

  yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);

  if (root == NULL || !yyjson_mut_is_obj(root)) {
    root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
  }

  const char *const parts[] = {"i", "c", "f", "e"};

  for (size_t i = 0; i < countof(parts); ++i)
    if (yyjson_mut_obj_get(root, parts[i]) == NULL)
      yyjson_mut_obj_add_val(doc, root, parts[i], yyjson_mut_obj(doc));

  return doc;
}

int dbmStateReloadSchema(dbm_state_t *self) {

  char *text = NULL;

  if (dbmKvGet(self->db, self->table, SCHEMA, &text))
    return -1;

  if (text != NULL) {
    yyjson_mut_doc_free(self->schema);
    self->schema = schemaFrom(text);
    free(text);
  }

  return 0;
}

dbm_state_t *dbmStateOpen(driver_t *db, const char *table, long timeoutMs,
                          long intervalMs, bool dry) {

  dbm_state_t *self = calloc(1, sizeof(dbm_state_t));

  if (self == NULL)
    return NULL;

  self->db = db;
  self->table = table;
  self->timeoutMs = timeoutMs > 0 ? timeoutMs : 60000;
  self->intervalMs = intervalMs > 0 ? intervalMs : 1000;
  processId(self->id);
  pthread_mutex_init(&self->writing, NULL);
  pthread_mutex_init(&self->statements, NULL);
  pthread_cond_init(&self->wake, NULL);
  db->serial = &self->statements;

  /**
   * A dry run reads what there is and writes nothing - no table, no row -
   * and learns against an empty schema where there is none yet.
   */
  if (dry) {

    char *text = NULL;

    dbmKvGet(db, table, SCHEMA, &text);
    self->schema = schemaFrom(text != NULL ? text : "{}");
    free(text);
    return self;
  }

  /**
   * Two processes starting on an empty database can both try to make the
   * table, and PostgreSQL then trips over its own catalog. The second try
   * finds the table there.
   */
  if (dbmKvCreate(db, table) && dbmKvCreate(db, table)) {
    dbmStateClose(self);
    return NULL;
  }

  char *text = NULL;

  if (dbmKvGet(db, table, SCHEMA, &text)) {
    dbmStateClose(self);
    return NULL;
  }

  if (text == NULL && dbmKvInsert(db, table, SCHEMA, "{}") &&
      dbmKvGet(db, table, SCHEMA, &text)) {
    dbmStateClose(self);
    return NULL;
  }

  self->schema = schemaFrom(text != NULL ? text : "{}");
  free(text);

  return self;
}

void dbmStateClose(dbm_state_t *self) {

  if (self == NULL)
    return;

  dbmStateUnlock(self);
  yyjson_mut_doc_free(self->schema);

  /* the connection outlives this, closed after it */
  if (self->db->serial == &self->statements)
    self->db->serial = NULL;

  pthread_mutex_destroy(&self->statements);
  pthread_mutex_destroy(&self->writing);
  pthread_cond_destroy(&self->wake);
  free(self);
}

/**
 * Starting a v2 migration, as node's startMigration does it: the record,
 * made if there is none - `{}`, as node makes it - and the lock row saying
 * which migration runs which way, from step 0. What the row said before is
 * looked at first: a migration that did not finish left `fin` at 0 and its
 * name in `f`. If that is this one going up again and the caller can
 * resume, it is described rather than started over.
 */
char *dbmStateBegin(dbm_state_t *self, const char *key, const char *op,
                    const char *hash, dbm_interrupted_t *interrupted) {

  char *record = NULL;
  char *row = NULL;

  if (interrupted != NULL)
    memset(interrupted, 0, sizeof *interrupted);

  if (dbmKvGet(self->db, self->table, key, &record))
    return NULL;

  /* the row as this process holds it - the heartbeat replaces it, so under its lock */
  if (self->active) {
    pthread_mutex_lock(&self->writing);
    row = self->current != NULL ? strdup(self->current) : NULL;
    pthread_mutex_unlock(&self->writing);
  } else if (dbmKvGet(self->db, self->table, LOCK, &row)) {
    free(record);
    return NULL;
  }

  json_t lock = meta_toJSON(row != NULL ? row : "{}");
  defer lock.release();
  free(row);

  json_t was = lock.s;
  const char *running = was.f;
  bool unfinished = strcmp(was.fin.kind(), "number") == 0 &&
                    was.fin.number() == 0 && running[0] != '\0';

  if (unfinished) {

    const char *wasHash = was.h;

    if (interrupted != NULL && strcmp(running, key) == 0 &&
        strcmp(was.o.text(), "up") == 0 && strcmp(op, "up") == 0) {

      interrupted->found = true;
      interrupted->step = was.step.number();
      interrupted->learned = was.learned.number();
      interrupted->done = was.done.number();
      interrupted->rollback = was.rb.number() == 1;
      interrupted->changed = wasHash[0] != '\0' && hash != NULL &&
                             strcmp(wasHash, hash) != 0;

      if (record == NULL) {
        if (dbmKvInsert(self->db, self->table, key, "{}"))
          return NULL;
        record = strdup("{}");
      }

      return record;
    }

    dbmSay(stderr, TEXT`[WARN] [state] ignoring the interrupted ${was.o.text()} of migration "${running}" at step ${was.step.number()}\n`);
  }

  dbm_text_t changes = {0};
  defer changes.release();

  changes.append(TEXT`{"step":0,"fin":0,"f":"${key}","o":"${op}","learned":0,"done":0,"rb":0,"h":`);

  if (hash != NULL)
    changes.append(TEXT`"${hash}"}`);
  else
    changes.put("null}");

  if (changes.failed || dbmStateMark(self, changes.text)) {
    free(record);
    return NULL;
  }

  if (record == NULL) {

    if (dbmKvInsert(self->db, self->table, key, "{}"))
      return NULL;

    record = strdup("{}");
  }

  return record;
}

/** The schema and one migration's record, written after every step, as node does. */
int dbmStateSave(dbm_state_t *self, const char *key, const char *migration) {

  char *schema = yyjson_mut_write(self->schema, 0, NULL);
  int answer = schema == NULL ||
               dbmKvUpdate(self->db, self->table, SCHEMA, schema) ||
               dbmKvUpdate(self->db, self->table, key, migration);

  free(schema);
  return answer ? -1 : 0;
}

int dbmStateForget(dbm_state_t *self, const char *key) {
  return dbmKvDelete(self->db, self->table, key);
}

void dbmStateForgetSchema(dbm_state_t *self) {
  yyjson_mut_doc_free(self->schema);
  self->schema = schemaFrom("{}");
}

int dbmStateBackup(dbm_state_t *self) {

  char *schema = NULL;
  char name[300];
  char file[320];

  if (dbmKvGet(self->db, self->table, SCHEMA, &schema))
    return -1;

  if (schema == NULL)
    return 0;

  dbmWrite(name, sizeof name, TEXT`${self->table}_b_${(long)time(NULL)}`);
  dbmWrite(file, sizeof file, TEXT`${name}.dbmigrate`);

  FILE *out = fopen(file, "w");

  if (out == NULL) {
    free(schema);
    return dbmFail(self->db, TEXT`could not write ${file}`);
  }

  /* the row as node writes it: key, value */
  dbmSay(out, TEXT`{"key":"${SCHEMA}","value":`);

  char *quoted;

  /* the schema is text inside the row, so it is written as a JSON string */
  {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *value = yyjson_mut_strcpy(doc, schema);

    yyjson_mut_doc_set_root(doc, value);
    quoted = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
  }

  fputs(quoted != NULL ? quoted : "\"\"", out);
  fputs("}", out);
  fclose(out);
  free(quoted);
  free(schema);

  dbmSay(stdout, TEXT`[INFO] [state] Created a backup of ${self->table} by writing to file ${file}\n`);

  if (self->db->renameTable(self->db, self->table, name) ||
      dbmKvCreate(self->db, self->table) ||
      dbmKvInsert(self->db, self->table, SCHEMA, "{}"))
    return -1;

  dbmSay(stdout, TEXT`[INFO] [state] Created a backup of ${self->table} by renaming table to ${name}\n`);
  return 0;
}
