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
                                           : "Undoing migration"} ${migration->name + 1}\n`);

  if (body == NULL && sql == NULL) {
    dbmSay(stderr, TEXT`[ERROR] ${migration->name + 1} has no ${way}\n`);
    return -1;
  }

  if (driver->startMigration(driver)) {
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
    dbmSay(stderr, TEXT`[ERROR] ${migration->name + 1}: ${db.error}\n`);

    if (driver->abortMigration(driver))
      dbmSay(stderr, TEXT`[ERROR] and the rollback failed too: ${driver->error}\n`);

    return -1;
  }

  if (driver->endMigration(driver)) {
    dbmSay(stderr, TEXT`[ERROR] could not commit ${migration->name + 1}: ${driver->error}\n`);
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

  size_t a = strlen(name + 1);
  size_t b = strlen(destination);

  return strncmp(name + 1, destination, a < b ? a : b);
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

  defer names.release();

  for (size_t i = 0; i < total && (count == 0 || done < count); ++i) {

    if (hasRun(names, migrations[i].name))
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

  defer names.release();

  for (int i = names.count() - 1; i >= 0 && (count == 0 || done < count);
       --i) {

    const char *name = names[i].name;

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
  bool past = newest >= 0 && towards(names[newest].name, destination) > 0;

  names.release();

  if (past) {
    dbmSay(stdout, TEXT`[INFO] Syncing downwards to ${destination}\n`);
    return dbmDown(driver, 0, destination, dryRun);
  }

  dbmSay(stdout, TEXT`[INFO] Syncing upwards to ${destination}\n`);
  return dbmUp(driver, 0, destination, dryRun);
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

    if (hasRun(names, migrations[i].name))
      continue;

    dbmSay(stdout, TEXT`[INFO] Pending: ${migrations[i].name + 1}\n`);
    ++pending;
  }

  dbmSay(stdout, TEXT`[INFO] ${pending} migration(s) to run\n`);
  return 0;
}
