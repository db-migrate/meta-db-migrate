/**
 * Running migrations: which ones, in which order, and what happens when one
 * of them fails.
 *
 * Each migration runs in a transaction of its own together with the line that
 * records it, so a migration and the record of it are never out of step: a
 * failure takes both back, and the next `up` starts at the one that failed.
 * That is node db-migrate's behaviour, and on PostgreSQL it covers the DDL
 * too, because PostgreSQL's DDL is transactional. MySQL's is not, which no
 * driver can paper over.
 */
#include <db_migrate_driver.h>

#include <stdio.h>
#include <string.h>

typedef enum direction { UP, DOWN } direction_t;

/**
 * The names already run, with the table they are kept in made first - and
 * whether that worked, beside them rather than through a pointer.
 */
static json_t, bool loaded(driver_t *driver) {

  json_t names = {0};

  if (driver->createMigrationsTable(driver)) {
    dbmSay(stderr, TEXT`[ERROR] could not create the migrations table: ${driver->error}\n`);
    return names, false;
  }

  if (driver->loadedMigrations(driver, &names)) {
    dbmSay(stderr, TEXT`[ERROR] could not read the migrations table: ${driver->error}\n`);
    return names, false;
  }

  return names, true;
}

/**
 * The scope the walker works in - "" for migrations/ itself, `billing` for
 * migrations/billing/ - as `up:billing` names it. Every command sees only
 * the migrations and the records of its own scope, the way node does it.
 */
static const char *scope = "";

void dbmUseScope(const char *name) {
  scope = name != NULL ? name : "";
}

/** Whether a recorded name belongs to the scope: what is before its last slash. */
static bool inScope(const char *name) {

  const char *slash = strrchr(name, '/');
  size_t length = slash != NULL ? (size_t)(slash - name) : 0;

  return strlen(scope) == length && strncmp(name, scope, length) == 0;
}

/** A name as a person reads it: the file, with its scope if it has one. */
static const char *shown(const char *name) {
  return name[0] == '/' ? name + 1 : name;
}

static bool hasRun(json_t names, const char *name) {

  for (int i = 0; i < names.count(); ++i)
    if (strcmp(names[i].name, name) == 0)
      return true;

  return false;
}

/** Whether SQL text says nothing: whitespace, `--` lines and block comments only. */
static bool onlyComments(const char *sql) {

  const char *at = sql;

  while (*at != '\0') {

    if (*at in {' ', '\t', '\n', '\r'}) {
      ++at;
    } else if (at[0] == '-' && at[1] == '-') {
      while (*at != '\0' && *at != '\n')
        ++at;
    } else if (at[0] == '/' && at[1] == '*') {

      const char *end = strstr(at + 2, "*/");

      if (end == NULL)
        return false;

      at = end + 2;
    } else {
      return false;
    }
  }

  return true;
}

/**
 * A v2 migration: no transaction - its rollback is its own record run
 * backwards - and the line in the migrations table written after it, as node
 * writes it.
 */
static int stepV2(driver_t *driver, const dbm_migration_t *migration,
                  direction_t direction) {

  char why[512] = "";

  if (driver->state == NULL) {
    dbmSay(stderr, TEXT`[ERROR] ${shown(migration->name)} is a v2 migration, and there is no state table to keep it in\n`);
    return -1;
  }

  int answer =
      direction == UP
          ? dbmUpV2(driver, driver->state, migration->name, migration->migrate,
                    why, sizeof why)
          : dbmDownV2(driver, driver->state, migration->name, why, sizeof why);

  if (answer == 0) {

    int recorded =
        direction == UP
            ? driver->addMigrationRecord(driver, migration->name)
            : driver->deleteMigrationRecord(driver, migration->name);

    if (recorded != 0)
      dbmWrite(why, sizeof why, TEXT`${driver->error}`);

    answer = recorded;
  }

  if (answer == 0 && direction == UP)
    dbmEndV2(driver->state, driver->dryRun);

  if (answer != 0) {
    dbmSay(stderr, TEXT`[ERROR] ${shown(migration->name)}: ${why}\n`);
    return -1;
  }

  return 0;
}

/**
 * The migration lock, when there is something to do - and the record of what
 * has run read again under it, because another process may have done the
 * work while this one waited. Nothing on a dry run, as node has it.
 */
static bool takeLock(driver_t *driver, json_t *names) {

  if (driver->state == NULL || driver->dryRun)
    return true;

  if (!dbmStateLock(driver->state))
    return false;

  if (dbmStateReloadSchema(driver->state)) {
    dbmSay(stderr, TEXT`[ERROR] could not read the schema state: ${driver->state->db->error}\n`);
    return false;
  }

  json_t again, bool ok = loaded(driver);

  if (!ok)
    return false;

  names->release();
  *names = again;
  return true;
}

static void giveLock(driver_t *driver) {

  if (driver->state != NULL)
    dbmStateUnlock(driver->state);
}

/** One migration, one direction, one transaction. */
static int step(driver_t *driver, const dbm_migration_t *migration,
                direction_t direction) {

  migrator_t db = {.driver = driver, .dryRun = driver->dryRun};

  /* compiled and opened now, if the launcher only knew its file */
  if (!dbmLoaded(migration))
    return -1;

  dbm_step_t body = direction == UP ? migration->up : migration->down;
  const char *sql = direction == UP ? migration->upSql : migration->downSql;
  const char *way = direction == UP ? "up" : "down";

  dbmSay(stdout, TEXT`[INFO] ${direction == UP ? "Processing migration"
                                           : "Undoing migration"} ${shown(migration->name)}\n`);

  if (migration->migrate != NULL)
    return stepV2(driver, migration, direction);

  if (body == NULL && sql == NULL) {
    dbmSay(stderr, TEXT`[ERROR] ${shown(migration->name)} has no ${way}\n`);
    return -1;
  }

  if (!driver->noTransactions && driver->startMigration(driver)) {
    dbmSay(stderr, TEXT`[ERROR] could not start a transaction: ${driver->error}\n`);
    return -1;
  }

  /**
   * A migration in SQL is its text, sent as it is - unless there is nothing
   * in it but comments, which is what `create --sql-file` writes and what a
   * migration with no `down` worth having keeps. MySQL calls an empty
   * statement an error; nothing to do is not one.
   */
  int answer = body != NULL       ? body(&db)
               : onlyComments(sql) ? 0
                                   : db.runSql(sql);

  /**
   * A migration that answered 0 but has a failure recorded failed: the
   * failure is the truth, and the 0 is a migration that forgot to look.
   * One that answered non-zero with nothing recorded failed too, and says
   * so in its own words.
   */
  if (answer != 0 && !db.failed)
    db.fail(TEXT`the migration answered non-zero without saying why`);

  if (!db.failed) {

    int recorded = direction == UP
                       ? driver->addMigrationRecord(driver, migration->name)
                       : driver->deleteMigrationRecord(driver, migration->name);

    if (recorded != 0)
      db.fail(TEXT`${driver->error}`);
  }

  if (db.failed) {
    dbmSay(stderr, TEXT`[ERROR] ${shown(migration->name)}: ${db.error}\n`);

    if (!driver->noTransactions && driver->abortMigration(driver))
      dbmSay(stderr, TEXT`[ERROR] and the rollback failed too: ${driver->error}\n`);

    return -1;
  }

  if (!driver->noTransactions && driver->endMigration(driver)) {
    dbmSay(stderr, TEXT`[ERROR] could not commit ${shown(migration->name)}: ${driver->error}\n`);
    return -1;
  }

  return 0;
}

/**
 * Whether a migration lies on the near side of a destination, compared the
 * way node does it: on as many characters as both have. So `20261008` is a
 * destination that every migration of that day reaches, and a full name is
 * one migration exactly.
 */
static int towards(const char *name, const char *destination) {

  const char *file = strrchr(name, '/') != NULL ? strrchr(name, '/') + 1 : name;
  size_t a = strlen(file);
  size_t b = strlen(destination);

  return strncmp(file, destination, a < b ? a : b);
}

/** Up to and including the destination, at most `count`; zero is no limit. */
int dbmUp(driver_t *driver, size_t count, const char *destination,
          bool dryRun) {

  size_t total;
  size_t done = 0;
  const dbm_migration_t *migrations = dbmMigrations(&total);

  driver->dryRun = dryRun;

  json_t names, bool ready = loaded(driver);

  if (!ready)
    return -1;

  bool pending = false;

  for (size_t i = 0; i < total && !pending; ++i)
    pending = inScope(migrations[i].name) &&
              !hasRun(names, migrations[i].name) &&
              (destination == NULL ||
               towards(migrations[i].name, destination) <= 0);

  if (pending && !takeLock(driver, &names)) {
    names.release();
    giveLock(driver);
    return -1;
  }

  defer names.release();
  defer giveLock(driver);

  for (size_t i = 0; i < total && (count == 0 || done < count); ++i) {

    if (!inScope(migrations[i].name) || hasRun(names, migrations[i].name))
      continue;

    /* sorted by name, so the first one past the destination ends it */
    if (destination != NULL && towards(migrations[i].name, destination) > 0)
      break;

    if (step(driver, &migrations[i], UP))
      return -1;

    ++done;
  }

  if (done == 0)
    dbmSay(stdout, TEXT`[INFO] No migrations to run\n`);

  dbmSay(stdout, TEXT`[INFO] Done\n`);
  return 0;
}

static const dbm_migration_t *named(const char *name) {

  size_t total;
  const dbm_migration_t *migrations = dbmMigrations(&total);

  for (size_t i = 0; i < total; ++i)
    if (strcmp(migrations[i].name, name) == 0)
      return &migrations[i];

  return NULL;
}

/**
 * Undoes the last `count` that ran, newest first, zero being all of them -
 * and with a destination, everything after it. The destination itself stays,
 * as node has it: `down 20261008120100` leaves that migration run.
 */
int dbmDown(driver_t *driver, size_t count, const char *destination,
            bool dryRun) {

  size_t done = 0;

  driver->dryRun = dryRun;

  json_t names, bool ready = loaded(driver);

  if (!ready)
    return -1;

  bool pending = false;

  for (int i = names.count() - 1; i >= 0 && !pending; --i)
    pending = inScope(names[i].name) &&
              (destination == NULL || towards(names[i].name, destination) > 0);

  if (pending && !takeLock(driver, &names)) {
    names.release();
    giveLock(driver);
    return -1;
  }

  defer names.release();
  defer giveLock(driver);

  for (int i = names.count() - 1; i >= 0 && (count == 0 || done < count);
       --i) {

    const char *name = names[i].name;

    if (!inScope(name))
      continue;

    if (destination != NULL && towards(name, destination) <= 0)
      break;

    const dbm_migration_t *migration = named(name);

    if (migration == NULL) {
      dbmSay(stderr, TEXT`[ERROR] ${name} was run, and this program does not have it - it cannot be undone from here\n`);
      return -1;
    }

    if (step(driver, migration, DOWN))
      return -1;

    ++done;
  }

  if (done == 0)
    dbmSay(stdout, TEXT`[INFO] No migrations to undo\n`);

  dbmSay(stdout, TEXT`[INFO] Done\n`);
  return 0;
}

int dbmReset(driver_t *driver, bool dryRun) {
  return dbmDown(driver, 0, NULL, dryRun);
}

/**
 * To the destination from wherever the database is: down when the newest
 * migration run lies past it, up otherwise.
 */
int dbmSync(driver_t *driver, const char *destination, bool dryRun) {

  driver->dryRun = dryRun;

  json_t names, bool ready = loaded(driver);

  if (!ready)
    return -1;

  int newest = names.count() - 1;

  while (newest >= 0 && !inScope(names[newest].name))
    --newest;

  bool past = newest >= 0 && towards(names[newest].name, destination) > 0;

  names.release();

  if (past) {
    dbmSay(stdout, TEXT`[INFO] Syncing downwards to ${destination}\n`);
    return dbmDown(driver, 0, destination, dryRun);
  }

  dbmSay(stdout, TEXT`[INFO] Syncing upwards to ${destination}\n`);
  return dbmUp(driver, 0, destination, dryRun);
}

/**
 * node's `fix`: the state rebuilt from the v2 migrations that ran, oldest
 * first - learned again against an empty schema, nothing sent. A v1
 * migration keeps no schema, and is passed over with a word. With a backup,
 * the state as it was goes to a file and to a table of its own first.
 */
int dbmFix(driver_t *driver, bool backup, bool dryRun) {

  driver->dryRun = dryRun;

  if (driver->state == NULL) {
    dbmSay(stderr, TEXT`[ERROR] fix needs the state table\n`);
    return -1;
  }

  /**
   * The backup before the lock, where node makes it: renaming the table
   * takes the lock row along with everything else, and a lock held in the
   * backup is a lock nobody holds.
   */
  if (backup && !dryRun && dbmStateBackup(driver->state)) {
    dbmSay(stderr, TEXT`[ERROR] could not back the state up: ${driver->state->db->error}\n`);
    return -1;
  }

  json_t names, bool ready = loaded(driver);

  if (!ready)
    return -1;

  if (names.count() > 0 && !takeLock(driver, &names)) {
    names.release();
    giveLock(driver);
    return -1;
  }

  defer names.release();
  defer giveLock(driver);

  dbmStateForgetSchema(driver->state);

  /* oldest first, as they were built */
  for (int i = 0; i < names.count(); ++i) {

    const char *name = names[i].name;
    const dbm_migration_t *migration = named(name);
    char why[512] = "";

    if (!inScope(name))
      continue;

    if (migration == NULL || !dbmLoaded(migration)) {
      dbmSay(stderr, TEXT`[ERROR] ${shown(name)} was run, and this program does not have it\n`);
      return -1;
    }

    if (migration->migrate == NULL) {
      dbmSay(stdout, TEXT`[WARN] skipping ${shown(name)}, v1 migrations do not keep a schema\n`);
      continue;
    }

    dbmSay(stdout, TEXT`[INFO] Fixing the state of ${shown(name)}\n`);

    if (dbmFixV2(driver, driver->state, name, migration->migrate, why,
                 sizeof why)) {
      dbmSay(stderr, TEXT`[ERROR] ${shown(name)}: ${why}\n`);
      return -1;
    }
  }

  dbmSay(stdout, TEXT`[INFO] Done\n`);
  return 0;
}

int dbmCheck(driver_t *driver) {

  size_t total;
  size_t pending = 0;
  const dbm_migration_t *migrations = dbmMigrations(&total);

  driver->dryRun = false;

  json_t names, bool ready = loaded(driver);

  if (!ready)
    return -1;

  defer names.release();

  for (size_t i = 0; i < total; ++i) {

    if (!inScope(migrations[i].name) || hasRun(names, migrations[i].name))
      continue;

    dbmSay(stdout, TEXT`[INFO] Pending: ${shown(migrations[i].name)}\n`);
    ++pending;
  }

  dbmSay(stdout, TEXT`[INFO] ${pending} migration(s) to run\n`);
  return 0;
}
