/**
 * The worker running background jobs: node db-migrate's lib/work.js and
 * `db-migrate work`, since 1.4.0 and 1.6.0.
 *
 *   migrate work --parallel 2 --pause 50 --batch 500 --watch
 *
 * Each slot takes the first job it may - by name, after none that blocks -
 * runs it, and takes the next, until there are none; with --watch it goes
 * on looking. A slot is a process of its own with its own connections, so
 * two never share anything but the database. All slots of one worker share
 * its id: a job one of them runs is not taken over by another.
 *
 * Where node has a bug, this does what node meant:
 *
 * - a job recovered by rolling back keeps its record, which node's worker
 *   deletes and never writes again
 * - a job whose migration is recorded already - its worker died between
 *   recording it and forgetting the job - is forgotten, not run again
 * - a job of a scope runs in its scope; node's worker knows the top level only
 * - --dry-run says which jobs would run, where node's fails on its own writes
 */
#include <db_migrate_driver.h>

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile int stopRequested;

/** The slots' processes, which a stop is passed on to. */
static pid_t workers[64];
static volatile int workerCount;

void dbmWorkStop(void) {

  stopRequested = 1;

  for (int i = 0; i < workerCount; ++i)
    if (workers[i] > 0)
      kill(workers[i], SIGTERM);
}

/** 16 random bytes in base64, as node's worker id. */
static void workerId(char into[32]) {

  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  unsigned char bytes[18] = {0};
  size_t at = 0;

  if (getrandom(bytes, 16, 0) != 16)
    for (size_t i = 0; i < 16; ++i)
      bytes[i] = (unsigned char)rand();

  for (size_t i = 0; i < 18; i += 3) {
    unsigned long group = ((unsigned long)bytes[i] << 16) |
                          ((unsigned long)bytes[i + 1] << 8) | bytes[i + 2];
    into[at++] = alphabet[(group >> 18) & 63];
    into[at++] = alphabet[(group >> 12) & 63];
    into[at++] = alphabet[(group >> 6) & 63];
    into[at++] = alphabet[group & 63];
  }

  /* 16 bytes are 22 characters and two of padding */
  into[22] = '=';
  into[23] = '=';
  into[24] = '\0';
}

/** The migration a job runs, by its key, in whichever scope it is. */
static const dbm_migration_t *migrationOf(const char *key) {

  size_t total;
  const dbm_migration_t *migrations = dbmMigrations(&total);

  for (size_t i = 0; i < total; ++i)
    if (strcmp(dbmKeyOf(migrations[i].name), key) == 0)
      return &migrations[i];

  return NULL;
}

/* ------------------------------------------------------------------ */
/* the heartbeat of a job                                             */
/* ------------------------------------------------------------------ */

typedef struct {
  dbm_job_t *job;
  long every;
  volatile bool stop;
} heartbeat_t;

static void *beating(void *raw) {

  heartbeat_t *beat = raw;

  while (!beat->stop) {

    long until = dbmNow() + beat->every;

    while (!beat->stop && dbmNow() < until)
      dbmSleep(10);

    char why[300];

    if (!beat->stop && dbmJobUpdate(beat->job, "{}", why, sizeof why))
      dbmSay(stderr, TEXT`[ERROR] ${why}\n`);
  }

  return NULL;
}

/* ------------------------------------------------------------------ */
/* one slot                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
  json_t config;
  const dbm_options_t *options;
  const dbm_work_t *work;
  const char *id;

  /** Where a slot of its own process says what became of its jobs. */
  int report;

  long done;
  long failed;
  dbm_text_t names;
  char error[600];
} slot_t;

static void reported(slot_t *slot, char kind, const char *name) {

  if (kind == 'D')
    ++slot->done;
  else {
    ++slot->failed;
    slot->names.append(TEXT`${slot->names.length > 0 ? ", " : ""}${name}`);
  }

  if (slot->report >= 0) {
    char line[300];
    size_t length = dbmWrite(line, sizeof line, TEXT`${kind} ${name}\n`);
    if (write(slot->report, line, length < sizeof line ? length : sizeof line - 1) < 0)
      dbmSay(stderr, TEXT`[WARN] [jobs] could not report ${name}\n`);
  }
}

static bool recorded(driver_t *driver, const char *name) {

  json_t names = {0};
  bool found = false;

  if (driver->loadedMigrations(driver, &names) != 0)
    return false;

  for (int i = 0; i < names.count(); ++i)
    found = found || strcmp(names.at(i).get("name").text(), name) == 0;

  names.release();
  return found;
}

/** One job run: 0 whatever became of it, -1 when the worker can not go on. */
static int runJob(slot_t *slot, driver_t *driver, const char *name,
                  yyjson_mut_doc *seen) {

  dbm_state_t *state = driver->state;
  dbm_job_t job = {.state = state, .id = slot->id, .stopping = &stopRequested,
                   .pause = slot->work->pause, .batch = slot->work->batch,
                   .seen = seen};
  const dbm_migration_t *migration = migrationOf(name);
  char why[600] = "";

  dbmWrite(job.name, sizeof job.name, TEXT`${name}`);

  if (migration == NULL || !dbmLoaded(migration)) {
    dbmJobUpdate(&job, "{\"s\":\"failed\",\"ID\":0,\"err\":\"the migration file is missing\"}",
                 why, sizeof why);
    dbmWrite(slot->error, sizeof slot->error, TEXT`The migration file of the job ${name} is missing`);
    return -1;
  }

  /* recorded already: its worker died before it forgot the job */
  if (recorded(driver, migration->name)) {
    dbmJobDone(&job, why, sizeof why);
    dbmSay(stdout, TEXT`[INFO] [jobs] ${name} is done\n`);
    return 0;
  }

  /* the release of the migration: a soft delete schedules its purge by it */
  dbmJobRelease(driver, migration);

  heartbeat_t beat = {.job = &job};
  pthread_t thread;
  long every = slot->work->timeout / 3;

  beat.every = every > 0 ? every : 1;

  bool beating_ = pthread_create(&thread, NULL, beating, &beat) == 0;
  bool rolledBack = false;
  int answer = dbmRunJob(driver, state, migration, &job, &rolledBack, why,
                         sizeof why);

  beat.stop = true;

  if (beating_)
    pthread_join(thread, NULL);

  char failed[800];

  if (job.lost) {
    dbmSay(stderr, TEXT`[WARN] [jobs] ${name} was taken over by another worker\n`);
  } else if (answer == 1) {
    dbmJobUpdate(&job, "{\"s\":\"queued\",\"ID\":0}", failed, sizeof failed);
    dbmSay(stdout, TEXT`[INFO] [jobs] ${name} stopped, it continues with the next run\n`);
  } else if (answer == 0) {

    if (driver->addMigrationRecord(driver, migration->name) ||
        dbmJobDone(&job, why, sizeof why)) {
      dbmWrite(slot->error, sizeof slot->error, TEXT`${why[0] != '\0' ? why : driver->error}`);
      return -1;
    }

    dbmSay(stdout, TEXT`[INFO] [jobs] ${name} is done\n`);
    reported(slot, 'D', name);
  } else {

    /* one rolled back starts over, else it goes on after the irreversible */
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *changes = yyjson_mut_obj(doc);

    yyjson_mut_doc_set_root(doc, changes);
    yyjson_mut_obj_add_str(doc, changes, "s", "failed");
    yyjson_mut_obj_add_int(doc, changes, "ID", 0);
    yyjson_mut_obj_add_strcpy(doc, changes, "err", why[0] != '\0' ? why : "it failed");

    if (rolledBack) {
      yyjson_mut_obj_add_int(doc, changes, "step", 0);
      yyjson_mut_obj_add_int(doc, changes, "learned", 0);
      yyjson_mut_obj_add_int(doc, changes, "done", 0);
      yyjson_mut_obj_add_int(doc, changes, "rb", 0);
    }

    char *text = yyjson_mut_write(doc, 0, NULL);

    dbmJobUpdate(&job, text != NULL ? text : "{}", failed, sizeof failed);
    free(text);
    yyjson_mut_doc_free(doc);

    dbmSay(stderr, TEXT`[ERROR] [jobs] ${name} failed: ${why}\n`);
    reported(slot, 'F', name);
  }

  return 0;
}

static int runSlot(slot_t *slot) {

  driver_t *stateDriver = NULL;
  driver_t *driver = dbmConnect(slot->config, slot->options, false,
                                &stateDriver, slot->error,
                                sizeof slot->error);

  if (driver == NULL)
    return -1;

  if (driver->state == NULL) {
    dbmWrite(slot->error, sizeof slot->error, TEXT`Background migrations need a driver supporting the migration lock`);
    dbmDisconnect(driver, stateDriver);
    return -1;
  }

  yyjson_mut_doc *seen = yyjson_mut_doc_new(NULL);
  int answer = 0;

  yyjson_mut_doc_set_root(seen, yyjson_mut_obj(seen));

  while (!stopRequested && answer == 0) {

    char name[256];
    int claimed = dbmJobsClaim(driver->state, slot->id, seen,
                               slot->work->timeout, name, sizeof name,
                               slot->error, sizeof slot->error);

    if (claimed < 0)
      answer = -1;
    else if (claimed == 1)
      answer = runJob(slot, driver, name, seen);
    else if (claimed == 0 && !slot->work->watch)
      break;
    else
      for (long waited = 0; !stopRequested && waited < slot->work->interval;
           waited += 10)
        dbmSleep(10);
  }

  yyjson_mut_doc_free(seen);
  dbmDisconnect(driver, stateDriver);
  return answer;
}

/* ------------------------------------------------------------------ */
/* the worker                                                         */
/* ------------------------------------------------------------------ */

/** A dry run: which jobs there are to run. */
static int dryRun(json_t config, const dbm_options_t *options, char *why,
                  size_t room) {

  driver_t *stateDriver = NULL;
  driver_t *driver = dbmConnect(config, options, true, &stateDriver, why, room);

  if (driver == NULL)
    return -1;

  char *text = driver->state != NULL ? dbmJobsRead(driver->state) : NULL;
  json_t jobs = meta_toJSON(text != NULL ? text : "{\"jobs\":{}}");
  json_t list = jobs.get("jobs");

  free(text);

  for (int i = 0; i < list.count(); ++i)
    if (strcmp(list.get(list.keyAt(i)).get("s").text(), "failed") != 0)
      dbmSay(stdout, TEXT`[INFO] [jobs] ${list.keyAt(i)} would run\n`);

  jobs.release();
  dbmDisconnect(driver, stateDriver);
  return 0;
}

int dbmWork(json_t config, const dbm_options_t *options, dbm_work_t *work,
            char *why, size_t room) {

  dbm_work_t settings = *work;
  char id[32];

  why[0] = '\0';
  stopRequested = 0;
  workerId(id);

  if (settings.parallel < 1)
    settings.parallel = 1;
  if (settings.interval <= 0)
    settings.interval = 5000;
  if (settings.timeout <= 0)
    settings.timeout = 60000;

  work->done = 0;
  work->failed = 0;

  if (settings.dryRun)
    return dryRun(config, options, why, room);

  slot_t slot = {.config = config, .options = options, .work = &settings,
                 .id = id, .report = -1};
  int answer = 0;

  if (settings.parallel == 1) {

    answer = runSlot(&slot);

    work->done = slot.done;
    work->failed = slot.failed;

    if (answer != 0)
      dbmWrite(why, room, TEXT`${slot.error}`);
    else if (slot.failed > 0)
      dbmWrite(why, room, TEXT`${slot.failed} background job(s) failed: ${slot.names.text}`);

    slot.names.release();
    return answer != 0 || slot.failed > 0 ? -1 : 0;
  }

  /* each slot a process: what it did comes back through a pipe */
  int channel[2];
  pid_t children[64];
  long count = settings.parallel < 64 ? settings.parallel : 64;

  if (pipe(channel) != 0) {
    dbmWrite(why, room, TEXT`cannot start the workers: ${strerror(errno)}`);
    return -1;
  }

  fflush(NULL);

  for (long i = 0; i < count; ++i) {

    children[i] = fork();

    if (children[i] == 0) {

      workerCount = 0;
      close(channel[0]);
      slot.report = channel[1];

      int code = runSlot(&slot);

      if (code != 0)
        dbmSay(stderr, TEXT`[ERROR] ${slot.error}\n`);

      fflush(NULL);
      _exit(code != 0 ? 2 : 0);
    }
  }

  for (long i = 0; i < count; ++i)
    workers[i] = children[i];

  workerCount = (int)count;

  /* a stop asked for while they started */
  if (stopRequested)
    dbmWorkStop();

  close(channel[1]);

  /* read until every slot closed its end */
  dbm_text_t names = {0};
  char buffer[4096];
  size_t held = 0;

  for (;;) {

    ssize_t got = read(channel[0], buffer + held, sizeof buffer - held - 1);

    if (got < 0 && errno == EINTR)
      continue;

    if (got <= 0)
      break;

    held += (size_t)got;
    buffer[held] = '\0';

    char *line;
    char *rest = buffer;

    while ((line = strchr(rest, '\n')) != NULL) {

      *line = '\0';

      if (rest[0] == 'D')
        ++work->done;
      else if (rest[0] == 'F') {
        ++work->failed;
        names.append(TEXT`${names.length > 0 ? ", " : ""}${rest + 2}`);
      }

      rest = line + 1;
    }

    held = strlen(rest);
    memmove(buffer, rest, held + 1);
  }

  close(channel[0]);
  workerCount = 0;

  for (long i = 0; i < count; ++i) {

    int status = 0;

    while (children[i] > 0 && waitpid(children[i], &status, 0) < 0 &&
           errno == EINTR)
      ;

    if (children[i] < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
      answer = -1;
  }

  if (answer != 0)
    dbmWrite(why, room, TEXT`a worker could not go on, see above`);
  else if (work->failed > 0)
    dbmWrite(why, room, TEXT`${work->failed} background job(s) failed: ${names.text}`);

  names.release();
  return answer != 0 || work->failed > 0 ? -1 : 0;
}
