/**
 * What a migration sees: `db->createTable(...)` and the rest.
 *
 * Every method is the driver's slot with two things around it. Errors stick:
 * the first failure is copied here and every later call answers -1 without
 * reaching the driver, so a migration reads as the list of steps it is. And
 * the driver's own reason is kept, word for word, because "it failed" is not
 * something anybody can act on.
 */
#include <db_migrate_driver.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Before a call: whether there is anything to do. */
static bool blocked(migrator_t *self) {
  return self->failed;
}

/** After a call: keep the reason when there is one. */
static int settle(migrator_t *self, int answer) {

  if (answer == 0)
    return 0;

  if (self->driver->error[0] == '\0')
    return self->fail(TEXT`the driver failed and said nothing about why`);

  return self->fail(TEXT`${self->driver->error}`);
}

int migrator_t__createTable(migrator_t *self, const char *table, json_t spec) {
  if (blocked(self)) return -1;
  return settle(self, self->driver->createTable(self->driver, table, spec));
}

int migrator_t__dropTable(migrator_t *self, const char *table) {
  if (blocked(self)) return -1;
  return settle(self, self->driver->dropTable(self->driver, table, false));
}

int migrator_t__dropTableIfExists(migrator_t *self, const char *table) {
  if (blocked(self)) return -1;
  return settle(self, self->driver->dropTable(self->driver, table, true));
}

int migrator_t__renameTable(migrator_t *self, const char *from, const char *to) {
  if (blocked(self)) return -1;
  return settle(self, self->driver->renameTable(self->driver, from, to));
}

int migrator_t__addColumn(migrator_t *self, const char *table,
                          const char *column, json_t spec) {
  if (blocked(self)) return -1;
  return settle(self,
                self->driver->addColumn(self->driver, table, column, spec));
}

int migrator_t__removeColumn(migrator_t *self, const char *table,
                             const char *column) {
  if (blocked(self)) return -1;
  return settle(self, self->driver->removeColumn(self->driver, table, column));
}

int migrator_t__renameColumn(migrator_t *self, const char *table,
                             const char *from, const char *to) {
  if (blocked(self)) return -1;
  return settle(self,
                self->driver->renameColumn(self->driver, table, from, to));
}

int migrator_t__changeColumn(migrator_t *self, const char *table,
                             const char *column, json_t spec) {
  if (blocked(self)) return -1;
  return settle(self,
                self->driver->changeColumn(self->driver, table, column, spec));
}

int migrator_t__addIndex(migrator_t *self, const char *table, const char *name,
                         json_t columns) {
  if (blocked(self)) return -1;
  return settle(self, self->driver->addIndex(self->driver, table, name,
                                             columns, false));
}

int migrator_t__addUniqueIndex(migrator_t *self, const char *table,
                               const char *name, json_t columns) {
  if (blocked(self)) return -1;
  return settle(self, self->driver->addIndex(self->driver, table, name,
                                             columns, true));
}

int migrator_t__removeIndex(migrator_t *self, const char *table,
                            const char *name) {
  if (blocked(self)) return -1;
  return settle(self, self->driver->removeIndex(self->driver, table, name));
}

int migrator_t__addForeignKey(migrator_t *self, const char *table,
                              const char *referenced, const char *name,
                              json_t mapping, json_t rules) {
  if (blocked(self)) return -1;
  return settle(self, self->driver->addForeignKey(self->driver, table,
                                                  referenced, name, mapping,
                                                  rules));
}

int migrator_t__removeForeignKey(migrator_t *self, const char *table,
                                 const char *name) {
  if (blocked(self)) return -1;
  return settle(self,
                self->driver->removeForeignKey(self->driver, table, name));
}

int migrator_t__insert(migrator_t *self, const char *table, json_t row) {
  if (blocked(self)) return -1;
  return settle(self, self->driver->insert(self->driver, table, row));
}

int migrator_t__runSql(migrator_t *self, const char *text) {

  dbm_text_t sql = {0};
  defer sql.release();

  if (blocked(self))
    return -1;

  sql.put(text);

  return settle(self, dbmSend(self->driver, &sql));
}

int migrator_t__run(migrator_t *self, sql_t query) {

  int answer;

  if (blocked(self)) {
    query.release();
    return -1;
  }

  answer = dbmQuery(self->driver, &query, NULL);

  query.release();
  return settle(self, answer);
}

json_t migrator_t__all(migrator_t *self, sql_t query) {

  json_t rows = meta_toJSON("[]");
  int answer = 0;

  /* a dry run prints the query and answers no rows, as a database would */
  if (!blocked(self) && self->driver->dryRun) {
    answer = dbmQuery(self->driver, &query, NULL);
  } else if (!blocked(self)) {
    rows.release();
    answer = dbmQuery(self->driver, &query, &rows);

    if (answer != 0)
      rows = meta_toJSON("[]");
  }

  query.release();
  settle(self, answer);

  return rows;
}

bool migrator_t__hasFailed(migrator_t *self) {
  return self->failed;
}

const char *migrator_t__lastError(migrator_t *self) {
  return self->failed ? self->error : "";
}

int migrator_t__fail(migrator_t *self, text_t why) {

  /* the first reason is the one that counts; the rest is fallout */
  if (!self->failed) {
    self->failed = true;
    why.into(self->error, sizeof self->error);
  }

  return -1;
}

const char *migrator_t__dialect(migrator_t *self) {
  return self->driver->name;
}

/* ------------------------------------------------------------------ */
/* what is registered                                                 */
/* ------------------------------------------------------------------ */

static dbm_migration_t[] migrations;

/**
 * `/path/to/migrations/20261008120000-add-pets.c` is recorded as
 * `/20261008120000-add-pets`, which is what node db-migrate writes into the
 * same table - so a database it migrated is one this can carry on.
 */
static bool nameOf(const char *file, char *into, size_t room) {

  const char *base = strrchr(file, '/');
  size_t length;

  base = base != NULL ? base + 1 : file;
  length = strlen(base);

  /* a migration in code, or one in SQL named by its up file */
  if (length > 2 && strcmp(base + length - 2, ".c") == 0)
    length -= 2;
  else if (length > 7 && strcmp(base + length - 7, "-up.sql") == 0)
    length -= 7;

  if (length + 2 > room) {
    dbmSay(stderr, TEXT`db-migrate: the name of ${file} is too long to record\n`);
    return false;
  }

  into[0] = '/';
  memcpy(into + 1, base, length);
  into[length + 1] = '\0';
  return true;
}

static dbm_migration_t *migrationNamed(const char *name) {
  return migrations.find(entry, strcmp(entry->name, name) == 0);
}

/**
 * From a migration's own DBM_MIGRATION. One the launcher registered lazily
 * already has its entry, and this fills in its steps rather than adding a
 * second migration of the same name.
 */
void dbmRegister(const char *file, dbm_step_t up, dbm_step_t down) {

  dbm_migration_t entry;

  memset(&entry, 0, sizeof entry);

  if (!nameOf(file, entry.name, sizeof entry.name))
    return;

  dbm_migration_t *known = migrationNamed(entry.name);

  if (known != NULL) {
    known->up = up;
    known->down = down;
    return;
  }

  entry.up = up;
  entry.down = down;
  migrations.push(entry);
}

void dbmRegisterLazily(const char *file, dbm_load_t load) {

  dbm_migration_t entry;

  memset(&entry, 0, sizeof entry);

  if (!nameOf(file, entry.name, sizeof entry.name))
    return;

  /* compiled into this program already: nothing to load */
  if (migrationNamed(entry.name) != NULL)
    return;

  dbmWrite(entry.file, sizeof entry.file, TEXT`${file}`);
  entry.load = load;
  migrations.push(entry);
}

bool dbmLoaded(const dbm_migration_t *migration) {

  if (migration->load == NULL || migration->up != NULL ||
      migration->down != NULL || migration->upSql != NULL)
    return true;

  if (!migration->load(migration->file))
    return false;

  /* its DBM_MIGRATION filled this very entry in, if the name agreed */
  if (migration->up == NULL && migration->down == NULL &&
      migration->upSql == NULL) {
    dbmSay(stderr, TEXT`[ERROR] ${migration->file} was loaded and registered nothing - it needs a DBM_MIGRATION(up, down)\n`);
    return false;
  }

  return true;
}

static int byName(const void *a, const void *b) {
  return strcmp(((const dbm_migration_t *)a)->name,
                ((const dbm_migration_t *)b)->name);
}

void dbmRegisterSql(const char *file, const char *up, const char *down) {

  dbm_migration_t entry;

  memset(&entry, 0, sizeof entry);

  if (!nameOf(file, entry.name, sizeof entry.name))
    return;

  dbm_migration_t *known = migrationNamed(entry.name);

  if (known == NULL) {
    migrations.push(entry);
    known = migrationNamed(entry.name);
  }

  if (known == NULL)
    return;

  free(known->upSql);
  free(known->downSql);
  known->upSql = up != NULL ? strdup(up) : NULL;
  known->downSql = down != NULL ? strdup(down) : NULL;
}

/** A whole file, or NULL. The caller frees it. */
static char *slurp(const char *path) {

  FILE *file = fopen(path, "rb");
  char *text = NULL;
  long length;

  if (file == NULL)
    return NULL;

  if (fseek(file, 0, SEEK_END) == 0 && (length = ftell(file)) >= 0 &&
      fseek(file, 0, SEEK_SET) == 0 && (text = malloc((size_t)length + 1))) {

    size_t got = fread(text, 1, (size_t)length, file);

    text[got] = '\0';
  }

  fclose(file);
  return text;
}

bool dbmLoadSqlFiles(const char *upFile) {

  char downFile[1100];
  size_t length = strlen(upFile);

  if (length < 7 || strcmp(upFile + length - 7, "-up.sql") != 0) {
    dbmSay(stderr, TEXT`[ERROR] ${upFile} is not an -up.sql file\n`);
    return false;
  }

  dbmWrite(downFile, sizeof downFile, TEXT`${upFile}`);
  dbmWrite(downFile + length - 7, sizeof downFile - (length - 7),
           TEXT`-down.sql`);

  char *up = slurp(upFile);
  char *down = slurp(downFile);

  if (up == NULL) {
    dbmSay(stderr, TEXT`[ERROR] cannot read ${upFile}\n`);
    free(down);
    return false;
  }

  dbmRegisterSql(upFile, up, down);
  free(up);
  free(down);
  return true;
}

const dbm_migration_t *dbmMigrations(size_t *count) {

  qsort(migrations.items, migrations.count, sizeof(dbm_migration_t), byName);

  *count = migrations.count;
  return migrations.items;
}

void dbmForgetMigrations(void) {

  for (entry in migrations) {
    free(entry->upSql);
    free(entry->downSql);
  }

  migrations.release();
}

typedef struct {
  const char *name;
  dbm_open_t open;
} dbm_driver_entry_t;

static dbm_driver_entry_t[] drivers;

void dbmRegisterDriver(const char *name, dbm_open_t open) {
  drivers.push((dbm_driver_entry_t){name, open});
}

/**
 * The names node db-migrate accepts for the same driver. A database.json
 * written for it says `postgres` as often as `pg`.
 */
static const char *canonical(const char *name) {

  if (name in {"postgres", "postgresql"})
    return "pg";

  if (strcmp(name, "sqlite") == 0)
    return "sqlite3";

  return name;
}

const char *dbmDriverDirectory = NULL;

static dbm_open_t registered(const char *name) {

  for (entry in drivers)
    if (strcmp(entry->name, name) == 0)
      return entry->open;

  return NULL;
}

/**
 * A driver that is not in this program, loaded from where the launcher keeps
 * them. Its constructor registers it, so asking again afterwards finds it.
 *
 * Only the development launcher sets the directory. A program with its
 * migrations compiled in links the drivers it uses and never loads one -
 * which is the point of it: libpq is wanted only where PostgreSQL is.
 */
static dbm_open_t loaded(const char *name, char *why, size_t room) {

  char path[1024];

  if (dbmDriverDirectory == NULL)
    return NULL;

  dbmWrite(path, sizeof path, TEXT`${dbmDriverDirectory}/libdbmigrate-${name}.so`);

  if (dlopen(path, RTLD_NOW | RTLD_GLOBAL) == NULL) {
    dbmWrite(why, room, TEXT`the ${name} driver could not be loaded: ${dlerror()}`);
    return NULL;
  }

  return registered(name);
}

driver_t *dbmOpen(json_t config, char *why, size_t room) {

  const char *wanted = canonical(config.driver);
  dbm_open_t open;

  if (wanted[0] == '\0') {
    dbmWrite(why, room, TEXT`the configuration does not name a driver`);
    return NULL;
  }

  open = registered(wanted);

  if (open == NULL)
    open = loaded(wanted, why, room);

  if (open != NULL)
    return open(config, why, room);

  if (why[0] != '\0')
    return NULL;

  dbmWrite(why, room, TEXT`there is no ${wanted} driver in this program - link it, or build the launcher with it`);
  return NULL;
}

void dbmClose(driver_t *driver) {

  if (driver == NULL)
    return;

  driver->close(driver);
  free(driver);
}
