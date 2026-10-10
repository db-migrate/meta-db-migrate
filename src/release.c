/**
 * Releases and the deprecation of tables and columns: node db-migrate's
 * lib/release.js, since 1.5.0.
 *
 * A migration starts a release with `release` in its _meta - any label,
 * only their order counts. Those after it without a label belong to it,
 * those before the first label to release 0.
 *
 * db->deprecateTable and db->deprecateColumn mark a table or column in the
 * schema's `d`, in the release of their migration R. When a migration of a
 * later release is about to run, what is due runs before it, as a v2
 * migration of its own named `__dbmigrate_release__:<label>`:
 *
 *   R+1   renamed to __dbm_deprecated_<name>_<time>, so whatever still uses
 *         it fails while the data is still there
 *   R+N   dropped, N = releases (4 by default), with drop "auto" only; with
 *         "manual" (the default) it is said to be due until a migration
 *         drops it with db->dropDeprecated()
 *
 * The rows a dml migration deleted in soft mode with { purge } are purged
 * at the start of a release the same way, and the backups of dml migrations
 * dropped. Reverting the first migration of a release reverts the steps run
 * before it - unless rows were purged, which nothing brings back.
 */
#include <db_migrate_driver.h>

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* options                                                            */
/* ------------------------------------------------------------------ */

/** A value as node's template writes it into a message: `${value}`. */
static void shownValue(json_t value, char *into, size_t room) {

  if (strcmp(value.kind(), "string") == 0) {
    dbmWrite(into, room, TEXT`${value.text()}`);
    return;
  }

  char *text = value.node != NULL ? yyjson_mut_val_write(value.node, 0, NULL)
                                  : NULL;

  dbmWrite(into, room, TEXT`${text != NULL ? text : "undefined"}`);
  free(text);
}

int dbmReleaseOptions(json_t options, yyjson_mut_doc *doc, yyjson_mut_val *into,
                      char *why, size_t room) {

  json_t releases = options.get("releases");
  json_t drop = options.get("drop");
  char said[128];

  if (releases.node != NULL) {

    const char *kind = releases.kind();
    double n = strcmp(kind, "number") == 0   ? releases.real()
               : strcmp(kind, "string") == 0 ? atof(releases.text())
                                             : 0;

    if (n < 1 || n != (double)(long)n ||
        (strcmp(kind, "string") == 0 &&
         strspn(releases.text(), "0123456789") != strlen(releases.text()))) {
      shownValue(releases, said, sizeof said);
      dbmWrite(why, room, TEXT`releases is the number of releases until dropping, at least 1, not ${said}`);
      return -1;
    }

    if (into != NULL)
      yyjson_mut_obj_add_int(doc, into, "releases", (long)n);
  }

  if (drop.node != NULL) {

    if (strcmp(drop.kind(), "string") != 0 ||
        !(drop.text() in {"auto", "manual"})) {
      shownValue(drop, said, sizeof said);
      dbmWrite(why, room, TEXT`drop is 'auto' or 'manual', not ${said}`);
      return -1;
    }

    if (into != NULL)
      yyjson_mut_obj_add_strcpy(doc, into, "drop", drop.text());
  }

  return 0;
}

int dbmReleaseSettings(dbm_state_t *state, yyjson_mut_val *given,
                       long *releases, char drop[8], char *why, size_t room) {

  *releases = 4;
  dbmWrite(drop, 8, TEXT`manual`);

  /* the project's, in .db-migraterc, checked every time as node does */
  if (state->deprecation != NULL) {

    json_t project = meta_toJSON(state->deprecation);
    int answer = dbmReleaseOptions(project, NULL, NULL, why, room);

    if (answer == 0) {
      if (strcmp(project.get("releases").kind(), "number") == 0)
        *releases = (long)project.get("releases").real();
      else if (strcmp(project.get("releases").kind(), "string") == 0)
        *releases = atol(project.get("releases").text());
      if (strcmp(project.get("drop").kind(), "string") == 0)
        dbmWrite(drop, 8, TEXT`${project.get("drop").text()}`);
    }

    project.release();

    if (answer != 0)
      return -1;
  }

  /* and the entry's own */
  yyjson_mut_val *n = yyjson_mut_obj_get(given, "releases");
  yyjson_mut_val *d = yyjson_mut_obj_get(given, "drop");

  if (n != NULL && yyjson_mut_is_num(n))
    *releases = (long)yyjson_mut_get_num(n);

  if (d != NULL && yyjson_mut_is_str(d))
    dbmWrite(drop, 8, TEXT`${yyjson_mut_get_str(d)}`);

  return 0;
}

long dbmReleaseAge(dbm_state_t *state, long index, const char *release) {

  if (release == NULL || state->releaseIndex == NULL)
    return index;

  json_t known = meta_toJSON(state->releaseIndex);
  json_t at = known.get(release);
  long made = strcmp(at.kind(), "number") == 0 ? at.number() : 0;

  known.release();
  return index - made;
}

/* ------------------------------------------------------------------ */
/* deprecations                                                       */
/* ------------------------------------------------------------------ */

void dbmHiddenName(const char *name, const char *migration, char *into,
                   size_t room) {

  char time[64];
  size_t length = strcspn(migration, "-");

  dbmWrite(time, length + 1 < sizeof time ? length + 1 : sizeof time,
           TEXT`${migration}`);

  if (strlen(name) + strlen(time) + strlen("__dbm_deprecated__") <= 60) {
    dbmWrite(into, room, TEXT`__dbm_deprecated_${name}_${time}`);
    return;
  }

  char hex[65];

  dbmSha256(name, strlen(name), hex);
  hex[16] = '\0';
  dbmWrite(into, room, TEXT`__dbm_deprecated_${hex}_${time}`);
}

static yyjson_mut_val *columnsOf(yyjson_mut_val *table) {
  yyjson_mut_val *columns = yyjson_mut_obj_get(table, "columns");
  return columns != NULL ? columns : table;
}

/** One deprecation, if what it names is there, under one name or the other. */
static int listed(dbm_state_t *state, long index, const char *t, const char *c,
                  yyjson_mut_val *entry, yyjson_mut_val *from,
                  dbm_deprecated_t *into, char *why, size_t room) {

  const char *to = yyjson_mut_get_str(yyjson_mut_obj_get(entry, "to"));
  yyjson_mut_val *r = yyjson_mut_obj_get(entry, "r");
  bool renamed = to != NULL && yyjson_mut_obj_get(from, to) != NULL;

  if (!renamed && yyjson_mut_obj_get(from, c != NULL ? c : t) == NULL)
    return 0;

  memset(into, 0, sizeof *into);
  into->column = c != NULL;
  into->renamed = renamed;
  dbmWrite(into->t, sizeof into->t, TEXT`${t}`);
  dbmWrite(into->c, sizeof into->c, TEXT`${c != NULL ? c : ""}`);
  dbmWrite(into->to, sizeof into->to, TEXT`${to != NULL ? to : ""}`);
  dbmWrite(into->name, sizeof into->name,
           TEXT`${renamed ? to : c != NULL ? c : t}`);

  if (r != NULL && yyjson_mut_is_str(r)) {
    into->hasRelease = true;
    dbmWrite(into->r, sizeof into->r, TEXT`${yyjson_mut_get_str(r)}`);
  }

  into->age = dbmReleaseAge(state, index, into->hasRelease ? into->r : NULL);

  if (dbmReleaseSettings(state, yyjson_mut_obj_get(entry, "o"),
                         &into->releases, into->drop, why, room))
    return -1;

  return 1;
}

long dbmDeprecations(dbm_state_t *state, long index, dbm_deprecated_t *into,
                     size_t room, char *why, size_t whyRoom) {

  yyjson_mut_val *schema = yyjson_mut_doc_get_root(state->schema);
  yyjson_mut_val *c = yyjson_mut_obj_get(schema, "c");
  yyjson_mut_val *d = yyjson_mut_obj_get(schema, "d");
  yyjson_mut_obj_iter walk;
  yyjson_mut_val *key;
  size_t count = 0;

  yyjson_mut_obj_iter_init(yyjson_mut_obj_get(d, "tables"), &walk);

  while (count < room && (key = yyjson_mut_obj_iter_next(&walk)) != NULL) {

    int found = listed(state, index, yyjson_mut_get_str(key), NULL,
                       yyjson_mut_obj_iter_get_val(key), c, &into[count], why,
                       whyRoom);

    if (found < 0)
      return -1;

    count += (size_t)found;
  }

  yyjson_mut_obj_iter_init(yyjson_mut_obj_get(d, "columns"), &walk);

  while (count < room && (key = yyjson_mut_obj_iter_next(&walk)) != NULL) {

    const char *t = yyjson_mut_get_str(key);
    yyjson_mut_val *columns = columnsOf(yyjson_mut_obj_get(c, t));
    yyjson_mut_obj_iter each;
    yyjson_mut_val *column;

    yyjson_mut_obj_iter_init(yyjson_mut_obj_iter_get_val(key), &each);

    while (count < room && (column = yyjson_mut_obj_iter_next(&each)) != NULL) {

      int found = listed(state, index, t, yyjson_mut_get_str(column),
                         yyjson_mut_obj_iter_get_val(column), columns,
                         &into[count], why, whyRoom);

      if (found < 0)
        return -1;

      count += (size_t)found;
    }
  }

  return (long)count;
}

/** `table "pets"`, `column "name" of "pets"`. */
static void describe(const dbm_deprecated_t *item, char *into, size_t room) {
  if (item->column)
    dbmWrite(into, room, TEXT`column "${item->c}" of "${item->t}"`);
  else
    dbmWrite(into, room, TEXT`table "${item->t}"`);
}

/* ------------------------------------------------------------------ */
/* the steps of a release                                             */
/* ------------------------------------------------------------------ */

/** What the release migration does: rename these, then drop those. */
static dbm_deprecated_t renames[256];
static dbm_deprecated_t drops[256];
static size_t renameCount;
static size_t dropCount;

static int releaseMigrate(schema_t *db) {

  for (size_t i = 0; i < renameCount; ++i) {
    const dbm_deprecated_t *item = &renames[i];
    if (item->column)
      db->renameColumn(item->t, item->c, item->to);
    else
      db->renameTable(item->t, item->to);
  }

  for (size_t i = 0; i < dropCount; ++i)
    dbmSchemaDropDeprecated(db, &drops[i]);

  return db->hasFailed() ? -1 : 0;
}

/** The release migration, which no file holds and the migrations table never lists. */
static void releaseMigration(const char *label, dbm_migration_t *into) {
  memset(into, 0, sizeof *into);
  dbmWrite(into->name, sizeof into->name, TEXT`${DBM_RELEASE_PREFIX}${label}`);
  into->migrate = releaseMigrate;
}

int dbmReleaseStart(driver_t *driver, dbm_state_t *state, const char *label,
                    long index, bool fix, char *why, size_t room) {

  dbm_deprecated_t *items = calloc(512, sizeof(dbm_deprecated_t));
  long count = items != NULL
                   ? dbmDeprecations(state, index, items, 512, why, room)
                   : -1;

  if (count < 0) {
    free(items);
    return -1;
  }

  renameCount = 0;
  dropCount = 0;

  for (long i = 0; i < count; ++i) {

    dbm_deprecated_t *item = &items[i];

    if (item->age >= item->releases && strcmp(item->drop, "auto") == 0) {
      if (dropCount < countof(drops))
        drops[dropCount++] = *item;
    } else if (item->age >= 1 && !item->renamed) {
      if (renameCount < countof(renames))
        renames[renameCount++] = *item;
    }
  }

  free(items);

  /* the rows and backups due, before anything else - fix only learns */
  if (!fix && dbmDmlReleaseStart(driver, state, label, why, room))
    return -1;

  if (renameCount == 0 && dropCount == 0)
    return 0;

  dbmSay(stdout, TEXT`[INFO] [release] ${fix ? "learning" : "starting"} release ${label}\n`);

  for (size_t i = 0; i < renameCount; ++i) {
    char described[600];
    describe(&renames[i], described, sizeof described);
    dbmSay(stdout, TEXT`[INFO] [release] renaming the deprecated ${described} to ${renames[i].to}\n`);
  }

  for (size_t i = 0; i < dropCount; ++i) {
    char described[600];
    describe(&drops[i], described, sizeof described);
    dbmSay(stdout, TEXT`[INFO] [release] dropping the deprecated ${described}\n`);
  }

  if (driver->dryRun)
    return 0;

  dbm_migration_t migration;
  releaseMigration(label, &migration);

  if (fix)
    return dbmFixV2(driver, state, &migration, why, room);

  if (dbmUpV2(driver, state, &migration, why, room))
    return -1;

  return dbmEndV2(state, false);
}

int dbmReleaseRevertible(dbm_state_t *state, const char *label, char *why,
                         size_t room) {

  char name[300];
  char *row = NULL;

  dbmWrite(name, sizeof name, TEXT`${DBM_RELEASE_PREFIX}${label}`);

  if (dbmKvGet(state->db, state->table, name, &row)) {
    dbmWrite(why, room, TEXT`${state->db->error}`);
    return -1;
  }

  if (row == NULL)
    return 0;

  json_t record = meta_toJSON(row);
  json_t steps = record.get("s");
  dbm_text_t purged = {0};

  free(row);

  for (int i = 0; i < steps.count(); ++i)
    if (strcmp(steps.at(i).get("t").kind(), "number") == 0 &&
        steps.at(i).get("t").number() == 3)
      purged.append(TEXT`${purged.length > 0 ? ", " : ""}purged "${steps.at(i).get("c").at(0).text()}"`);

  record.release();

  int answer = 0;

  if (purged.length > 0) {
    dbmWrite(why, room, TEXT`Release ${label} can not be reverted, it ${purged.text} before its first migration.`);
    answer = -1;
  }

  purged.release();
  return answer;
}

int dbmReleaseRevert(driver_t *driver, dbm_state_t *state, const char *label,
                     char *why, size_t room) {

  dbm_migration_t migration;
  char *row = NULL;

  releaseMigration(label, &migration);

  if (dbmKvGet(state->db, state->table, migration.name, &row)) {
    dbmWrite(why, room, TEXT`${state->db->error}`);
    return -1;
  }

  bool recorded = row != NULL;

  free(row);

  if (!recorded || driver->dryRun)
    return 0;

  dbmSay(stdout, TEXT`[INFO] [release] reverting the steps of release ${label}\n`);
  return dbmDownV2(driver, state, &migration, why, room);
}

void dbmReleaseWarn(driver_t *driver, dbm_state_t *state) {

  char why[600] = "";
  dbm_deprecated_t *items = calloc(512, sizeof(dbm_deprecated_t));
  long count = items != NULL
                   ? dbmDeprecations(state, state->releaseCurrent, items, 512,
                                     why, sizeof why)
                   : 0;

  if (count < 0)
    dbmSay(stderr, TEXT`[ERROR] ${why}\n`);

  for (long i = 0; i < count; ++i) {

    dbm_deprecated_t *item = &items[i];
    char described[600];

    if (strcmp(item->drop, "manual") != 0 || item->age < item->releases)
      continue;

    describe(item, described, sizeof described);

    if (item->column)
      dbmSay(stderr, TEXT`[WARN] [release] the deprecated ${described} is due for dropping, deprecated ${item->age} releases ago. Drop it in a migration with db.dropDeprecated("${item->t}", "${item->c}").\n`);
    else
      dbmSay(stderr, TEXT`[WARN] [release] the deprecated ${described} is due for dropping, deprecated ${item->age} releases ago. Drop it in a migration with db.dropDeprecated("${item->t}").\n`);
  }

  free(items);
  dbmDmlWarn(driver, state);
}
