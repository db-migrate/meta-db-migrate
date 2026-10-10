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

static yyjson_mut_val *rootOf(yyjson_mut_doc *doc) {
  return yyjson_mut_doc_get_root(doc);
}

static const char *textOf(yyjson_mut_val *value) {
  return value != NULL && yyjson_mut_is_str(value) ? yyjson_mut_get_str(value)
                                                   : "";
}

/** A field set where it is, or added at the end - as JS assigns. */
static void assign(yyjson_mut_doc *doc, yyjson_mut_val *object,
                   const char *key, yyjson_mut_val *value) {
  if (!yyjson_mut_obj_replace(object, yyjson_mut_str(doc, key), value))
    yyjson_mut_obj_add(object, yyjson_mut_strcpy(doc, key), value);
}

/** The jobs as they are stored, or `{"jobs":{}}`; NULL when reading failed. */
static yyjson_mut_doc *readJobs(dbm_state_t *state, char **row) {

  *row = NULL;

  if (dbmKvGet(state->db, state->table, DBM_JOBS, row))
    return NULL;

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
    assign(doc, root, "jobs", yyjson_mut_obj(doc));
  }

  yyjson_mut_doc_set_root(doc, root);
  return doc;
}

char *dbmJobsRead(dbm_state_t *state) {

  char *row = NULL;
  yyjson_mut_doc *doc = readJobs(state, &row);

  free(row);

  if (doc == NULL)
    return NULL;

  char *text = yyjson_mut_write(doc, 0, NULL);
  yyjson_mut_doc_free(doc);
  return text;
}

/**
 * The jobs changed by `mutate`, tried again until no other worker wrote in
 * between - every write with a new `n`, so the swap can tell. `mutate`
 * answers false to leave them as they are.
 */
int dbmJobsChange(dbm_state_t *state, dbm_jobs_mutation_t mutate,
                  void *context, char *why, size_t room) {

  for (int attempt = 0;; ++attempt) {

    char *row = NULL;
    yyjson_mut_doc *doc = readJobs(state, &row);

    if (doc == NULL) {
      dbmWrite(why, room, TEXT`${state->db->error}`);
      return -1;
    }

    yyjson_mut_val *root = rootOf(doc);

    if (!mutate(doc, yyjson_mut_obj_get(root, "jobs"), root, context)) {
      yyjson_mut_doc_free(doc);
      free(row);
      return 0;
    }

    char n[17];

    nonce(n);
    assign(doc, root, "n", yyjson_mut_strcpy(doc, n));

    char *value = yyjson_mut_write(doc, 0, NULL);

    yyjson_mut_doc_free(doc);

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

/* ------------------------------------------------------------------ */
/* registering                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
  const char *name;
  bool blocking;
} registering_t;

static bool registering(yyjson_mut_doc *doc, yyjson_mut_val *jobs,
                        yyjson_mut_val *root, void *context) {

  registering_t *it = context;
  yyjson_mut_val *job = yyjson_mut_obj_get(jobs, it->name);
  const char *const progress[] = {"step", "learned", "done", "rb"};
  char n[17];

  (void)root;

  if (job != NULL && strcmp(textOf(yyjson_mut_obj_get(job, "s")), "failed") != 0)
    return false;

  /* a failed one keeps how far it got and its hash; its err goes */
  yyjson_mut_val *fresh = yyjson_mut_obj(doc);

  for (size_t i = 0; i < countof(progress); ++i) {
    yyjson_mut_val *was = yyjson_mut_obj_get(job, progress[i]);
    yyjson_mut_obj_add_val(doc, fresh, progress[i],
                           was != NULL ? yyjson_mut_val_mut_copy(doc, was)
                                       : yyjson_mut_sint(doc, 0));
  }

  yyjson_mut_obj_iter walk;
  yyjson_mut_val *key;

  yyjson_mut_obj_iter_init(job, &walk);

  while (job != NULL && (key = yyjson_mut_obj_iter_next(&walk)) != NULL) {
    const char *name = yyjson_mut_get_str(key);
    if (!(name in {"step", "learned", "done", "rb", "err"}))
      yyjson_mut_obj_add(fresh, yyjson_mut_strcpy(doc, name),
                         yyjson_mut_val_mut_copy(doc, yyjson_mut_obj_iter_get_val(key)));
  }

  nonce(n);
  assign(doc, fresh, "s", yyjson_mut_str(doc, "queued"));
  assign(doc, fresh, "blocking", yyjson_mut_bool(doc, it->blocking));
  assign(doc, fresh, "ID", yyjson_mut_sint(doc, 0));
  assign(doc, fresh, "n", yyjson_mut_strcpy(doc, n));

  if (job != NULL)
    yyjson_mut_obj_replace(jobs, yyjson_mut_str(doc, it->name), fresh);
  else
    yyjson_mut_obj_add(jobs, yyjson_mut_strcpy(doc, it->name), fresh);

  return true;
}

int dbmJobsRegister(dbm_state_t *state, const char *name, bool blocking,
                    char *why, size_t room) {

  registering_t it = {name, blocking};
  char *record = NULL;

  if (dbmJobsChange(state, registering, &it, why, room))
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

static bool removing(yyjson_mut_doc *doc, yyjson_mut_val *jobs,
                     yyjson_mut_val *root, void *context) {
  (void)doc;
  (void)root;
  return yyjson_mut_obj_remove_key(jobs, context) != NULL;
}

int dbmJobsRemove(dbm_state_t *state, const char *name, char *why,
                  size_t room) {
  return dbmJobsChange(state, removing, (void *)name, why, room);
}

/* ------------------------------------------------------------------ */
/* pausing them for migrations                                        */
/* ------------------------------------------------------------------ */

static bool pausing(yyjson_mut_doc *doc, yyjson_mut_val *jobs,
                    yyjson_mut_val *root, void *context) {

  yyjson_mut_val *pause = yyjson_mut_obj(doc);
  char n[17];

  (void)jobs;
  nonce(n);
  yyjson_mut_obj_add_strcpy(doc, pause, "ID", context);
  yyjson_mut_obj_add_strcpy(doc, pause, "n", n);
  assign(doc, root, "pause", pause);
  return true;
}

/** What node keeps per job while waiting: the last `n` seen, and since when. */
static void seenAt(yyjson_mut_doc *seen, const char *name, const char *n,
                   long now, bool *changed, long *since) {

  yyjson_mut_val *root = rootOf(seen);
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

  yyjson_mut_doc *seen = yyjson_mut_doc_new(NULL);

  yyjson_mut_doc_set_root(seen, yyjson_mut_obj(seen));

  if (dbmJobsChange(state, pausing, state->id, why, room)) {
    yyjson_mut_doc_free(seen);
    return -1;
  }

  for (;;) {

    long now = dbmNow();
    char *row = NULL;
    yyjson_mut_doc *doc = readJobs(state, &row);

    free(row);

    if (doc == NULL) {
      dbmWrite(why, room, TEXT`${state->db->error}`);
      yyjson_mut_doc_free(seen);
      return -1;
    }

    dbm_text_t running = {0};
    int count = 0;
    yyjson_mut_obj_iter walk;
    yyjson_mut_val *key;

    yyjson_mut_obj_iter_init(yyjson_mut_obj_get(rootOf(doc), "jobs"), &walk);

    while ((key = yyjson_mut_obj_iter_next(&walk)) != NULL) {

      yyjson_mut_val *job = yyjson_mut_obj_iter_get_val(key);
      yyjson_mut_val *id = yyjson_mut_obj_get(job, "ID");
      bool changed;
      long since;

      if (strcmp(textOf(yyjson_mut_obj_get(job, "s")), "running") != 0 ||
          (yyjson_mut_is_str(id) && strcmp(yyjson_mut_get_str(id), state->id) == 0))
        continue;

      /* one whose worker does not answer is not waited for */
      seenAt(seen, yyjson_mut_get_str(key), textOf(yyjson_mut_obj_get(job, "n")),
             now, &changed, &since);

      if (changed || now - since < state->timeoutMs) {
        running.append(TEXT`${count > 0 ? ", " : ""}${yyjson_mut_get_str(key)}`);
        ++count;
      }
    }

    yyjson_mut_doc_free(doc);

    if (count == 0) {
      running.release();
      yyjson_mut_doc_free(seen);
      return 0;
    }

    dbmSay(stdout, TEXT`[INFO] [jobs] waiting for ${(long)count} background job(s) to pause: ${running.text}\n`);
    running.release();

    /* they stop after their current batch, usually soon */
    dbmSleep(state->intervalMs < 200 ? state->intervalMs : 200);
  }
}

static bool resuming(yyjson_mut_doc *doc, yyjson_mut_val *jobs,
                     yyjson_mut_val *root, void *context) {

  yyjson_mut_val *pause = yyjson_mut_obj_get(root, "pause");

  (void)doc;
  (void)jobs;

  if (pause == NULL || strcmp(textOf(yyjson_mut_obj_get(pause, "ID")), context) != 0)
    return false;

  yyjson_mut_obj_remove_key(root, "pause");
  return true;
}

void dbmJobsResume(dbm_state_t *state) {

  char why[300];

  if (dbmJobsChange(state, resuming, state->id, why, sizeof why))
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
  yyjson_mut_doc *doc = readJobs(state, &row);
  bool paused = false;

  free(row);

  yyjson_mut_val *pause = doc != NULL ? yyjson_mut_obj_get(rootOf(doc), "pause")
                                      : NULL;

  if (pause != NULL &&
      dbmKvGet(state->db, state->table, "__dbmigrate_state__", &lock) == 0 &&
      lock != NULL) {

    json_t row = meta_toJSON(lock);
    json_t holder = row.get("s").get("ID");

    if (strcmp(holder.kind(), "string") == 0 &&
        strcmp(holder.text(), textOf(yyjson_mut_obj_get(pause, "ID"))) == 0) {

      bool changed;
      long since;
      long now = dbmNow();

      seenAt(seen, "\x01lock", lock, now, &changed, &since);
      paused = changed || now - since < state->timeoutMs;
    }

    row.release();
  }

  free(lock);
  yyjson_mut_doc_free(doc);
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

static bool claiming(yyjson_mut_doc *doc, yyjson_mut_val *jobs,
                     yyjson_mut_val *root, void *context) {

  claiming_t *it = context;
  yyjson_mut_val *pause = yyjson_mut_obj_get(root, "pause");
  yyjson_mut_val *seenRoot = rootOf(it->seen);
  yyjson_mut_val *invalid = yyjson_mut_obj_get(seenRoot, "\x01invalid");
  const char *pauseN = textOf(yyjson_mut_obj_get(pause, "n"));
  long now = dbmNow();

  it->took = false;

  /* a pause found invalid before is ignored, one set in between holds */
  it->wait = pause != NULL &&
             (it->valid || invalid == NULL || strcmp(textOf(invalid), pauseN) != 0);

  if (pause != NULL && !it->valid && invalid == NULL)
    yyjson_mut_obj_add(seenRoot, yyjson_mut_str(it->seen, "\x01invalid"),
                       yyjson_mut_strcpy(it->seen, pauseN));

  if (it->wait)
    return false;

  /* in the order of their names */
  size_t count = yyjson_mut_obj_size(jobs);
  const char **names = calloc(count + 1, sizeof(char *));
  yyjson_mut_obj_iter walk;
  yyjson_mut_val *key;
  size_t at = 0;

  yyjson_mut_obj_iter_init(jobs, &walk);

  while (names != NULL && (key = yyjson_mut_obj_iter_next(&walk)) != NULL)
    names[at++] = yyjson_mut_get_str(key);

  for (size_t i = 0; names != NULL && i < at; ++i)
    for (size_t j = i + 1; j < at; ++j)
      if (strcmp(names[j], names[i]) < 0) {
        const char *swap = names[i];
        names[i] = names[j];
        names[j] = swap;
      }

  bool changed = false;

  for (size_t i = 0; names != NULL && i < at; ++i) {

    yyjson_mut_val *job = yyjson_mut_obj_get(jobs, names[i]);
    const char *s = textOf(yyjson_mut_obj_get(job, "s"));
    yyjson_mut_val *id = yyjson_mut_obj_get(job, "ID");
    bool ours = yyjson_mut_is_str(id) && strcmp(yyjson_mut_get_str(id), it->id) == 0;
    bool available = strcmp(s, "queued") == 0;

    if (strcmp(s, "running") == 0 && !ours) {

      bool fresh;
      long since;

      seenAt(it->seen, names[i], textOf(yyjson_mut_obj_get(job, "n")), now,
             &fresh, &since);

      if (!fresh && now - since >= it->timeout) {
        dbmSay(stderr, TEXT`[WARN] [jobs] taking over ${names[i]}, its worker did not respond for ${it->timeout} ms\n`);
        available = true;
      }
    }

    if (available) {
      char n[17];
      nonce(n);
      assign(doc, job, "s", yyjson_mut_str(doc, "running"));
      assign(doc, job, "ID", yyjson_mut_strcpy(doc, it->id));
      assign(doc, job, "n", yyjson_mut_strcpy(doc, n));
      dbmWrite(it->taken, it->room, TEXT`${names[i]}`);
      it->took = true;
      changed = true;
      break;
    }

    if (yyjson_mut_is_true(yyjson_mut_obj_get(job, "blocking")))
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

  if (dbmJobsChange(state, claiming, &it, why, whyRoom))
    return -1;

  return it.wait ? 2 : it.took ? 1 : 0;
}

typedef struct {
  dbm_job_t *job;
  const char *changes;
  bool remove;
} updating_t;

static bool updating(yyjson_mut_doc *doc, yyjson_mut_val *jobs,
                     yyjson_mut_val *root, void *context) {

  updating_t *it = context;
  yyjson_mut_val *job = yyjson_mut_obj_get(jobs, it->job->name);
  yyjson_mut_val *id = yyjson_mut_obj_get(job, "ID");

  (void)root;

  if (job == NULL || !yyjson_mut_is_str(id) ||
      strcmp(yyjson_mut_get_str(id), it->job->id) != 0) {
    it->job->lost = true;
    return false;
  }

  if (it->remove) {
    yyjson_mut_obj_remove_key(jobs, it->job->name);
    return true;
  }

  yyjson_doc *read = yyjson_read(it->changes, strlen(it->changes), 0);
  yyjson_val *key;
  yyjson_obj_iter walk;

  yyjson_obj_iter_init(yyjson_doc_get_root(read), &walk);

  while ((key = yyjson_obj_iter_next(&walk)) != NULL) {

    yyjson_val *value = yyjson_obj_iter_get_val(key);

    /* null takes a field away, as JS's undefined does in its JSON */
    if (yyjson_is_null(value))
      yyjson_mut_obj_remove_key(job, yyjson_get_str(key));
    else
      assign(doc, job, yyjson_get_str(key), yyjson_val_mut_copy(doc, value));
  }

  yyjson_doc_free(read);

  char n[17];
  nonce(n);
  assign(doc, job, "n", yyjson_mut_strcpy(doc, n));
  return true;
}

int dbmJobUpdate(dbm_job_t *job, const char *changes, char *why, size_t room) {
  updating_t it = {.job = job, .changes = changes};
  return dbmJobsChange(job->state, updating, &it, why, room);
}

int dbmJobDone(dbm_job_t *job, char *why, size_t room) {
  updating_t it = {.job = job, .remove = true};
  return dbmJobsChange(job->state, updating, &it, why, room);
}
