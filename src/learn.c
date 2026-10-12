/**
 * v2 migrations: node db-migrate's learn, state travel and translate.
 *
 * A v2 migration says only what it builds. Every step is learned twice over
 * before it runs: into the schema the v2 migrations have built so far -
 * `__dbmigrate_schema__`, `{i, c, f, e}` - and into the migration's own
 * record - `{i, c, f, s}`, where `s` is the list of steps that undo it and
 * `i`, `c`, `f` keep what a step removed, so the undoing can put it back.
 * Both are written to node's state table after every step, before the step
 * is sent: a process that dies halfway leaves behind exactly what it did and
 * how to take it back.
 *
 *   {"t":0,"a":"dropTable","c":["pets"]}        call it as it stands
 *   {"t":1,"a":"createTable","c":["pets"]}      rebuild it from what was kept
 *
 * `down` runs the list backwards, learning each step the other way round
 * without recording it. So does a migration that fails halfway, which has no
 * transaction around it - its rollback is this, by exactly the steps that
 * reached the database: each undo step carries the step it undoes (`n`), and
 * the lock row says which step was sent last (`done`).
 *
 * A run that dies halfway leaves the lock row saying which migration it was
 * and how far it got - started, learned, done - and the next run resumes it,
 * as node does since 1.0.0-beta.38: skipping what was done, or rolling it back
 * and starting over, as the migration says.
 *
 * Where node's learn has a bug the database would feel, this does what node
 * meant rather than what it does - the records stay node's shape either way:
 *
 * - changeColumn keeps the column as it was, not as it became, so `down`
 *   changes it back
 * - removeForeignKey records how to add the key again
 * - renameTable takes the table's keys and indexes with it rather than
 *   forgetting them
 * - the list is undone from its end, so a record interrupted halfway keeps
 *   its order
 */
#include <db_migrate_driver.h>

#include <stdlib.h>
#include <string.h>
#include <time.h>

/** The steps, by the names node records them under. */
typedef enum dbmAction {
  ACTION_CREATE_TABLE "createTable",
  ACTION_DROP_TABLE "dropTable",
  ACTION_RENAME_TABLE "renameTable",
  ACTION_ADD_COLUMN "addColumn",
  ACTION_REMOVE_COLUMN "removeColumn",
  ACTION_RENAME_COLUMN "renameColumn",
  ACTION_CHANGE_COLUMN "changeColumn",
  ACTION_ADD_INDEX "addIndex",
  ACTION_REMOVE_INDEX "removeIndex",
  ACTION_ADD_FOREIGN_KEY "addForeignKey",
  ACTION_REMOVE_FOREIGN_KEY "removeForeignKey",
  ACTION_SET_DEPRECATED "setDeprecated",
} dbm_action_t;

struct schema_t {
  driver_t *driver;
  dbm_state_t *state;

  /** This migration's record, `{i, c, f, s}`, and the key it is kept under. */
  yyjson_mut_doc *record;
  const char *key;

  /** Undoing: learn, but record nothing. */
  bool unlearn;

  /** `fix`: learn and record, but send nothing - the schema is there already. */
  bool fixing;

  /** A dry run writes no state; the SQL is printed by the driver. */
  bool dry;

  /** Whether the last step recorded was also sent, for a rollback to know. */
  bool sent;

  /** The step counted last - node's op - and the last one sent. */
  int counter;
  int done;

  /** An interrupted run being resumed: what of it is done already. */
  const dbm_interrupted_t *recovery;
  const char *name;

  /** The step being run, as the log says it: addColumn("pets", "age"). */
  char current[600];

  /**
   * Declaring an object made outside v2 migrations - db->adopt()->...: it is
   * learned and recorded, nothing is sent, and its undoing only forgets it
   * (`t:2`). And a drop of an object the schema does not know, asked for
   * with { irreversible: true }: sent, recorded as `t:3`, never undone.
   */
  bool adopting;
  bool irreversible;

  /** What db->adopt() answers: this schema, seen as adopting. */
  schema_adopt_t view;

  bool failed;
  char error[512];
};

/** What one step is about; which fields mean anything depends on the step. */
typedef struct {
  const char *table;
  const char *name;
  const char *other;
  json_t spec;
  json_t options;
  bool unique;
} step_t;

/* ------------------------------------------------------------------ */
/* JSON                                                               */
/* ------------------------------------------------------------------ */

/**
 * A value of a document this file reads and writes but does not own, as a
 * json_t: the schema is the state's and the record is the run's, and each is
 * freed by whoever made it - both reach this file as yyjson's documents,
 * because that is how the ABI hands them over.
 *
 * Built field by field, because the runtime has no way to name a value of a
 * document somebody else holds: what makes a json_t is meta_toJSON or a
 * literal, and both make a document of their own. `_shared` is the runtime's
 * word for "not this value's to free", so a release - meta's or anybody's -
 * leaves the document alone, and every key read through the view carries the
 * word on. A NULL `doc` is a value only ever read: what is written into a
 * document is allocated in the document written into.
 */
static json_t viewOf(yyjson_mut_doc *doc, yyjson_mut_val *node) {

  json_t view;

  memset(&view, 0, sizeof view);
  view._doc = doc;
  view._node = node;
  view._key = "";
  view._shared = 1;
  return view;
}

static json_t schemaOf(schema_t *self) {
  yyjson_mut_doc *doc = self->state->schema;
  return viewOf(doc, yyjson_mut_doc_get_root(doc));
}

static json_t recordOf(schema_t *self) {
  return viewOf(self->record, yyjson_mut_doc_get_root(self->record));
}

/** Whether a key is missing - a key that holds null is there. */
static bool absent(json_t value) {
  return strcmp(value.kind(), "nothing") == 0;
}

static bool isObject(json_t value) {
  return strcmp(value.kind(), "object") == 0;
}

/**
 * `parent[key] = made`, for a value made in parent's own document by objectIn
 * or arrayIn: it is linked rather than copied, where the key stands if it is
 * there. Not `parent[key] = &made`, which is what that is in json_t: `&` is
 * kept to targets released through json_t, because the pools it takes in are
 * listed on the target's note and freed by json_t's release - and the schema
 * and the record are freed by their owners with yyjson's, so a note made for
 * either would be one nobody frees.
 */
static void place(json_t parent, const char *key, json_t made) {
  yyjson_mut_obj_put(parent._node, yyjson_mut_strcpy(parent._doc, key),
                     made._node);
}

/**
 * A new object, or array, in the document `beside` is in - filled through
 * the view and then placed. Built where it will live rather than as a
 * literal, which would be a document of its own, copied in and released.
 */
static json_t objectIn(json_t beside) {
  return viewOf(beside._doc, yyjson_mut_obj(beside._doc));
}

static json_t arrayIn(json_t beside) {
  return viewOf(beside._doc, yyjson_mut_arr(beside._doc));
}

/** The object under `key`, made if it is not there. */
static json_t child(json_t parent, const char *key) {

  if (!isObject(parent[key]))
    place(parent, key, objectIn(parent));

  return parent[key];
}

/** A document someone handed over, copied into `into`'s - `{}` for nothing. */
static json_t copyIn(json_t into, json_t value) {
  return absent(value) ? objectIn(into)
                       : viewOf(into._doc, yyjson_mut_val_mut_copy(
                                               into._doc, value._node));
}

/** A column spec's `{columns: ...}` form, or the spec itself. */
static json_t columnsOf(json_t table) {

  json_t columns = table.columns;

  return isObject(columns) ? columns : table;
}

/* ------------------------------------------------------------------ */
/* failing                                                            */
/* ------------------------------------------------------------------ */

static int fail(schema_t *self, text_t why) {

  if (!self->failed) {
    self->failed = true;
    why.into(self->error, sizeof self->error);
  }

  return -1;
}

int schema_t__fail(schema_t *self, text_t why) {
  return fail(self, why);
}

bool schema_t__hasFailed(schema_t *self) {
  return self->failed;
}

const char *schema_t__lastError(schema_t *self) {
  return self->failed ? self->error : "";
}

/* ------------------------------------------------------------------ */
/* recording                                                          */
/* ------------------------------------------------------------------ */

/** One undo step, `{t, a, c}`, at the end of the record's list. */
static void record(schema_t *self, int type, dbm_action_t action,
                   const char *const *arguments, size_t count,
                   yyjson_mut_val *extra) {

  if (self->unlearn)
    return;

  /* adopted: its undoing only forgets it again */
  if (self->adopting)
    type = 2;

  yyjson_mut_doc *doc = self->record;
  yyjson_mut_val *entry = yyjson_mut_obj(doc);
  yyjson_mut_val *args = yyjson_mut_arr(doc);

  for (size_t i = 0; i < count; ++i)
    yyjson_mut_arr_add_strcpy(doc, args, arguments[i]);

  if (extra != NULL)
    yyjson_mut_arr_append(args, yyjson_mut_val_mut_copy(doc, extra));

  yyjson_mut_obj_add_int(doc, entry, "t", type);
  yyjson_mut_obj_add_str(doc, entry, "a", spellingof(enum dbmAction, action));
  yyjson_mut_obj_add_val(doc, entry, "c", args);

  /* the step it undoes, so a rollback knows which ran - node's tag, which
     its fix does not set */
  if (!self->fixing)
    yyjson_mut_obj_add_int(doc, entry, "n", self->counter);
  yyjson_mut_arr_append(recordOf(self).s._node, entry);
}

/** One undo step whose arguments are not all names: `{t, a, c: args}`. */
static void recordWith(schema_t *self, int type, dbm_action_t action,
                       yyjson_mut_val *args) {

  if (self->unlearn)
    return;

  yyjson_mut_doc *doc = self->record;
  yyjson_mut_val *entry = yyjson_mut_obj(doc);

  yyjson_mut_obj_add_int(doc, entry, "t", type);
  yyjson_mut_obj_add_str(doc, entry, "a", spellingof(enum dbmAction, action));
  yyjson_mut_obj_add_val(doc, entry, "c", args);

  if (!self->fixing)
    yyjson_mut_obj_add_int(doc, entry, "n", self->counter);
  yyjson_mut_arr_append(recordOf(self).s._node, entry);
}

/** The schema and the record, written as they are now. Nothing on a dry run. */
static int save(schema_t *self) {

  if (self->dry)
    return 0;

  char *text = yyjson_mut_write(self->record, 0, NULL);
  int answer = text == NULL || dbmStateSave(self->state, self->key, text);

  free(text);

  if (answer)
    return fail(self, TEXT`could not write the state: ${self->state->db->error}`);

  return 0;
}

/** Node's state travel: the step counted, then everything written. */
static int travel(schema_t *self) {

  if (self->dry)
    return 0;

  if (dbmStateProgress(self->state, ++self->counter, -1))
    return fail(self, TEXT`could not write the state: ${self->state->db->error}`);

  return save(self);
}

/* ------------------------------------------------------------------ */
/* learning                                                           */
/* ------------------------------------------------------------------ */

static json_t tableIn(schema_t *self, const char *table) {
  return schemaOf(self).c[table];
}

/**
 * An object the schema does not know was not made by a v2 migration but by a
 * v1 one or by hand - node's words for it say how to go on: adopt it first,
 * or, for a drop, drop it irreversibly.
 */
#define DBM_IRREVERSIBLE ", or pass { irreversible: true } to drop it without being able to revert it"

static int unknownTable(schema_t *self, const char *table, bool drop) {
  return fail(self, TEXT`The table "${table}" is unknown to the schema of db-migrate, it was not created by a v2 migration. Declare it first with db.adopt.createTable("${table}", columns)${drop ? DBM_IRREVERSIBLE : ""}.`);
}

static int unknownColumn(schema_t *self, const char *table, const char *column,
                         bool drop) {
  return fail(self, TEXT`The column "${column}" of "${table}" is unknown to the schema of db-migrate, it was not created by a v2 migration. Declare it first with db.adopt.addColumn("${table}", "${column}", spec)${drop ? DBM_IRREVERSIBLE : ""}.`);
}

static int unknownIndex(schema_t *self, const char *table, const char *index) {
  return fail(self, TEXT`The index "${index}" of "${table}" is unknown to the schema of db-migrate, it was not created by a v2 migration. Declare it first with db.adopt.addIndex("${table}", "${index}", columns).`);
}

static int unknownForeignKey(schema_t *self, const char *table,
                             const char *key, bool drop) {
  return fail(self, TEXT`The foreign key "${key}" of "${table}" is unknown to the schema of db-migrate, it was not created by a v2 migration. Declare it first with db.adopt.addForeignKey("${table}", referencedTable, "${key}", mapping)${drop ? DBM_IRREVERSIBLE : ""}.`);
}

static bool hasColumn(schema_t *self, const char *table, const char *column) {

  json_t spec = tableIn(self, table);

  return !absent(spec) &&
         (!absent(spec[column]) || !absent(columnsOf(spec)[column]));
}

static int needTable(schema_t *self, const char *table) {
  return absent(tableIn(self, table)) ? unknownTable(self, table, false) : 0;
}

static int needColumn(schema_t *self, const char *table, const char *column) {

  if (absent(tableIn(self, table)))
    return unknownTable(self, table, false);

  return hasColumn(self, table, column)
             ? 0
             : unknownColumn(self, table, column, false);
}

/** A column name from an index's list, without the quotes node's pg driver left on it. */
static void unquoted(const char *name, char *into, size_t room) {

  size_t length = strlen(name);

  if (length >= 2 && name[0] == '"' && name[length - 1] == '"') {
    dbmWrite(into, room, TEXT`${name + 1}`);
    into[length - 2 < room ? length - 2 : room - 1] = '\0';
  } else {
    dbmWrite(into, room, TEXT`${name}`);
  }
}

/** The keys a table's columns declare, into the schema's `f`, as node keeps them. */
static void learnColumnKeys(schema_t *self, const char *table, json_t spec) {

  if (!isObject(spec))
    return;

  for (column in spec) {

    json_t foreign = column.foreignKey;

    if (!isObject(foreign))
      continue;

    json_t keys = child(child(schemaOf(self), "f"), table);
    json_t entry = objectIn(keys);
    json_t rules = foreign.rules;
    json_t mapping = foreign.mapping;

    entry.t = table;
    entry.rt = foreign.table.text();

    if (!absent(rules))
      entry.r = rules;

    if (strcmp(mapping.kind(), "string") == 0) {
      json_t m = objectIn(keys);
      m[column.keyOf()] = mapping;
      place(entry, "m", m);
    } else {
      entry.m = mapping;
    }

    place(keys, foreign.name.text(), entry);
  }
}

/** What a step does to the schema, and the step that undoes it. */
static int learn(schema_t *self, dbm_action_t action, step_t *step) {

  json_t schema = schemaOf(self);
  json_t kept = recordOf(self);
  json_t c = child(schema, "c");
  json_t f = child(schema, "f");
  json_t i = child(schema, "i");
  const char *t = step->table;

  match (action) {

  case ACTION_CREATE_TABLE: {
    c[t] = step->spec;
    learnColumnKeys(self, t, step->spec);

    const char *args[] = {t};
    record(self, 0, ACTION_DROP_TABLE, args, 1, NULL);
    return 0;
  }

  case ACTION_DROP_TABLE: {

    /* one the schema does not know: refused, or dropped beyond undoing */
    if (absent(tableIn(self, t)) && !self->unlearn) {
      if (!self->irreversible)
        return unknownTable(self, t, true);

      const char *args[] = {t};
      record(self, 3, ACTION_DROP_TABLE, args, 1, NULL);
      return 0;
    }

    const char *const parts[] = {"c", "f", "i"};

    for (size_t at = 0; at < countof(parts); ++at) {

      json_t from = schema[parts[at]];
      json_t held = from[t];

      if (!absent(held)) {
        child(kept, parts[at])[t] = held;
        from.remove(t);
      }
    }

    const char *args[] = {t};
    record(self, 1, ACTION_CREATE_TABLE, args, 1, NULL);
    return 0;
  }

  case ACTION_RENAME_TABLE: {

    const char *n = step->name;
    const char *const parts[] = {"c", "f", "i"};
    json_t d = child(schema, "d");
    const char *const kinds[] = {"tables", "columns"};

    /* deprecations move with the table - unless it is renamed for its own
       deprecation, which stays under its name */
    for (size_t at = 0; at < countof(kinds); ++at) {

      json_t marks = d[kinds[at]];
      json_t held = marks[t];
      bool own = at == 0 && !absent(held) && strcmp(held.to.text(), n) == 0;

      if (!absent(held) && !own) {
        marks[n] = held;
        marks.remove(t);
      }
    }

    for (size_t at = 0; at < countof(parts); ++at) {

      json_t part = schema[parts[at]];
      json_t held = part[t];

      if (!absent(held)) {
        part[n] = held;
        part.remove(t);
      }
    }

    const char *args[] = {n, t};
    record(self, 0, ACTION_RENAME_TABLE, args, 2, NULL);
    return 0;
  }

  case ACTION_ADD_COLUMN: {

    if (needTable(self, t))
      return -1;

    columnsOf(tableIn(self, t))[step->name] = step->spec;

    const char *args[] = {t, step->name};
    record(self, 0, ACTION_REMOVE_COLUMN, args, 2, NULL);
    return 0;
  }

  case ACTION_REMOVE_COLUMN: {

    if (!hasColumn(self, t, step->name) && !self->unlearn) {
      if (!self->irreversible)
        return absent(tableIn(self, t))
                   ? unknownTable(self, t, true)
                   : unknownColumn(self, t, step->name, true);

      const char *args[] = {t, step->name};
      record(self, 3, ACTION_REMOVE_COLUMN, args, 2, NULL);
      return 0;
    }

    if (needColumn(self, t, step->name))
      return -1;

    json_t columns = columnsOf(tableIn(self, t));
    json_t column = columns[step->name];
    bool notNull = isObject(column) && column.notNull.truth();
    const char *strategy = step->options.columnStrategy.text();

    /**
     * A NOT NULL column cannot come back without a value for the rows that
     * are there by then, so dropping one has to say how it returns.
     */
    if (notNull && !self->unlearn) {

      if (!self->driver->columnStrategies)
        return fail(self, TEXT`This driver does not support column recreation strategies.`);

      if (strategy[0] == '\0')
        return fail(self, TEXT`Can not drop a notNull column without providing a recreation strategy.`);

      if (!(strategy in {"defaultValue", "delay"}))
        return fail(self, TEXT`There is no such column recreation strategy "${strategy}!"`);
    }

    /* beside what the migration removed from this table before, not over it */
    json_t was = child(child(kept, "c"), t);

    if (notNull && !self->unlearn && strcmp(strategy, "delay") == 0) {

      was[step->name] = column;
      const char *args[] = {t, step->other, step->name};
      record(self, 0, ACTION_RENAME_COLUMN, args, 3, NULL);
    } else if (!self->unlearn) {

      was[step->name] = column;

      if (notNull)
        was[step->name].defaultValue = step->options.passthrough.defaultValue;

      const char *args[] = {t, step->name};
      record(self, 1, ACTION_ADD_COLUMN, args, 2, step->options._node);
    }

    columns.remove(step->name);
    return 0;
  }

  case ACTION_RENAME_COLUMN: {

    if (needColumn(self, t, step->name))
      return -1;

    json_t columns = columnsOf(tableIn(self, t));

    columns[step->other] = columns[step->name];
    columns.remove(step->name);
    const char *args[] = {t, step->other, step->name};
    record(self, 0, ACTION_RENAME_COLUMN, args, 3, NULL);
    return 0;
  }

  case ACTION_CHANGE_COLUMN: {

    if (needColumn(self, t, step->name))
      return -1;

    json_t columns = columnsOf(tableIn(self, t));
    json_t column = columns[step->name];

    /* undone: exactly the column as it was kept, not merged into the new */
    if (self->unlearn) {
      columns[step->name] = step->spec;
      return 0;
    }

    /* the column as it was, which node meant to keep and did not */
    child(child(kept, "c"), t)[step->name] = column;

    if (!isObject(column)) {
      json_t spelled = objectIn(columns);
      spelled.type = column;
      place(columns, step->name, spelled);
      column = spelled;
    }

    for (value in step->spec)
      column[value.keyOf()] = value;

    const char *args[] = {t, step->name};
    record(self, 1, ACTION_CHANGE_COLUMN, args, 2, NULL);
    return 0;
  }

  case ACTION_ADD_INDEX: {

    for (column in step->spec) {

      char name[256];

      unquoted(column.text(), name, sizeof name);

      if (needColumn(self, t, name))
        return -1;
    }

    json_t index = objectIn(i);

    index.t = t;
    index.c = step->spec;

    if (step->unique)
      index.u = true;

    place(child(i, t), step->name, index);
    const char *args[] = {t, step->name};
    record(self, 0, ACTION_REMOVE_INDEX, args, 2, NULL);
    return 0;
  }

  case ACTION_REMOVE_INDEX: {

    if (needTable(self, t))
      return -1;

    json_t index = i[t][step->name];

    if (absent(index))
      return unknownIndex(self, t, step->name);

    child(child(kept, "i"), t)[step->name] = index;
    i[t].remove(step->name);
    const char *args[] = {t, step->name};
    record(self, 1, ACTION_ADD_INDEX, args, 2, NULL);
    return 0;
  }

  case ACTION_ADD_FOREIGN_KEY: {

    if (needTable(self, t) || needTable(self, step->other))
      return -1;

    json_t entry = objectIn(f);

    entry.t = t;
    entry.rt = step->other;
    entry.m = step->spec;

    if (isObject(step->options) && step->options.count() > 0)
      entry.r = step->options;

    place(child(f, t), step->name, entry);
    const char *args[] = {t, step->name};
    record(self, 0, ACTION_REMOVE_FOREIGN_KEY, args, 2, NULL);
    return 0;
  }

  case ACTION_REMOVE_FOREIGN_KEY: {

    json_t key = f[t][step->name];

    if (absent(key) && !self->unlearn) {
      if (!self->irreversible)
        return absent(tableIn(self, t))
                   ? unknownTable(self, t, true)
                   : unknownForeignKey(self, t, step->name, true);

      const char *args[] = {t, step->name};
      record(self, 3, ACTION_REMOVE_FOREIGN_KEY, args, 2, NULL);
      return 0;
    }

    if (needTable(self, t))
      return -1;

    if (absent(key))
      return unknownForeignKey(self, t, step->name, false);

    child(child(kept, "f"), t)[step->name] = key;
    f[t].remove(step->name);

    /* node records nothing here, and so could not put the key back */
    const char *args[] = {t, step->name};
    record(self, 1, ACTION_ADD_FOREIGN_KEY, args, 2, NULL);
    return 0;
  }

  case ACTION_SET_DEPRECATED: {

    /* d.tables[t] or d.columns[t][c] set to the entry, or cleared */
    const char *kind = step->other;
    const char *column = step->name;
    json_t marks = child(child(schema, "d"), kind);
    json_t holder = column != NULL ? child(marks, t) : marks;
    const char *key = column != NULL ? column : t;

    json_t args = arrayIn(kept);

    /* a column of NULL and a mark that was not there are both null */
    args.push(kind);
    args.push(t);
    args.push(column);
    args.push(holder[key]);

    if (!step->spec.isNothing()) {
      holder[key] = step->spec;
    } else {
      holder.remove(key);
      if (column != NULL && holder.count() == 0)
        marks.remove(t);
    }

    recordWith(self, 2, ACTION_SET_DEPRECATED, args._node);
    return 0;
  }
  }

  return fail(self, TEXT`a step v2 does not know`);
}

/* ------------------------------------------------------------------ */
/* sending                                                            */
/* ------------------------------------------------------------------ */

/** The step itself, sent through the driver as any migration's would be. */
static int send(schema_t *self, dbm_action_t action, step_t *step) {

  driver_t *driver = self->driver;
  const char *t = step->table;
  int answer = -1;

  match (action) {

  case ACTION_CREATE_TABLE:
    answer = driver->createTable(driver, t, step->spec);
    break;

  case ACTION_DROP_TABLE:
    answer = driver->dropTable(driver, t, false);
    break;

  case ACTION_RENAME_TABLE:
    answer = driver->renameTable(driver, t, step->name);
    break;

  case ACTION_ADD_COLUMN:
    answer = driver->addColumn(driver, t, step->name, step->spec);
    break;

  case ACTION_REMOVE_COLUMN:

    /* `delay` keeps the column under another name instead of dropping it */
    answer = step->other != NULL
                 ? driver->renameColumn(driver, t, step->name, step->other)
                 : driver->removeColumn(driver, t, step->name);
    break;

  case ACTION_RENAME_COLUMN:
    answer = driver->renameColumn(driver, t, step->name, step->other);
    break;

  case ACTION_CHANGE_COLUMN:
    answer = driver->changeColumn(driver, t, step->name, step->spec);
    break;

  case ACTION_ADD_INDEX: {

    /* written without the quotes node's pg driver left in its state */
    json_t columns = [];

    for (column in step->spec) {

      char name[256];

      unquoted(column.text(), name, sizeof name);
      columns.push(name);
    }

    answer = driver->addIndex(driver, t, step->name, columns, step->unique);

    /* a driver is called through a pointer, which meta reads as keeping */
    columns.release();
    break;
  }

  case ACTION_REMOVE_INDEX:
    answer = driver->removeIndex(driver, t, step->name);
    break;

  case ACTION_ADD_FOREIGN_KEY: {

    /* a key kept without rules is added with none: `{}` */
    answer = driver->addForeignKey(driver, t, step->other, step->name,
                                   step->spec,
                                   copyIn(schemaOf(self), step->options));
    break;
  }

  case ACTION_REMOVE_FOREIGN_KEY:
    answer = driver->removeForeignKey(driver, t, step->name);
    break;

  /* learned only: nothing to send */
  case ACTION_SET_DEPRECATED:
    answer = 0;
    break;
  }

  if (answer != 0)
    return fail(self, TEXT`${driver->error}`);

  return 0;
}

/**
 * One step of a migration going up: counted and written, learned, written
 * again with its undo step in it, and only then sent - node's order, so a
 * process that dies at any point leaves a record that says how far it got.
 */
/** A field of the lock row set to a step - "learned", "done" - unless dry. */
static int mark(schema_t *self, const char *field, int op) {

  char changes[64];

  if (self->dry)
    return 0;

  dbmWrite(changes, sizeof changes, TEXT`{"${field}":${(long)op}}`);

  if (dbmStateMark(self->state, changes))
    return fail(self, TEXT`could not write the state: ${self->state->db->error}`);

  return 0;
}

/** A step for the log, as node writes it: addColumn("pets", "age"). */
static void described(dbm_action_t action, const step_t *step, char *into,
                      size_t room) {
  dbmWrite(into, room,
           TEXT`${spellingof(enum dbmAction, action)}("${step->table != NULL ? step->table : ""}"${step->name != NULL ? ", \"" : ""}${step->name != NULL ? step->name : ""}${step->name != NULL ? "\"" : ""})`);
}

static int perform(schema_t *self, dbm_action_t action, step_t *step) {

  if (self->failed)
    return -1;

  /* node's fix chain: learned first, then the step counted and written */
  if (self->fixing)
    return learn(self, action, step) || travel(self) ? -1 : 0;

  int op = self->counter + 1;
  const dbm_interrupted_t *recovery = self->recovery;
  char *what = self->current;

  described(action, step, what, sizeof self->current);

  /* resuming: what the interrupted run did is in the database and the state */
  if (recovery != NULL && op <= recovery->done) {
    self->counter = op;
    self->done = op;
    dbmSay(stdout, TEXT`[INFO] [recovery] ${self->name}: skipping already executed step ${(long)op}/${recovery->done} ${what}\n`);
    return 0;
  }

  /* its undoing was recorded, but it never finished on the database */
  bool learned = recovery != NULL && op <= recovery->learned;

  if (learned)
    dbmSay(stdout, TEXT`[INFO] [recovery] ${self->name}: executing interrupted step ${(long)op} ${what} without learning it again\n`);

  if (travel(self) || (!learned && learn(self, action, step)) || save(self) ||
      mark(self, "learned", op))
    return -1;

  /* adopted: it is there already, made by somebody else; a mark is learned only */
  if (self->adopting || action == ACTION_SET_DEPRECATED) {
    self->done = op;
    return mark(self, "done", op);
  }

  self->sent = false;
  self->driver->signaled = false;

  if (send(self, action, step))
    return -1;

  self->sent = true;

  /* in memory first: a failing write must not hide that the step ran */
  self->done = op;
  return mark(self, "done", op);
}

/* ------------------------------------------------------------------ */
/* what a v2 migration calls                                          */
/* ------------------------------------------------------------------ */

int schema_t__createTable(schema_t *self, const char *table, json_t spec) {

  step_t step = {.table = table, .spec = copyIn(schemaOf(self), spec)};

  /* node's convention: every table a v2 migration makes carries this column -
     one adopted was not made by one, and has what it has */
  if (!self->adopting && absent(step.spec["__dbmigrate__flag"])) {
    json_t flag = objectIn(step.spec);
    flag.type = "string";
    place(step.spec, "__dbmigrate__flag", flag);
  }

  return perform(self, ACTION_CREATE_TABLE, &step);
}

/** Whether options say { irreversible: true } - only `true` itself does. */
static bool irreversibleIn(json_t options) {
  return strcmp(options.irreversible.kind(), "bool") == 0 &&
         options.irreversible.truth();
}

/** A drop, refused or irreversible for an object the schema does not know. */
static int dropping(schema_t *self, dbm_action_t action, step_t *step,
                    json_t options) {

  self->irreversible = irreversibleIn(options);

  int answer = perform(self, action, step);

  self->irreversible = false;
  return answer;
}

int schema_t__dropTable(schema_t *self, const char *table) {
  return self->dropTableWith(table, {});
}

int schema_t__dropTableWith(schema_t *self, const char *table, json_t options) {
  step_t step = {.table = table};
  return dropping(self, ACTION_DROP_TABLE, &step, options);
}

int schema_t__renameTable(schema_t *self, const char *from, const char *to) {
  step_t step = {.table = from, .name = to};
  return perform(self, ACTION_RENAME_TABLE, &step);
}

int schema_t__addColumn(schema_t *self, const char *table, const char *column,
                        json_t spec) {
  step_t step = {.table = table, .name = column,
                 .spec = copyIn(schemaOf(self), spec)};
  return perform(self, ACTION_ADD_COLUMN, &step);
}

int schema_t__removeColumnWith(schema_t *self, const char *table,
                               const char *column, json_t options) {

  char renamed[300];
  step_t step = {.table = table, .name = column,
                 .options = copyIn(recordOf(self), options)};

  /* `delay` renames it out of the way, under node's name for that */
  if (strcmp(step.options.columnStrategy.text(), "delay") == 0) {

    const char *chosen = step.options.passthrough.column;

    if (chosen[0] != '\0') {
      dbmWrite(renamed, sizeof renamed, TEXT`${chosen}`);
    } else {
      char date[40];
      time_t now = time(NULL);
      struct tm utc;

      gmtime_r(&now, &utc);
      strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S.000Z", &utc);
      dbmWrite(renamed, sizeof renamed, TEXT`__dbmrn_${column}_${date}__`);
    }

    step.other = renamed;
  }

  return dropping(self, ACTION_REMOVE_COLUMN, &step, options);
}

int schema_t__removeColumn(schema_t *self, const char *table,
                           const char *column) {
  return self->removeColumnWith(table, column, {});
}

int schema_t__renameColumn(schema_t *self, const char *table, const char *from,
                           const char *to) {
  step_t step = {.table = table, .name = from, .other = to};
  return perform(self, ACTION_RENAME_COLUMN, &step);
}

int schema_t__changeColumn(schema_t *self, const char *table,
                           const char *column, json_t spec) {
  step_t step = {.table = table, .name = column,
                 .spec = copyIn(schemaOf(self), spec)};
  return perform(self, ACTION_CHANGE_COLUMN, &step);
}

static int addIndex(schema_t *self, const char *table, const char *name,
                    json_t columns, bool unique) {

  step_t step = {.table = table, .name = name, .unique = unique};

  /* `"name"` and `["name"]` are the same index, kept as the second */
  if (strcmp(columns.kind(), "string") == 0) {
    step.spec = arrayIn(schemaOf(self));
    step.spec.push(columns.text());
  } else {
    step.spec = copyIn(schemaOf(self), columns);
  }

  return perform(self, ACTION_ADD_INDEX, &step);
}

int schema_t__addIndex(schema_t *self, const char *table, const char *name,
                       json_t columns) {
  return addIndex(self, table, name, columns, false);
}

int schema_t__addUniqueIndex(schema_t *self, const char *table,
                             const char *name, json_t columns) {
  return addIndex(self, table, name, columns, true);
}

int schema_t__removeIndex(schema_t *self, const char *table, const char *name) {
  step_t step = {.table = table, .name = name};
  return perform(self, ACTION_REMOVE_INDEX, &step);
}

int schema_t__addForeignKey(schema_t *self, const char *table,
                            const char *referenced, const char *name,
                            json_t mapping, json_t rules) {

  step_t step = {.table = table, .name = name, .other = referenced};

  step.spec = copyIn(schemaOf(self), mapping);
  step.options = copyIn(schemaOf(self), rules);
  return perform(self, ACTION_ADD_FOREIGN_KEY, &step);
}

int schema_t__removeForeignKey(schema_t *self, const char *table,
                               const char *name) {
  return self->removeForeignKeyWith(table, name, {});
}

int schema_t__removeForeignKeyWith(schema_t *self, const char *table,
                                   const char *name, json_t options) {
  step_t step = {.table = table, .name = name};
  return dropping(self, ACTION_REMOVE_FOREIGN_KEY, &step, options);
}

/* ------------------------------------------------------------------ */
/* deprecating, for a later release                                   */
/* ------------------------------------------------------------------ */

int dbmSchemaSetDeprecated(schema_t *self, const char *kind, const char *table,
                           const char *column, yyjson_mut_val *entry) {

  step_t step = {.table = table, .name = column, .other = kind,
                 .spec = viewOf(NULL, entry)};

  return perform(self, ACTION_SET_DEPRECATED, &step);
}

int dbmSchemaDropDeprecated(schema_t *self, const dbm_deprecated_t *item) {

  if (item->column)
    self->removeColumn(item->t, item->name);
  else
    self->dropTable(item->name);

  return dbmSchemaSetDeprecated(self, item->column ? "columns" : "tables",
                                item->t, item->column ? item->c : NULL, NULL);
}

/** `{r, to, o}` for what `name` is deprecated as, in this migration's release. */
static yyjson_mut_val *deprecation(schema_t *self, const char *name,
                                   json_t options) {

  json_t entry = objectIn(schemaOf(self));
  json_t given = objectIn(entry);
  char to[300];
  char why[300];

  /* the options are checked and written by release.c, which takes yyjson's */
  if (dbmReleaseOptions(options, given._doc, given._node, why, sizeof why)) {
    fail(self, TEXT`${why}`);
    return NULL;
  }

  dbmHiddenName(name, self->name, to, sizeof to);

  entry.r = self->state->releaseLabel;
  entry.to = to;

  if (given.count() > 0)
    place(entry, "o", given);

  return entry._node;
}

int schema_t__deprecateTableWith(schema_t *self, const char *table,
                                 json_t options) {

  if (self->failed)
    return -1;

  if (absent(tableIn(self, table)))
    return unknownTable(self, table, false);

  yyjson_mut_val *entry = deprecation(self, table, options);

  return entry != NULL
             ? dbmSchemaSetDeprecated(self, "tables", table, NULL, entry)
             : -1;
}

int schema_t__deprecateTable(schema_t *self, const char *table) {
  return self->deprecateTableWith(table, {});
}

int schema_t__deprecateColumnWith(schema_t *self, const char *table,
                                  const char *column, json_t options) {

  if (self->failed)
    return -1;

  if (absent(tableIn(self, table)))
    return unknownTable(self, table, false);

  json_t spec = columnsOf(tableIn(self, table))[column];

  if (absent(spec))
    return unknownColumn(self, table, column, false);

  yyjson_mut_val *entry = deprecation(self, column, options);

  if (entry == NULL)
    return -1;

  /* the application stops writing it: NOT NULL relaxed, by the whole spec,
     as MySQL changes a column - written where notNull stands, as a key that
     is there is */
  if (isObject(spec) && spec.notNull.truth()) {

    json_t relaxed = {spec: spec};

    relaxed.spec.notNull = false;

    if (self->changeColumn(table, column, relaxed.spec) != 0)
      return -1;
  }

  return dbmSchemaSetDeprecated(self, "columns", table, column, entry);
}

int schema_t__deprecateColumn(schema_t *self, const char *table,
                              const char *column) {
  return self->deprecateColumnWith(table, column, {});
}

/**
 * The deprecations named - all that are due when `table` is NULL - dropped
 * and forgotten. One named that is not deprecated is refused.
 */
static int dropDeprecated(schema_t *self, const char *table,
                          const char *column) {

  if (self->failed)
    return -1;

  char why[600];
  dbm_deprecated_t *items = calloc(512, sizeof(dbm_deprecated_t));
  long count = items != NULL
                   ? dbmDeprecations(self->state, self->state->releaseCurrent,
                                     items, 512, why, sizeof why)
                   : -1;
  long dropped = 0;

  if (count < 0) {
    free(items);
    return fail(self, TEXT`${items != NULL ? why : "out of memory"}`);
  }

  for (long i = 0; i < count && !self->failed; ++i) {

    dbm_deprecated_t *item = &items[i];
    bool target = table == NULL
                      ? item->age >= item->releases
                      : strcmp(item->t, table) == 0 &&
                            (column == NULL
                                 ? !item->column
                                 : item->column && strcmp(item->c, column) == 0);

    if (target) {
      dbmSchemaDropDeprecated(self, item);
      ++dropped;
    }
  }

  free(items);

  if (table != NULL && dropped == 0) {
    if (column != NULL)
      return fail(self, TEXT`The column "${column}" of "${table}" is not deprecated, deprecate it first`);
    return fail(self, TEXT`The table "${table}" is not deprecated, deprecate it first`);
  }

  return self->failed ? -1 : 0;
}

int schema_t__dropDeprecated(schema_t *self) {
  return dropDeprecated(self, NULL, NULL);
}

int schema_t__dropDeprecatedTable(schema_t *self, const char *table) {
  return dropDeprecated(self, table, NULL);
}

int schema_t__dropDeprecatedColumn(schema_t *self, const char *table,
                                   const char *column) {
  return dropDeprecated(self, table, column);
}

/* ------------------------------------------------------------------ */
/* adopting what was made outside v2 migrations                       */
/* ------------------------------------------------------------------ */

schema_adopt_t *schema_t__adopt(schema_t *self) {
  self->view.db = self;
  return &self->view;
}

/** Adopting is for what the schema does not know yet, in node's words. */
static int known(schema_t *self, text_t what) {

  char described[600];

  dbmWrite(described, sizeof described, what);
  return fail(self, TEXT`${described} is known to the schema already, adopt is for objects created outside of v2 migrations only.`);
}

/** An instruction run as adopting: learned and recorded, not sent. */
#define DBM_ADOPTING(db, call)                                                 \
  ({                                                                           \
    (db)->adopting = true;                                                     \
    int adopted__ = (call);                                                    \
    (db)->adopting = false;                                                    \
    adopted__;                                                                 \
  })

int schema_adopt_t__createTable(schema_adopt_t *self, const char *table,
                                json_t spec) {

  if (self->db->failed)
    return -1;

  if (!absent(tableIn(self->db, table)))
    return known(self->db, TEXT`createTable("${table}")`);

  return DBM_ADOPTING(self->db, schema_t__createTable(self->db, table, spec));
}

int schema_adopt_t__addColumn(schema_adopt_t *self, const char *table,
                              const char *column, json_t spec) {

  if (self->db->failed)
    return -1;

  if (hasColumn(self->db, table, column))
    return known(self->db, TEXT`addColumn("${table}", "${column}")`);

  return DBM_ADOPTING(self->db,
                      schema_t__addColumn(self->db, table, column, spec));
}

int schema_adopt_t__addIndex(schema_adopt_t *self, const char *table,
                             const char *name, json_t columns) {

  if (self->db->failed)
    return -1;

  if (!absent(schemaOf(self->db).i[table][name]))
    return known(self->db, TEXT`addIndex("${table}", "${name}")`);

  return DBM_ADOPTING(self->db,
                      schema_t__addIndex(self->db, table, name, columns));
}

int schema_adopt_t__addUniqueIndex(schema_adopt_t *self, const char *table,
                                   const char *name, json_t columns) {

  if (self->db->failed)
    return -1;

  if (!absent(schemaOf(self->db).i[table][name]))
    return known(self->db, TEXT`addIndex("${table}", "${name}")`);

  return DBM_ADOPTING(self->db,
                      schema_t__addUniqueIndex(self->db, table, name, columns));
}

int schema_adopt_t__addForeignKey(schema_adopt_t *self, const char *table,
                                  const char *referenced, const char *name,
                                  json_t mapping, json_t rules) {

  if (self->db->failed)
    return -1;

  if (!absent(schemaOf(self->db).f[table][name]))
    return known(self->db, TEXT`addForeignKey("${table}", "${referenced}", "${name}")`);

  return DBM_ADOPTING(self->db,
                      schema_t__addForeignKey(self->db, table, referenced,
                                              name, mapping, rules));
}

/* ------------------------------------------------------------------ */
/* undoing                                                            */
/* ------------------------------------------------------------------ */

/** A step of the undoing: learned without being recorded, then sent. */
static int undo(schema_t *self, dbm_action_t action, step_t *step) {

  if (learn(self, action, step))
    return -1;

  return send(self, action, step);
}

/**
 * A table back as it was: from the columns kept in the record, then its
 * indexes and the keys its columns did not declare themselves.
 */
static int rebuildTable(schema_t *self, const char *t) {

  json_t kept = recordOf(self);
  json_t spec = child(kept, "c")[t];

  if (absent(spec))
    return fail(self, TEXT`the state does not say what ${t} looked like`);

  step_t create = {.table = t, .spec = spec};

  if (undo(self, ACTION_CREATE_TABLE, &create))
    return -1;

  /* the keys the columns declare were made with the table */
  json_t keys = child(kept, "f")[t];

  for (column in columnsOf(spec)) {

    json_t declared = column.foreignKey;

    if (!absent(declared))
      keys.remove(declared.name.text());
  }

  for (index in child(kept, "i")[t]) {

    step_t step = {.table = t, .name = index.keyOf(), .spec = index.c,
                   .unique = index.u.truth()};

    if (undo(self, ACTION_ADD_INDEX, &step))
      return -1;
  }

  for (key in keys) {

    step_t step = {.table = t, .name = key.keyOf(), .other = key.rt.text(),
                   .spec = key.m, .options = key.r};

    if (undo(self, ACTION_ADD_FOREIGN_KEY, &step))
      return -1;
  }

  return 0;
}

/**
 * A whole number the record holds - `t`, `n` - and 0 for anything else, as
 * it was read before: `number()` alone would also read `true` as 1 and `"3"`
 * as 3, which a record written by hand could hold.
 */
static long integerOf(json_t value) {
  return strcmp(value.kind(), "number") == 0 ? value.number() : 0;
}

/** One recorded undo step, `{t, a, c}`. */
static int undoEntry(schema_t *self, json_t entry) {

  json_t kept = recordOf(self);
  long type = integerOf(entry.t);
  const char *named = entry.a;
  json_t args = entry.c;
  int found = (int)fromspelling(enum dbmAction, named);

  if (found < 0 || !(type in {0, 1, 2}))
    return fail(self, TEXT`Invalid state record, of type ${type}: ${named}`);

  dbm_action_t action = (dbm_action_t)found;
  step_t step = {.table = args[0].text()};

  /* a mark, put back as it was before */
  if (action == ACTION_SET_DEPRECATED) {

    json_t column = args[2];

    step.other = args[0].text();
    step.table = args[1].text();
    step.name = strcmp(column.kind(), "string") == 0 ? column.text() : NULL;
    step.spec = args[3];
    return learn(self, action, &step);
  }

  /* 0 is called as it was recorded; 2, adopted, is only forgotten again */
  if (type == 0 || type == 2) {

    /* called as it was recorded */
    match (action) {
    case ACTION_RENAME_TABLE:
      step.name = args[1].text();
      break;
    case ACTION_RENAME_COLUMN:
      step.name = args[1].text();
      step.other = args[2].text();
      break;
    case ACTION_DROP_TABLE:
      break;
    case ACTION_REMOVE_COLUMN, ACTION_REMOVE_INDEX,
        ACTION_REMOVE_FOREIGN_KEY:
      step.name = args[1].text();
      break;
    default:
      return fail(self, TEXT`a ${named} step cannot be undone as it stands`);
    }

    return type == 2 ? learn(self, action, &step) : undo(self, action, &step);
  }

  /* rebuilt from what the record kept */
  step.name = args[1].text();

  match (action) {

  case ACTION_CREATE_TABLE:
    return rebuildTable(self, step.table);

  case ACTION_ADD_COLUMN, ACTION_CHANGE_COLUMN:
    step.spec = child(kept, "c")[step.table][step.name];

    if (absent(step.spec))
      return fail(self, TEXT`the state does not say what ${step.table}.${step.name} looked like`);

    return undo(self, action, &step);

  case ACTION_ADD_INDEX: {

    json_t index = child(kept, "i")[step.table][step.name];

    if (absent(index)) {
      dbmSay(stderr, TEXT`[WARN] There was an attempt to create the index "${step.name}" in the table "${step.table}". This index did not exist at the time of deleting it - recreate it by hand if it was made by hand.\n`);
      return 0;
    }

    step.spec = index.c;
    step.unique = index.u.truth();
    return undo(self, action, &step);
  }

  case ACTION_ADD_FOREIGN_KEY: {

    json_t key = child(kept, "f")[step.table][step.name];

    if (absent(key))
      return fail(self, TEXT`the state does not say what foreign key ${step.name} was`);

    step.other = key.rt.text();
    step.spec = key.m;
    step.options = key.r;
    return undo(self, action, &step);
  }

  default:
    return fail(self, TEXT`a ${named} step has nothing to be rebuilt from`);
  }
}

/**
 * The record's steps backwards, each taken off the end and the state written
 * after it, so an undoing that is interrupted can be carried on from where it
 * stopped. Which steps are in it is the caller's: keepExecuted leaves out
 * those that never reached the database.
 */
static int undoAll(schema_t *self) {

  json_t steps = recordOf(self).s;
  bool unlearn = self->unlearn;

  self->unlearn = true;

  while (steps.count() > 0) {

    if (undoEntry(self, steps[steps.count() - 1]))
      break;

    /* raw: json_t takes a key out of an object, and has no word for taking
       a position out of an array */
    yyjson_mut_arr_remove_last(steps._node);

    if (save(self))
      break;
  }

  self->unlearn = unlearn;
  return self->failed ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* running one                                                        */
/* ------------------------------------------------------------------ */

/** The key node keeps a migration's record under: its file name, nothing else. */
const char *dbmKeyOf(const char *name) {

  const char *slash = strrchr(name, '/');

  return slash != NULL ? slash + 1 : name;
}

/** The record as stored, or a new one - `{}` is what node stores at first. */
yyjson_mut_doc *dbmRecordFrom(const char *text) {

  yyjson_doc *read = text != NULL ? yyjson_read(text, strlen(text), 0) : NULL;
  yyjson_mut_doc *doc = read != NULL ? yyjson_doc_mut_copy(read, NULL)
                                     : yyjson_mut_doc_new(NULL);

  yyjson_doc_free(read);

  yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);

  if (root == NULL || !yyjson_mut_is_obj(root)) {
    root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
  }

  const char *const parts[] = {"i", "c", "f"};

  for (size_t i = 0; i < countof(parts); ++i)
    if (yyjson_mut_obj_get(root, parts[i]) == NULL)
      yyjson_mut_obj_add_val(doc, root, parts[i], yyjson_mut_obj(doc));

  if (yyjson_mut_obj_get(root, "s") == NULL)
    yyjson_mut_obj_add_val(doc, root, "s", yyjson_mut_arr(doc));

  return doc;
}

/** The step an undo entry belongs to: `n`, or 0 for one an older run wrote. */
static long stepOf(json_t entry) {
  return integerOf(entry.n);
}

/**
 * The undo entries of the steps that changed the database, the rest let go:
 * every step up to `done`, and `failed` as well if its main statement went
 * through before it failed (`signaled`). node's executedSteps.
 */
static void keepExecuted(schema_t *self, long done, long failed,
                         bool signaled) {

  json_t steps = recordOf(self).s;
  int at = 0;

  while (at < steps.count()) {

    long n = stepOf(steps[at]);

    if (n <= done || (signaled && failed > done && n == failed))
      ++at;
    else
      yyjson_mut_arr_remove(steps._node, (size_t)at); /* raw, as in undoAll */
  }
}

/** The file a migration was written in, hashed - node's `h` - or NULL. */
const char *dbmHashOf(const dbm_migration_t *migration, char hex[65]) {
  return migration->file[0] != '\0' && dbmSha256File(migration->file, hex)
             ? hex
             : NULL;
}

/**
 * How an interrupted run is resumed: the migration's own say, "skip" when it
 * says nothing - but a run interrupted while rolling back is rolled back
 * further, and steps of a file that changed since are not skipped blind.
 * NULL, with the reason, when it cannot be.
 */
static const char *recoveryOf(const dbm_migration_t *migration,
                              const dbm_interrupted_t *interrupted, char *why,
                              size_t room) {

  const char *name = dbmKeyOf(migration->name);
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
    dbmWrite(why, room, TEXT`Migration "${name}" was interrupted at step ${interrupted->step} and changed since, so the executed steps can not be skipped safely. Use DBM_MIGRATION_V2_RECOVERY(migrate, "rollback") to revert them, or repair the state manually.`);
    return NULL;
  }

  return mode;
}

/**
 * The steps of the record that cannot be undone, node's way of saying them:
 * `step 2 dropTable("junk"), step 3 ...`, or "" when there are none.
 */
static void irreversibleSteps(schema_t *self, char *into, size_t room) {

  dbm_text_t said = {0};
  defer said.release();

  into[0] = '\0';

  for (entry in recordOf(self).s) {

    if (integerOf(entry.t) != 3)
      continue;

    said.append(TEXT`${said.length > 0 ? ", " : ""}step ${stepOf(entry)} ${entry.a.text()}(`);

    json_t args = entry.c;

    for (int i = 0; i < args.count(); ++i)
      if (strcmp(args[i].kind(), "string") == 0)
        said.append(TEXT`${i > 0 ? ", " : ""}"${args[i].text()}"`);

    said.put(")");
  }

  if (said.text != NULL)
    dbmWrite(into, room, TEXT`${said.text}`);
}

/** Undo what is left in the record, as a rollback does, and end the run. */
static int rollBack(schema_t *self) {

  if (save(self) || mark(self, "rb", 1))
    return -1;

  self->failed = false;

  if (undoAll(self))
    return -1;

  if (!self->dry && (dbmStateForget(self->state, self->key) ||
                     dbmStateProgress(self->state, -1, 1)))
    return fail(self, TEXT`could not write the state: ${self->state->db->error}`);

  return 0;
}

/**
 * A v2 migration up: its record begun, its steps run and learned, and on a
 * failure everything it did so far undone from that record - there is no
 * transaction to roll back. A run a previous one left unfinished is resumed
 * first, the way the migration says. Answers 0 when it ran; the reason for
 * anything else is in `why`.
 */
int dbmUpV2(driver_t *driver, dbm_state_t *state,
            const dbm_migration_t *migration, char *why, size_t room) {

  const char *name = migration->name;
  schema_t db = {.driver = driver, .state = state, .key = dbmKeyOf(name),
                 .dry = driver->dryRun, .name = dbmKeyOf(name)};
  dbm_interrupted_t interrupted = {0};
  char hex[65];
  const char *hash = dbmHashOf(migration, hex);
  char *stored = db.dry ? NULL
                        : dbmStateBegin(state, db.key, "up", hash, &interrupted);

  if (!db.dry && stored == NULL) {
    dbmWrite(why, room, TEXT`could not begin the state of ${name}: ${state->db->error}`);
    return -1;
  }

  db.record = dbmRecordFrom(stored);
  free(stored);

  if (interrupted.found) {

    const char *mode = recoveryOf(migration, &interrupted, why, room);

    if (mode == NULL) {
      yyjson_mut_doc_free(db.record);
      return -1;
    }

    dbmSay(stderr, TEXT`[WARN] [recovery] ${db.key}: the previous run was interrupted at step ${interrupted.step}, ${interrupted.done} steps were executed, recovering by ${mode}\n`);

    if (strcmp(mode, "rollback") == 0) {

      /* a step started but not done did not change the database */
      keepExecuted(&db, interrupted.done, 0, false);

      if (rollBack(&db)) {
        dbmWrite(why, room, TEXT`could not roll back the interrupted run: ${db.error}`);
        yyjson_mut_doc_free(db.record);
        return -1;
      }

      yyjson_mut_doc_free(db.record);
      stored = dbmStateBegin(state, db.key, "up", hash, NULL);

      if (stored == NULL) {
        dbmWrite(why, room, TEXT`could not begin the state of ${name}: ${state->db->error}`);
        return -1;
      }

      db.record = dbmRecordFrom(stored);
      free(stored);
    } else {
      db.recovery = &interrupted;
    }
  }

  int answer = migration->migrate(&db);

  if (answer != 0 && !db.failed)
    fail(&db, TEXT`the migration answered non-zero without saying why`);

  if (db.failed) {

    char reason[512];
    char instruction[700] = "";

    /* "at" the step that failed, "after" the last one when the code failed */
    if (db.counter > 0)
      dbmWrite(instruction, sizeof instruction, TEXT`${db.done >= db.counter ? "after" : "at"} step ${(long)db.counter} ${db.current}`);

    dbmWrite(reason, sizeof reason, TEXT`${db.error}`);
    dbmSayFailure(driver, db.key, instruction[0] != '\0' ? instruction : NULL,
                  reason);
    dbmSay(stderr, TEXT`[INFO] Rolling back ${db.key}\n`);

    /* the steps that ran, and the failed one if its table or column is there */
    keepExecuted(&db, db.done, db.counter, !db.sent && driver->signaled);

    /**
     * One of them cannot be undone: nothing is rolled back, the run stays
     * unfinished in the lock row, and the next one continues after the
     * steps that ran - as node does.
     */
    char irreversible[600];

    irreversibleSteps(&db, irreversible, sizeof irreversible);

    if (irreversible[0] != '\0') {
      save(&db);
      dbmSay(stderr, TEXT`[ERROR] Migration "${db.key}" failed and can not be rolled back, it ran a step with { irreversible: true }. The steps executed stay, the next run continues after them.\n`);
      why[0] = '\0';
      yyjson_mut_doc_free(db.record);
      return -1;
    }

    if (rollBack(&db))
      dbmSay(stderr, TEXT`[ERROR] and undoing it failed too: ${db.error}\n`);

    /* said above, with its statement - `why` stays empty for the walker */
    why[0] = '\0';
    yyjson_mut_doc_free(db.record);
    return -1;
  }

  yyjson_mut_doc_free(db.record);
  return 0;
}

/**
 * `fix`: a v2 migration that ran, learned again from scratch - its record
 * rebuilt and the schema with it, nothing sent, because what it built is
 * there. node appends the new steps to the record it finds; this starts the
 * record over, which is what rebuilding it means.
 */
int dbmFixV2(driver_t *driver, dbm_state_t *state,
             const dbm_migration_t *migration, char *why, size_t room) {

  const char *name = migration->name;
  schema_t db = {.driver = driver, .state = state, .key = dbmKeyOf(name),
                 .dry = driver->dryRun, .fixing = true, .name = dbmKeyOf(name)};

  if (!db.dry) {

    char hex[65];
    char *stored = dbmStateBegin(state, db.key, "fix", dbmHashOf(migration, hex),
                                 NULL);

    if (stored == NULL) {
      dbmWrite(why, room, TEXT`could not begin the state of ${name}: ${state->db->error}`);
      return -1;
    }

    free(stored);
  }

  db.record = dbmRecordFrom(NULL);

  int answer = migration->migrate(&db);

  if (answer != 0 && !db.failed)
    fail(&db, TEXT`the migration answered non-zero without saying why`);

  if (!db.failed && !db.dry && dbmStateProgress(state, -1, 1))
    fail(&db, TEXT`${state->db->error}`);

  if (db.failed)
    dbmWrite(why, room, TEXT`${db.error}`);

  yyjson_mut_doc_free(db.record);
  return db.failed ? -1 : 0;
}

/** The end of a v2 migration that ran: the lock row says so. */
int dbmEndV2(dbm_state_t *state, bool dry) {
  return dry ? 0 : dbmStateProgress(state, -1, 1);
}

/** A v2 migration down: its record run backwards, then forgotten. */
int dbmDownV2(driver_t *driver, dbm_state_t *state,
              const dbm_migration_t *migration, char *why, size_t room) {

  const char *name = migration->name;
  schema_t db = {.driver = driver, .state = state, .key = dbmKeyOf(name),
                 .dry = driver->dryRun, .unlearn = true, .name = dbmKeyOf(name)};
  char *stored = NULL;

  if (dbmKvGet(state->db, state->table, db.key, &stored)) {
    dbmWrite(why, room, TEXT`${state->db->error}`);
    return -1;
  }

  /**
   * node carries on with nothing to undo and deletes the record; that leaves
   * a schema behind which nothing knows how to take away, so this stops.
   */
  if (stored == NULL) {
    dbmWrite(why, room, TEXT`there is no state for ${db.key} in ${state->table} - it cannot be undone from here`);
    return -1;
  }

  /* the lock row says which migration goes down, as node's does */
  if (!db.dry) {

    char hex[65];
    char *begun = dbmStateBegin(state, db.key, "down", dbmHashOf(migration, hex),
                                NULL);

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
    dbmWrite(why, room, TEXT`Migration "${db.key}" can not be reverted, it ran ${irreversible} with { irreversible: true }.`);
    yyjson_mut_doc_free(db.record);
    return -1;
  }

  int answer = undoAll(&db);

  if (answer == 0 && !db.dry)
    answer = dbmStateForget(state, db.key) || dbmStateProgress(state, -1, 1);

  if (answer != 0)
    dbmWrite(why, room, TEXT`${db.failed ? db.error : state->db->error}`);

  yyjson_mut_doc_free(db.record);
  return answer != 0 ? -1 : 0;
}
