/**
 * Static seeds, as node db-migrate 1.3 has them: data for development and
 * tests that can be run again and again.
 *
 *   seeds/1-owners.c:
 *
 *     static int seed(seed_t *db) {
 *       return db->insert("owners", [{id: 1, name: "Ann"}, {id: 2, name: "Bob"}]);
 *     }
 *     DBM_SEED(seed)
 *
 * A seed inserts into tables made by v2 migrations only, because every row
 * it inserts is marked in their __dbmigrate__flag column - `seed:1-owners` -
 * and that is how it is removed again. Running seeds removes what they
 * inserted before, the last first, then runs them in name order; which
 * tables each one wrote to is kept in the state table under
 * `__dbmigrate_seeds__`, written before the rows are, so a seed that fails
 * halfway is cleaned up by the next run.
 *
 * Seeds take no migration lock, as node's do not.
 */
#include <db_migrate_driver.h>

#include <stdlib.h>
#include <string.h>

#define SEEDS "__dbmigrate_seeds__"
#define FLAG "__dbmigrate__flag"

typedef struct {
  char name[256];
  dbm_seed_fn seed;
} dbm_seed_entry_t;

static dbm_seed_entry_t[] seeds;

struct seed_t {
  driver_t *driver;
  dbm_state_t *state;
  const char *name;

  /** `__dbmigrate_seeds__`: each seed, the tables it inserted into. */
  yyjson_mut_doc *seeded;
  bool stored;

  bool dry;
  bool failed;
  char error[512];
};

void dbmRegisterSeed(const char *file, dbm_seed_fn seed) {

  dbm_seed_entry_t entry = {.seed = seed};
  const char *base = strrchr(file, '/');
  size_t length;

  base = base != NULL ? base + 1 : file;
  length = strlen(base);

  if (length > 2 && strcmp(base + length - 2, ".c") == 0)
    length -= 2;

  if (length >= sizeof entry.name)
    return;

  memcpy(entry.name, base, length);
  entry.name[length] = '\0';

  for (known in seeds)
    if (strcmp(known->name, entry.name) == 0) {
      known->seed = seed;
      return;
    }

  seeds.push(entry);
}

static int byName(const void *a, const void *b) {
  return strcmp(((const dbm_seed_entry_t *)a)->name,
                ((const dbm_seed_entry_t *)b)->name);
}

/* ------------------------------------------------------------------ */
/* the state                                                          */
/* ------------------------------------------------------------------ */

static yyjson_mut_val *rootOf(yyjson_mut_doc *doc) {
  return yyjson_mut_doc_get_root(doc);
}

static int load(seed_t *self) {

  char *text = NULL;

  if (dbmKvGet(self->state->db, self->state->table, SEEDS, &text))
    return -1;

  self->stored = text != NULL;

  yyjson_doc *read = text != NULL ? yyjson_read(text, strlen(text), 0) : NULL;

  self->seeded = read != NULL ? yyjson_doc_mut_copy(read, NULL)
                              : yyjson_mut_doc_new(NULL);
  yyjson_doc_free(read);
  free(text);

  if (!yyjson_mut_is_obj(rootOf(self->seeded)))
    yyjson_mut_doc_set_root(self->seeded, yyjson_mut_obj(self->seeded));

  return 0;
}

/** Written as it is now - except on a dry run, which writes no state. */
static int store(seed_t *self) {

  if (self->dry)
    return 0;

  char *text = yyjson_mut_write(self->seeded, 0, NULL);
  int answer = text == NULL
                   ? -1
                   : self->stored
                         ? dbmKvUpdate(self->state->db, self->state->table,
                                       SEEDS, text)
                         : dbmKvInsert(self->state->db, self->state->table,
                                       SEEDS, text);

  free(text);

  if (answer == 0)
    self->stored = true;

  return answer;
}

static int fail(seed_t *self, text_t why) {

  if (!self->failed) {
    why.into(self->error, sizeof self->error);
    self->failed = true;
  }

  return -1;
}

int seed_t__fail(seed_t *self, text_t why) {
  return fail(self, why);
}

/* ------------------------------------------------------------------ */
/* what a seed calls                                                  */
/* ------------------------------------------------------------------ */

/** A table of the learned schema, with the flag column, or NULL. */
static yyjson_mut_val *flaggedTable(seed_t *self, const char *table) {

  yyjson_mut_doc *schema = self->state->schema;
  yyjson_mut_val *spec = yyjson_mut_obj_get(
      yyjson_mut_obj_get(yyjson_mut_doc_get_root(schema), "c"), table);
  yyjson_mut_val *columns = yyjson_mut_obj_get(spec, "columns");

  if (columns == NULL)
    columns = spec;

  return yyjson_mut_obj_get(columns, FLAG) != NULL ? spec : NULL;
}

static bool inSchema(seed_t *self, const char *table) {

  yyjson_mut_doc *schema = self->state->schema;

  return yyjson_mut_obj_get(
             yyjson_mut_obj_get(yyjson_mut_doc_get_root(schema), "c"),
             table) != NULL;
}

/** The table among those this seed inserted into - written down first. */
static int remembered(seed_t *self, const char *table) {

  yyjson_mut_val *tables = yyjson_mut_obj_get(rootOf(self->seeded), self->name);

  for (size_t i = 0; i < yyjson_mut_arr_size(tables); ++i)
    if (strcmp(yyjson_mut_get_str(yyjson_mut_arr_get(tables, i)), table) == 0)
      return 0;

  yyjson_mut_arr_add_strcpy(self->seeded, tables, table);

  if (store(self))
    return fail(self, TEXT`could not write the state: ${self->state->db->error}`);

  return 0;
}

static int insertRows(seed_t *self, const char *table, json_t rows) {

  if (self->failed)
    return -1;

  if (flaggedTable(self, table) == NULL)
    return fail(self, TEXT`Seed "${self->name}" can not insert into "${table}", seeds insert into tables created by v2 migrations only, by their __dbmigrate__flag column, so the rows can be removed again.`);

  if (rows.count() == 0)
    return 0;

  if (remembered(self, table))
    return -1;

  char flag[300];

  dbmWrite(flag, sizeof flag, TEXT`seed:${self->name}`);

  for (int i = 0; i < rows.count(); ++i) {

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *row = yyjson_mut_val_mut_copy(doc, rows.at(i).node);

    yyjson_mut_obj_remove_key(row, FLAG);
    yyjson_mut_obj_add_strcpy(doc, row, FLAG, flag);
    yyjson_mut_doc_set_root(doc, row);

    json_t marked = meta_jsonFromMut(doc);
    int answer = self->driver->insert(self->driver, table, marked);

    marked.release();

    if (answer != 0)
      return fail(self, TEXT`${self->driver->error}`);
  }

  return 0;
}

int seed_t__insert(seed_t *self, const char *table, json_t rows) {

  json_t list = dbmRowsOf(rows, meta_toJSON("null"), self->error,
                          sizeof self->error);
  defer list.release();

  if (self->error[0] != '\0' && !self->failed) {
    self->failed = true;
    return -1;
  }

  return insertRows(self, table, list);
}

int seed_t__insertColumns(seed_t *self, const char *table, json_t columns,
                          json_t values) {

  json_t list = dbmRowsOf(columns, values, self->error, sizeof self->error);
  defer list.release();

  if (self->error[0] != '\0' && !self->failed) {
    self->failed = true;
    return -1;
  }

  return insertRows(self, table, list);
}

json_t seed_t__all(seed_t *self, sql_t query) {

  json_t rows = meta_toJSON("[]");

  if (self->failed)
    return rows;

  rows.release();

  if (dbmQuery(self->driver, &query, &rows) != 0) {
    fail(self, TEXT`${self->driver->error}`);
    return meta_toJSON("[]");
  }

  return rows;
}

/* ------------------------------------------------------------------ */
/* running them                                                       */
/* ------------------------------------------------------------------ */

/** What a seed inserted, removed - its tables last first - and forgotten. */
static int removeSeeded(seed_t *self, const char *name) {

  yyjson_mut_val *tables = yyjson_mut_obj_get(rootOf(self->seeded), name);
  char flag[300];

  dbmWrite(flag, sizeof flag, TEXT`seed:${name}`);

  for (size_t i = yyjson_mut_arr_size(tables); i > 0; --i) {

    const char *table = yyjson_mut_get_str(yyjson_mut_arr_get(tables, i - 1));

    /* a table the migrations dropped since has nothing left to remove */
    if (table == NULL || !inSchema(self, table))
      continue;

    dbm_text_t head = {0};
    defer head.release();

    head.put("DELETE FROM ");
    self->driver->quoteName(self->driver, &head, table);
    head.put(" WHERE ");
    self->driver->quoteName(self->driver, &head, FLAG);
    head.put(" = ");

    sql_t raw = sqlRaw(head.failed ? "" : head.text);
    sql_t query = SQL`${&raw}${(const char *)flag}`;
    int answer = dbmQuery(self->driver, &query, NULL);

    query.release();
    raw.release();

    if (answer != 0)
      return fail(self, TEXT`${self->driver->error}`);
  }

  yyjson_mut_obj_remove_key(rootOf(self->seeded), name);
  return 0;
}

/** The seeded names, the last first. */
static size_t seededNames(seed_t *self, const char **into, size_t room) {

  yyjson_mut_val *root = rootOf(self->seeded);
  size_t count = yyjson_mut_obj_size(root);
  size_t at = 0;
  yyjson_mut_obj_iter walk;
  yyjson_mut_val *key;

  yyjson_mut_obj_iter_init(root, &walk);

  while ((key = yyjson_mut_obj_iter_next(&walk)) != NULL && at < room)
    into[count - 1 - at++] = yyjson_mut_get_str(key);

  return at;
}

static int finish(seed_t *self, int answer, char *why, size_t room) {

  if (answer != 0 || self->failed)
    dbmWrite(why, room, TEXT`${self->error[0] != '\0' ? self->error : self->state->db->error}`);

  yyjson_mut_doc_free(self->seeded);
  return answer != 0 || self->failed ? -1 : 0;
}

int dbmSeed(driver_t *driver, dbm_state_t *state, const char *name,
            bool undo, bool dry, char *why, size_t room) {

  seed_t self = {.driver = driver, .state = state, .dry = dry};
  const char *names[256];
  size_t count;

  driver->dryRun = dry;

  if (dbmStateReloadSchema(state) || load(&self)) {
    dbmWrite(why, room, TEXT`could not read the state: ${state->db->error}`);
    return -1;
  }

  /* `seed down [name]`, `seed reset`: only removing */
  if (undo) {

    if (name != NULL &&
        yyjson_mut_obj_get(rootOf(self.seeded), name) == NULL) {
      fail(&self, TEXT`There is no seeded seed "${name}"`);
      return finish(&self, -1, why, room);
    }

    count = name != NULL ? (names[0] = name, 1)
                         : seededNames(&self, names, countof(names));

    for (size_t i = 0; i < count && !self.failed; ++i) {

      char removing[256];

      /* the name is a key of the document about to change */
      dbmWrite(removing, sizeof removing, TEXT`${names[i]}`);
      dbmSay(stdout, TEXT`[INFO] [seed] removing ${removing}\n`);
      removeSeeded(&self, removing);
    }

    return finish(&self, self.failed ? -1 : store(&self), why, room);
  }

  qsort(seeds.items, seeds.count, sizeof(dbm_seed_entry_t), byName);

  bool found = name == NULL;

  for (entry in seeds)
    found = found || strcmp(entry->name, name) == 0;

  if (!found) {
    fail(&self, TEXT`There is no seed "${name}"`);
    return finish(&self, -1, why, room);
  }

  /* what they inserted before goes first: all of them, or just this one */
  if (name != NULL) {
    if (yyjson_mut_obj_get(rootOf(self.seeded), name) != NULL)
      removeSeeded(&self, name);
  } else {

    count = seededNames(&self, names, countof(names));

    for (size_t i = 0; i < count && !self.failed; ++i) {
      char removing[256];
      dbmWrite(removing, sizeof removing, TEXT`${names[i]}`);
      removeSeeded(&self, removing);
    }
  }

  if (self.failed || store(&self))
    return finish(&self, -1, why, room);

  for (entry in seeds) {

    if (name != NULL && strcmp(entry->name, name) != 0)
      continue;

    dbmSay(stdout, TEXT`[INFO] [seed] ${entry->name}\n`);

    /* its tables start over, and it moves to the end */
    self.name = entry->name;
    yyjson_mut_obj_remove_key(rootOf(self.seeded), entry->name);
    yyjson_mut_obj_add(rootOf(self.seeded),
                       yyjson_mut_strcpy(self.seeded, entry->name),
                       yyjson_mut_arr(self.seeded));

    int answer = entry->seed(&self);

    if (answer != 0 && !self.failed)
      fail(&self, TEXT`the seed answered non-zero without saying why`);

    if (self.failed)
      return finish(&self, -1, why, room);

    if (store(&self))
      return finish(&self, -1, why, room);
  }

  return finish(&self, 0, why, room);
}
