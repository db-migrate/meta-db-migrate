/**
 * Background jobs: node db-migrate's lib/jobs.js, since 1.4.0.
 *
 * A dml migration with { background: true } is not run by up, it is
 * registered as a job; `work` runs the jobs. All of them live in one row of
 * the state table, changed by compare and swap only, so any number of
 * workers can work on them at once:
 *
 *   {"jobs":{"20261009000003-m3":{"step":0,"learned":0,"done":0,"rb":0,
 *     "s":"queued","blocking":false,"ID":0,"n":"3f9a0c1d2e4b5a67"}},
 *    "pause":{"ID":"<lock holder>","n":"..."},"n":"a1b2c3d4e5f60718"}
 *
 *   s          queued, running or failed - a done job is gone
 *   blocking   later jobs wait until this one is done
 *   ID, n      the worker running it, and what it renews with every write:
 *              a job whose n stays the same for the timeout is taken over
 *   step, learned, done, rb   how far it got, as the lock row says it of a
 *              migration in the foreground
 *   h, err     the hash of its file when it started, why it failed
 *
 * Migrations go first: whoever holds the migration lock pauses the jobs
 * before migrating - no job is taken any more, the running ones stop after
 * their batch and continue once the lock is given back.
 */
#include <db_migrate_driver.h>

#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>

static void nonce(char into[17]) {

  unsigned char bytes[8];

  if (getrandom(bytes, sizeof bytes, 0) != (ssize_t)sizeof bytes)
    for (size_t i = 0; i < sizeof bytes; ++i)
      bytes[i] = (unsigned char)rand();

  for (size_t i = 0; i < sizeof bytes; ++i)
    dbmWrite(into + 2 * i, 3, TEXT`${"0123456789abcdef"[bytes[i] >> 4]}${"0123456789abcdef"[bytes[i] & 15]}`);
}

long dbmNow(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

void dbmSleep(long ms) {
  struct timespec wait = {ms / 1000, (ms % 1000) * 1000000};
  while (nanosleep(&wait, &wait) != 0)
    ;
}

static const char *textOf(yyjson_mut_val *value) {
  return value != NULL && yyjson_mut_is_str(value) ? yyjson_mut_get_str(value)
                                                   : "";
}

/**
 * A field set where it is, or added at the end - as JS assigns. By hand,
 * because `object.key = value` takes a key that is there out and adds it at
 * the end, and node reads this document back and writes it again: its keys
 * stay where they are.
 */
static void put(yyjson_mut_doc *doc, yyjson_mut_val *object, const char *key,
                yyjson_mut_val *value) {
  if (!yyjson_mut_obj_replace(object, yyjson_mut_str(doc, key), value))
    yyjson_mut_obj_add(object, yyjson_mut_strcpy(doc, key), value);
}

/** The same on a json_t, with whatever value the place is given. */
static void assign(json_t object, const char *key, json_value_t value) {
  put(object._doc, object._node, key, jsonValueNode(object._doc, value));
}

/**
 * The jobs as they are stored, or `{"jobs":{}}`; nothing when reading failed.
 * Put right in yyjson, in the one document that is then handed over, rather
 * than a second one made for a row that is not what it should be.
 */
@owns static json_t readJobs(dbm_state_t *state, char **row) {

  *row = NULL;

  if (dbmKvGet(state->db, state->table, DBM_JOBS, row))
    return meta_jsonFromMut(NULL);

  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_doc *read = *row != NULL ? yyjson_read(*row, strlen(*row), 0) : NULL;
  yyjson_mut_val *root = read != NULL && yyjson_is_obj(yyjson_doc_get_root(read))
                             ? yyjson_val_mut_copy(doc, yyjson_doc_get_root(read))
                             : NULL;

  yyjson_doc_free(read);

  if (root == NULL) {
    root = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, root, "jobs", yyjson_mut_obj(doc));
  } else if (!yyjson_mut_is_obj(yyjson_mut_obj_get(root, "jobs"))) {
    put(doc, root, "jobs", yyjson_mut_obj(doc));
  }

  yyjson_mut_doc_set_root(doc, root);
  return meta_jsonFromMut(doc);
}

char *dbmJobsRead(dbm_state_t *state) {

  char *row = NULL;
  json_t doc = readJobs(state, &row);

  free(row);

  return doc.isNothing() ? NULL : doc.owned();
}

/** What changes the jobs here: the whole row, which `jobs` is a key of. */
typedef bool (*changing_t)(json_t doc, void *context);

/**
 * The jobs changed by `mutate`, tried again until no other worker wrote in
 * between - every write with a new `n`, so the swap can tell. `mutate`
 * answers false to leave them as they are.
 */
static int change(dbm_state_t *state, changing_t mutate, void *context,
                  char *why, size_t room) {

  for (int attempt = 0;; ++attempt) {

    char *row = NULL;
    json_t doc = readJobs(state, &row);

    /* by hand: what is handed to a pointer, meta takes as kept */
    defer doc.release();

    if (doc.isNothing()) {
      dbmWrite(why, room, TEXT`${state->db->error}`);
      return -1;
    }

    if (!mutate(doc, context)) {
      free(row);
      return 0;
    }

    char n[17];

    nonce(n);
    assign(doc, "n", n);

    char *value = doc.owned();

    /* inserted by another worker first: tried again with theirs */
    if (row == NULL)
      dbmKvInsert(state->db, state->table, DBM_JOBS, value);
    else
      dbmKvSwap(state->db, state->table, DBM_JOBS, value, row);

    free(row);

    char *after = NULL;
    bool written = dbmKvGet(state->db, state->table, DBM_JOBS, &after) == 0 &&
                   after != NULL && value != NULL && strcmp(after, value) == 0;

    free(after);
    free(value);

    if (written)
      return 0;

    if (attempt > 50) {
      dbmWrite(why, room, TEXT`[jobs] could not change the jobs, too much contention`);
      return -1;
    }

    dbmSleep(rand() % (20 * (attempt + 1 < 10 ? attempt + 1 : 10)));
  }
}

/** A mutation as the header has it, written against yyjson. */
typedef struct {
  dbm_jobs_mutation_t mutate;
  void *context;
} unwrapped_t;

static bool unwrapped(json_t doc, void *context) {
  unwrapped_t *it = context;
  return it->mutate(doc._doc, doc.jobs._node, doc._node, it->context);
}

int dbmJobsChange(dbm_state_t *state, dbm_jobs_mutation_t mutate,
                  void *context, char *why, size_t room) {
  unwrapped_t it = {mutate, context};
  return change(state, unwrapped, &it, why, room);
}

/* ------------------------------------------------------------------ */
/* registering                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
  const char *name;
  const char *scope;
  bool blocking;
} registering_t;

static bool registering(json_t doc, void *context) {

  registering_t *it = context;
  json_t job = doc.jobs[it->name];
  const char *const progress[] = {"step", "learned", "done", "rb"};
  char n[17];

  if (strcmp(job.kind(), "nothing") != 0 && strcmp(job.s.text(), "failed") != 0)
    return false;

  /* a failed one keeps how far it got and its hash; its err goes - built in
     yyjson where it is going, in the row's own document */
  yyjson_mut_doc *into = doc._doc;
  yyjson_mut_val *fresh = yyjson_mut_obj(into);

  for (size_t i = 0; i < countof(progress); ++i) {
    yyjson_mut_val *was = job[progress[i]]._node;
    yyjson_mut_obj_add_val(into, fresh, progress[i],
                           was != NULL ? yyjson_mut_val_mut_copy(into, was)
                                       : yyjson_mut_sint(into, 0));
  }

  for (field in job)
    if (!(field.keyOf() in {"step", "learned", "done", "rb", "err"}))
      yyjson_mut_obj_add(fresh, yyjson_mut_strcpy(into, field.keyOf()),
                         yyjson_mut_val_mut_copy(into, field._node));

  nonce(n);
  put(into, fresh, "s", yyjson_mut_str(into, "queued"));
  put(into, fresh, "blocking", yyjson_mut_bool(into, it->blocking));
  put(into, fresh, "ID", yyjson_mut_sint(into, 0));
  put(into, fresh, "n", yyjson_mut_strcpy(into, n));

  /* the scope it belongs to, as node 1.8.1 keeps it: none for the top level */
  if (it->scope != NULL && it->scope[0] != '\0')
    put(into, fresh, "sc", yyjson_mut_strcpy(into, it->scope));

  put(into, doc.jobs._node, it->name, fresh);
  return true;
}

int dbmJobsRegister(dbm_state_t *state, const char *name, const char *scope,
                    bool blocking, char *why, size_t room) {

  registering_t it = {name, scope, blocking};
  char *record = NULL;

  if (change(state, registering, &it, why, room))
    return -1;

  /* its record, where the steps it runs are kept */
  if (dbmKvGet(state->db, state->table, name, &record) ||
      (record == NULL && dbmKvInsert(state->db, state->table, name, "{}"))) {
    dbmWrite(why, room, TEXT`${state->db->error}`);
    return -1;
  }

  free(record);
  dbmSay(stdout, TEXT`[INFO] [jobs] ${name} runs in the background\n`);
  return 0;
}

const char *dbmJobScope(json_t job, const char *fallback) {

  json_t sc = job.sc;

  return strcmp(sc.kind(), "string") == 0 ? sc.text() : fallback;
}

static bool removing(json_t doc, void *context) {
  return doc.jobs.remove(context);
}

int dbmJobsRemove(dbm_state_t *state, const char *name, char *why,
                  size_t room) {
  return change(state, removing, (void *)name, why, room);
}

/* ------------------------------------------------------------------ */
/* pausing them for migrations                                        */
/* ------------------------------------------------------------------ */

static bool pausing(json_t doc, void *context) {

  /* in the row's own document, where it is going */
  yyjson_mut_val *pause = yyjson_mut_obj(doc._doc);
  char n[17];

  nonce(n);
  yyjson_mut_obj_add_strcpy(doc._doc, pause, "ID", context);
  yyjson_mut_obj_add_strcpy(doc._doc, pause, "n", n);
  put(doc._doc, doc._node, "pause", pause);
  return true;
}

/**
 * What node keeps per job while waiting: the last `n` seen, and since when.
 *
 * In yyjson rather than a json_t, because `seen` comes through the header as
 * a yyjson document, and a json_t is made only by taking a document over -
 * there is none that looks into one somebody else frees.
 */
static void seenAt(yyjson_mut_doc *seen, const char *name, const char *n,
                   long now, bool *changed, long *since) {

  yyjson_mut_val *root = yyjson_mut_doc_get_root(seen);
  yyjson_mut_val *last = yyjson_mut_obj_get(root, name);

  if (last == NULL || strcmp(textOf(yyjson_mut_obj_get(last, "n")), n) != 0) {
    yyjson_mut_val *entry = yyjson_mut_obj(seen);
    yyjson_mut_obj_add_strcpy(seen, entry, "n", n);
    yyjson_mut_obj_add_int(seen, entry, "since", now);
    yyjson_mut_obj_put(root, yyjson_mut_strcpy(seen, name), entry);
    *changed = true;
    *since = now;
    return;
  }

  *changed = false;
  *since = (long)yyjson_mut_get_sint(yyjson_mut_obj_get(last, "since"));
}

int dbmJobsPause(dbm_state_t *state, char *why, size_t room) {

  json_t seen = {};

  if (change(state, pausing, state->id, why, room))
    return -1;

  for (;;) {

    long now = dbmNow();
    char *row = NULL;
    json_t doc = readJobs(state, &row);

    free(row);

    if (doc.isNothing()) {
      dbmWrite(why, room, TEXT`${state->db->error}`);
      return -1;
    }

    dbm_text_t running = {0};
    int count = 0;

    for (job in doc.jobs) {

      bool changed;
      long since;

      if (strcmp(job.s.text(), "running") != 0 ||
          strcmp(job.ID.text(), state->id) == 0)
        continue;

      /* one whose worker does not answer is not waited for */
      seenAt(seen._doc, job.keyOf(), job.n.text(), now, &changed, &since);

      if (changed || now - since < state->timeoutMs) {
        running.append(TEXT`${count > 0 ? ", " : ""}${job.keyOf()}`);
        ++count;
      }
    }

    if (count == 0) {
      running.release();
      return 0;
    }

    dbmSay(stdout, TEXT`[INFO] [jobs] waiting for ${(long)count} background job(s) to pause: ${running.text}\n`);
    running.release();

    /* they stop after their current batch, usually soon */
    dbmSleep(state->intervalMs < 200 ? state->intervalMs : 200);
  }
}

static bool resuming(json_t doc, void *context) {

  json_t pause = doc.pause;

  if (strcmp(pause.kind(), "nothing") == 0 ||
      strcmp(pause.ID.text(), context) != 0)
    return false;

  doc.remove("pause");
  return true;
}

void dbmJobsResume(dbm_state_t *state) {

  char why[300];

  if (change(state, resuming, state->id, why, sizeof why))
    dbmSay(stderr, TEXT`[WARN] [jobs] could not resume the jobs: ${why}\n`);
}

/**
 * The pause, if a migration holds it: its holder holds the migration lock
 * and keeps the lock row alive - it did not stay the same for the lock
 * timeout, measured on this clock in `seen`.
 */
bool dbmJobsPaused(dbm_state_t *state, yyjson_mut_doc *seen) {

  char *row;
  char *lock = NULL;
  json_t doc = readJobs(state, &row);
  json_t pause = doc.pause;
  bool paused = false;

  free(row);

  if (strcmp(pause.kind(), "nothing") != 0 &&
      dbmKvGet(state->db, state->table, "__dbmigrate_state__", &lock) == 0 &&
      lock != NULL) {

    json_t row = meta_toJSON(lock);
    json_t holder = row.s.ID;

    if (strcmp(holder.kind(), "string") == 0 &&
        strcmp(holder.text(), pause.ID.text()) == 0) {

      bool changed;
      long since;
      long now = dbmNow();

      seenAt(seen, "\x01lock", lock, now, &changed, &since);
      paused = changed || now - since < state->timeoutMs;
    }
  }

  free(lock);
  return paused;
}

/* ------------------------------------------------------------------ */
/* taking one, and keeping it                                         */
/* ------------------------------------------------------------------ */

typedef struct {
  const char *id;
  yyjson_mut_doc *seen;
  long timeout;
  bool valid;
  bool wait;
  char *taken;
  size_t room;
  bool took;
} claiming_t;

static bool claiming(json_t doc, void *context) {

  claiming_t *it = context;
  json_t pause = doc.pause;
  bool paused = strcmp(pause.kind(), "nothing") != 0;
  yyjson_mut_val *seenRoot = yyjson_mut_doc_get_root(it->seen);
  yyjson_mut_val *invalid = yyjson_mut_obj_get(seenRoot, "\x01invalid");
  const char *pauseN = pause.n;
  long now = dbmNow();

  it->took = false;

  /* a pause found invalid before is ignored, one set in between holds */
  it->wait = paused &&
             (it->valid || invalid == NULL || strcmp(textOf(invalid), pauseN) != 0);

  if (paused && !it->valid && invalid == NULL)
    yyjson_mut_obj_add(seenRoot, yyjson_mut_str(it->seen, "\x01invalid"),
                       yyjson_mut_strcpy(it->seen, pauseN));

  if (it->wait)
    return false;

  /* in the order of their names */
  json_t jobs = doc.jobs;
  const char **names = calloc((size_t)jobs.count() + 1, sizeof(char *));
  size_t at = 0;

  if (names != NULL)
    for (job in jobs)
      names[at++] = job.keyOf();

  for (size_t i = 0; i < at; ++i)
    for (size_t j = i + 1; j < at; ++j)
      if (strcmp(names[j], names[i]) < 0) {
        const char *swap = names[i];
        names[i] = names[j];
        names[j] = swap;
      }

  bool changed = false;

  for (size_t i = 0; i < at; ++i) {

    json_t job = jobs[names[i]];
    const char *s = job.s;
    bool ours = strcmp(job.ID.text(), it->id) == 0;
    bool available = strcmp(s, "queued") == 0;

    if (strcmp(s, "running") == 0 && !ours) {

      bool fresh;
      long since;

      seenAt(it->seen, names[i], job.n.text(), now, &fresh, &since);

      if (!fresh && now - since >= it->timeout) {
        dbmSay(stderr, TEXT`[WARN] [jobs] taking over ${names[i]}, its worker did not respond for ${it->timeout} ms\n`);
        available = true;
      }
    }

    if (available) {
      char n[17];
      nonce(n);
      assign(job, "s", "running");
      assign(job, "ID", it->id);
      assign(job, "n", n);
      dbmWrite(it->taken, it->room, TEXT`${names[i]}`);
      it->took = true;
      changed = true;
      break;
    }

    if (job.blocking.truth())
      break;
  }

  free(names);
  return changed;
}

int dbmJobsClaim(dbm_state_t *state, const char *id, yyjson_mut_doc *seen,
                 long timeout, char *name, size_t room, char *why,
                 size_t whyRoom) {

  claiming_t it = {.id = id, .seen = seen, .timeout = timeout, .taken = name,
                   .room = room};

  it.valid = dbmJobsPaused(state, seen);

  if (change(state, claiming, &it, why, whyRoom))
    return -1;

  return it.wait ? 2 : it.took ? 1 : 0;
}

typedef struct {
  dbm_job_t *job;
  const char *changes;
  bool remove;
} updating_t;

static bool updating(json_t doc, void *context) {

  updating_t *it = context;
  json_t job = doc.jobs[it->job->name];

  if (strcmp(job.ID.text(), it->job->id) != 0) {
    it->job->lost = true;
    return false;
  }

  if (it->remove) {
    doc.jobs.remove(it->job->name);
    return true;
  }

  json_t changes = meta_toJSON(it->changes);

  for (field in changes)
    /* null takes a field away, as JS's undefined does in its JSON */
    if (strcmp(field.kind(), "null") == 0)
      job.remove(field.keyOf());
    else
      assign(job, field.keyOf(), field);

  char n[17];
  nonce(n);
  assign(job, "n", n);
  return true;
}

int dbmJobUpdate(dbm_job_t *job, const char *changes, char *why, size_t room) {
  updating_t it = {.job = job, .changes = changes};
  return change(job->state, updating, &it, why, room);
}

int dbmJobDone(dbm_job_t *job, char *why, size_t room) {
  updating_t it = {.job = job, .remove = true};
  return change(job->state, updating, &it, why, room);
}
