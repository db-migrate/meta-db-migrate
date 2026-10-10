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
  yyjson_mut_val *spec;
  yyjson_mut_val *options;
  bool unique;
} step_t;

/* ------------------------------------------------------------------ */
/* JSON, the mutable kind                                             */
/* ------------------------------------------------------------------ */

static yyjson_mut_doc *schemaDoc(schema_t *self) {
  return self->state->schema;
}

static yyjson_mut_val *rootOf(yyjson_mut_doc *doc) {
  return yyjson_mut_doc_get_root(doc);
}

/** The object under `key`, made if it is not there. */
static yyjson_mut_val *child(yyjson_mut_doc *doc, yyjson_mut_val *parent,
                             const char *key) {

  yyjson_mut_val *found = yyjson_mut_obj_get(parent, key);

  if (found != NULL && yyjson_mut_is_obj(found))
    return found;

  found = yyjson_mut_obj(doc);
  yyjson_mut_obj_put(parent, yyjson_mut_strcpy(doc, key), found);
  return found;
}

/** `parent[key] = value`, a copy into `doc`, in place of whatever was there. */
static void put(yyjson_mut_doc *doc, yyjson_mut_val *parent, const char *key,
                yyjson_mut_val *value) {
  yyjson_mut_obj_put(parent, yyjson_mut_strcpy(doc, key),
                     value != NULL ? yyjson_mut_val_mut_copy(doc, value)
                                   : yyjson_mut_null(doc));
}

static void removeKey(yyjson_mut_val *parent, const char *key) {
  if (parent != NULL)
    yyjson_mut_obj_remove_key(parent, key);
}

/** A document someone handed over, as a value of `doc`. */
static yyjson_mut_val *copyIn(yyjson_mut_doc *doc, json_t value) {
  return value.node != NULL ? yyjson_mut_val_mut_copy(doc, value.node)
                            : yyjson_mut_obj(doc);
}

/** A value of the state, as a document a driver can read. The caller releases it. */
static json_t asJson(yyjson_mut_val *value) {

  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);

  yyjson_mut_doc_set_root(doc, value != NULL
                                   ? yyjson_mut_val_mut_copy(doc, value)
                                   : yyjson_mut_obj(doc));
  return meta_jsonFromMut(doc);
}

/** A column spec's `{columns: ...}` form, or the spec itself. */
static yyjson_mut_val *columnsOf(yyjson_mut_val *table) {

  yyjson_mut_val *columns = yyjson_mut_obj_get(table, "columns");

  return columns != NULL && yyjson_mut_is_obj(columns) ? columns : table;
}

static const char *textOf(yyjson_mut_val *value) {
  return value != NULL && yyjson_mut_is_str(value) ? yyjson_mut_get_str(value)
                                                   : "";
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
  yyjson_mut_val *steps = yyjson_mut_obj_get(rootOf(doc), "s");
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
  yyjson_mut_arr_append(steps, entry);
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
  yyjson_mut_arr_append(yyjson_mut_obj_get(rootOf(doc), "s"), entry);
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

static yyjson_mut_val *tableIn(schema_t *self, const char *table) {
  return yyjson_mut_obj_get(yyjson_mut_obj_get(rootOf(schemaDoc(self)), "c"),
                            table);
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

  yyjson_mut_val *spec = tableIn(self, table);

  return spec != NULL && (yyjson_mut_obj_get(spec, column) != NULL ||
                          yyjson_mut_obj_get(columnsOf(spec), column) != NULL);
}

static int needTable(schema_t *self, const char *table) {
  return tableIn(self, table) == NULL ? unknownTable(self, table, false) : 0;
}

static int needColumn(schema_t *self, const char *table, const char *column) {

  if (tableIn(self, table) == NULL)
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
static void learnColumnKeys(schema_t *self, const char *table,
                            yyjson_mut_val *spec) {

  yyjson_mut_doc *doc = schemaDoc(self);
  yyjson_mut_val *key;
  yyjson_mut_obj_iter walk;

  yyjson_mut_obj_iter_init(spec, &walk);

  while ((key = yyjson_mut_obj_iter_next(&walk)) != NULL) {

    yyjson_mut_val *column = yyjson_mut_obj_iter_get_val(key);
    yyjson_mut_val *foreign = yyjson_mut_is_obj(column)
                                  ? yyjson_mut_obj_get(column, "foreignKey")
                                  : NULL;

    if (foreign == NULL || !yyjson_mut_is_obj(foreign))
      continue;

    yyjson_mut_val *keys = child(doc, child(doc, rootOf(doc), "f"), table);
    yyjson_mut_val *entry = yyjson_mut_obj(doc);
    yyjson_mut_val *rules = yyjson_mut_obj_get(foreign, "rules");
    yyjson_mut_val *mapping = yyjson_mut_obj_get(foreign, "mapping");

    yyjson_mut_obj_add_strcpy(doc, entry, "t", table);
    yyjson_mut_obj_add_strcpy(doc, entry, "rt",
                              textOf(yyjson_mut_obj_get(foreign, "table")));

    if (rules != NULL)
      put(doc, entry, "r", rules);

    if (mapping != NULL && yyjson_mut_is_str(mapping)) {
      yyjson_mut_val *m = yyjson_mut_obj(doc);
      put(doc, m, yyjson_mut_get_str(key), mapping);
      yyjson_mut_obj_add_val(doc, entry, "m", m);
    } else {
      put(doc, entry, "m", mapping);
    }

    put(doc, keys, textOf(yyjson_mut_obj_get(foreign, "name")), entry);
  }
}

/** What a step does to the schema, and the step that undoes it. */
static int learn(schema_t *self, dbm_action_t action, step_t *step) {

  yyjson_mut_doc *doc = schemaDoc(self);
  yyjson_mut_doc *mod = self->record;
  yyjson_mut_val *schema = rootOf(doc);
  yyjson_mut_val *kept = rootOf(mod);
  yyjson_mut_val *c = child(doc, schema, "c");
  yyjson_mut_val *f = child(doc, schema, "f");
  yyjson_mut_val *i = child(doc, schema, "i");
  const char *t = step->table;

  match (action) {

  case ACTION_CREATE_TABLE: {
    put(doc, c, t, step->spec);
    learnColumnKeys(self, t, step->spec);

    const char *args[] = {t};
    record(self, 0, ACTION_DROP_TABLE, args, 1, NULL);
    return 0;
  }

  case ACTION_DROP_TABLE: {

    /* one the schema does not know: refused, or dropped beyond undoing */
    if (tableIn(self, t) == NULL && !self->unlearn) {
      if (!self->irreversible)
        return unknownTable(self, t, true);

      const char *args[] = {t};
      record(self, 3, ACTION_DROP_TABLE, args, 1, NULL);
      return 0;
    }

    const char *const parts[] = {"c", "f", "i"};

    for (size_t at = 0; at < countof(parts); ++at) {

      yyjson_mut_val *from = yyjson_mut_obj_get(schema, parts[at]);
      yyjson_mut_val *held = yyjson_mut_obj_get(from, t);

      if (held != NULL) {
        put(mod, child(mod, kept, parts[at]), t, held);
        removeKey(from, t);
      }
    }

    const char *args[] = {t};
    record(self, 1, ACTION_CREATE_TABLE, args, 1, NULL);
    return 0;
  }

  case ACTION_RENAME_TABLE: {

    const char *n = step->name;
    yyjson_mut_val *parts[] = {c, f, i};
    yyjson_mut_val *d = child(doc, schema, "d");
    const char *const kinds[] = {"tables", "columns"};

    /* deprecations move with the table - unless it is renamed for its own
       deprecation, which stays under its name */
    for (size_t at = 0; at < countof(kinds); ++at) {

      yyjson_mut_val *marks = yyjson_mut_obj_get(d, kinds[at]);
      yyjson_mut_val *held = yyjson_mut_obj_get(marks, t);
      bool own = at == 0 && held != NULL &&
                 strcmp(textOf(yyjson_mut_obj_get(held, "to")), n) == 0;

      if (held != NULL && !own) {
        put(doc, marks, n, held);
        removeKey(marks, t);
      }
    }

    for (size_t at = 0; at < countof(parts); ++at) {

      yyjson_mut_val *held = yyjson_mut_obj_get(parts[at], t);

      if (held != NULL) {
        put(doc, parts[at], n, held);
        removeKey(parts[at], t);
      }
    }

    const char *args[] = {n, t};
    record(self, 0, ACTION_RENAME_TABLE, args, 2, NULL);
    return 0;
  }

  case ACTION_ADD_COLUMN: {

    if (needTable(self, t))
      return -1;

    put(doc, columnsOf(tableIn(self, t)), step->name, step->spec);

    const char *args[] = {t, step->name};
    record(self, 0, ACTION_REMOVE_COLUMN, args, 2, NULL);
    return 0;
  }

  case ACTION_REMOVE_COLUMN: {

    if (!hasColumn(self, t, step->name) && !self->unlearn) {
      if (!self->irreversible)
        return tableIn(self, t) == NULL
                   ? unknownTable(self, t, true)
                   : unknownColumn(self, t, step->name, true);

      const char *args[] = {t, step->name};
      record(self, 3, ACTION_REMOVE_COLUMN, args, 2, NULL);
      return 0;
    }

    if (needColumn(self, t, step->name))
      return -1;

    yyjson_mut_val *columns = columnsOf(tableIn(self, t));
    yyjson_mut_val *column = yyjson_mut_obj_get(columns, step->name);
    bool notNull = yyjson_mut_is_obj(column) &&
                   yyjson_mut_is_true(yyjson_mut_obj_get(column, "notNull"));
    const char *strategy =
        textOf(yyjson_mut_obj_get(step->options, "columnStrategy"));

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
    yyjson_mut_val *was = child(mod, child(mod, kept, "c"), t);

    if (notNull && !self->unlearn && strcmp(strategy, "delay") == 0) {

      put(mod, was, step->name, column);

      const char *args[] = {t, step->other, step->name};
      record(self, 0, ACTION_RENAME_COLUMN, args, 3, NULL);
    } else if (!self->unlearn) {

      put(mod, was, step->name, column);

      if (notNull) {
        yyjson_mut_val *passthrough =
            yyjson_mut_obj_get(step->options, "passthrough");
        put(mod, yyjson_mut_obj_get(was, step->name), "defaultValue",
            yyjson_mut_obj_get(passthrough, "defaultValue"));
      }

      const char *args[] = {t, step->name};
      record(self, 1, ACTION_ADD_COLUMN, args, 2, step->options);
    }

    removeKey(columns, step->name);
    return 0;
  }

  case ACTION_RENAME_COLUMN: {

    if (needColumn(self, t, step->name))
      return -1;

    yyjson_mut_val *columns = columnsOf(tableIn(self, t));

    put(doc, columns, step->other, yyjson_mut_obj_get(columns, step->name));
    removeKey(columns, step->name);

    const char *args[] = {t, step->other, step->name};
    record(self, 0, ACTION_RENAME_COLUMN, args, 3, NULL);
    return 0;
  }

  case ACTION_CHANGE_COLUMN: {

    if (needColumn(self, t, step->name))
      return -1;

    yyjson_mut_val *columns = columnsOf(tableIn(self, t));
    yyjson_mut_val *column = yyjson_mut_obj_get(columns, step->name);

    /* undone: exactly the column as it was kept, not merged into the new */
    if (self->unlearn) {
      yyjson_mut_obj_put(columns, yyjson_mut_strcpy(doc, step->name),
                         yyjson_mut_val_mut_copy(doc, step->spec));
      return 0;
    }

    /* the column as it was, which node meant to keep and did not */
    put(mod, child(mod, child(mod, kept, "c"), t), step->name, column);

    if (!yyjson_mut_is_obj(column)) {
      yyjson_mut_val *spelled = yyjson_mut_obj(doc);
      put(doc, spelled, "type", column);
      yyjson_mut_obj_put(columns, yyjson_mut_strcpy(doc, step->name), spelled);
      column = spelled;
    }

    yyjson_mut_val *key;
    yyjson_mut_obj_iter walk;

    yyjson_mut_obj_iter_init(step->spec, &walk);

    while ((key = yyjson_mut_obj_iter_next(&walk)) != NULL)
      put(doc, column, yyjson_mut_get_str(key),
          yyjson_mut_obj_iter_get_val(key));

    const char *args[] = {t, step->name};
    record(self, 1, ACTION_CHANGE_COLUMN, args, 2, NULL);
    return 0;
  }

  case ACTION_ADD_INDEX: {

    for (size_t at = 0; at < yyjson_mut_arr_size(step->spec); ++at) {

      char name[256];

      unquoted(textOf(yyjson_mut_arr_get(step->spec, at)), name, sizeof name);

      if (needColumn(self, t, name))
        return -1;
    }

    yyjson_mut_val *index = yyjson_mut_obj(doc);

    yyjson_mut_obj_add_strcpy(doc, index, "t", t);
    put(doc, index, "c", step->spec);

    if (step->unique)
      yyjson_mut_obj_add_bool(doc, index, "u", true);

    yyjson_mut_obj_put(child(doc, i, t), yyjson_mut_strcpy(doc, step->name),
                       index);

    const char *args[] = {t, step->name};
    record(self, 0, ACTION_REMOVE_INDEX, args, 2, NULL);
    return 0;
  }

  case ACTION_REMOVE_INDEX: {

    if (needTable(self, t))
      return -1;

    yyjson_mut_val *index = yyjson_mut_obj_get(yyjson_mut_obj_get(i, t),
                                               step->name);

    if (index == NULL)
      return unknownIndex(self, t, step->name);

    yyjson_mut_val *was = child(mod, child(mod, kept, "i"), t);

    put(mod, was, step->name, index);
    removeKey(yyjson_mut_obj_get(i, t), step->name);

    const char *args[] = {t, step->name};
    record(self, 1, ACTION_ADD_INDEX, args, 2, NULL);
    return 0;
  }

  case ACTION_ADD_FOREIGN_KEY: {

    if (needTable(self, t) || needTable(self, step->other))
      return -1;

    yyjson_mut_val *entry = yyjson_mut_obj(doc);

    yyjson_mut_obj_add_strcpy(doc, entry, "t", t);
    yyjson_mut_obj_add_strcpy(doc, entry, "rt", step->other);
    put(doc, entry, "m", step->spec);

    if (step->options != NULL && yyjson_mut_obj_size(step->options) > 0)
      put(doc, entry, "r", step->options);

    yyjson_mut_obj_put(child(doc, f, t), yyjson_mut_strcpy(doc, step->name),
                       entry);

    const char *args[] = {t, step->name};
    record(self, 0, ACTION_REMOVE_FOREIGN_KEY, args, 2, NULL);
    return 0;
  }

  case ACTION_REMOVE_FOREIGN_KEY: {

    yyjson_mut_val *key = yyjson_mut_obj_get(yyjson_mut_obj_get(f, t),
                                             step->name);

    if (key == NULL && !self->unlearn) {
      if (!self->irreversible)
        return tableIn(self, t) == NULL
                   ? unknownTable(self, t, true)
                   : unknownForeignKey(self, t, step->name, true);

      const char *args[] = {t, step->name};
      record(self, 3, ACTION_REMOVE_FOREIGN_KEY, args, 2, NULL);
      return 0;
    }

    if (needTable(self, t))
      return -1;

    if (key == NULL)
      return unknownForeignKey(self, t, step->name, false);

    yyjson_mut_val *was = child(mod, child(mod, kept, "f"), t);

    put(mod, was, step->name, key);
    removeKey(yyjson_mut_obj_get(f, t), step->name);

    /* node records nothing here, and so could not put the key back */
    const char *args[] = {t, step->name};
    record(self, 1, ACTION_ADD_FOREIGN_KEY, args, 2, NULL);
    return 0;
  }

  case ACTION_SET_DEPRECATED: {

    /* d.tables[t] or d.columns[t][c] set to the entry, or cleared */
    const char *kind = step->other;
    const char *column = step->name;
    yyjson_mut_val *marks = child(doc, child(doc, schema, "d"), kind);
    yyjson_mut_val *holder = column != NULL ? child(doc, marks, t) : marks;
    const char *key = column != NULL ? column : t;
    yyjson_mut_val *previous = yyjson_mut_obj_get(holder, key);
    yyjson_mut_val *args = yyjson_mut_arr(mod);

    yyjson_mut_arr_add_strcpy(mod, args, kind);
    yyjson_mut_arr_add_strcpy(mod, args, t);

    if (column != NULL)
      yyjson_mut_arr_add_strcpy(mod, args, column);
    else
      yyjson_mut_arr_add_null(mod, args);

    yyjson_mut_arr_append(args, previous != NULL
                                    ? yyjson_mut_val_mut_copy(mod, previous)
                                    : yyjson_mut_null(mod));

    if (step->spec != NULL && !yyjson_mut_is_null(step->spec)) {
      put(doc, holder, key, step->spec);
    } else {
      removeKey(holder, key);
      if (column != NULL && yyjson_mut_obj_size(holder) == 0)
        removeKey(marks, t);
    }

    recordWith(self, 2, ACTION_SET_DEPRECATED, args);
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

  case ACTION_CREATE_TABLE: {
    json_t spec = asJson(step->spec);
    answer = driver->createTable(driver, t, spec);
    spec.release();
    break;
  }

  case ACTION_DROP_TABLE:
    answer = driver->dropTable(driver, t, false);
    break;

  case ACTION_RENAME_TABLE:
    answer = driver->renameTable(driver, t, step->name);
    break;

  case ACTION_ADD_COLUMN: {
    json_t spec = asJson(step->spec);
    answer = driver->addColumn(driver, t, step->name, spec);
    spec.release();
    break;
  }

  case ACTION_REMOVE_COLUMN:

    /* `delay` keeps the column under another name instead of dropping it */
    answer = step->other != NULL
                 ? driver->renameColumn(driver, t, step->name, step->other)
                 : driver->removeColumn(driver, t, step->name);
    break;

  case ACTION_RENAME_COLUMN:
    answer = driver->renameColumn(driver, t, step->name, step->other);
    break;

  case ACTION_CHANGE_COLUMN: {
    json_t spec = asJson(step->spec);
    answer = driver->changeColumn(driver, t, step->name, spec);
    spec.release();
    break;
  }

  case ACTION_ADD_INDEX: {

    /* written without the quotes node's pg driver left in its state */
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *names = yyjson_mut_arr(doc);

    for (size_t at = 0; at < yyjson_mut_arr_size(step->spec); ++at) {

      char name[256];

      unquoted(textOf(yyjson_mut_arr_get(step->spec, at)), name, sizeof name);
      yyjson_mut_arr_add_strcpy(doc, names, name);
    }

    yyjson_mut_doc_set_root(doc, names);

    json_t columns = meta_jsonFromMut(doc);
    answer = driver->addIndex(driver, t, step->name, columns, step->unique);
    columns.release();
    break;
  }

  case ACTION_REMOVE_INDEX:
    answer = driver->removeIndex(driver, t, step->name);
    break;

  case ACTION_ADD_FOREIGN_KEY: {
    json_t mapping = asJson(step->spec);
    json_t rules = asJson(step->options);
    answer = driver->addForeignKey(driver, t, step->other, step->name, mapping,
                                   rules);
    mapping.release();
    rules.release();
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

  step_t step = {.table = table};

  step.spec = copyIn(schemaDoc(self), spec);

  /* node's convention: every table a v2 migration makes carries this column -
     one adopted was not made by one, and has what it has */
  if (!self->adopting &&
      yyjson_mut_obj_get(step.spec, "__dbmigrate__flag") == NULL) {
    yyjson_mut_val *flag = yyjson_mut_obj(schemaDoc(self));
    yyjson_mut_obj_add_str(schemaDoc(self), flag, "type", "string");
    yyjson_mut_obj_add_val(schemaDoc(self), step.spec, "__dbmigrate__flag",
                           flag);
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
  step_t step = {.table = table, .name = column};
  step.spec = copyIn(schemaDoc(self), spec);
  return perform(self, ACTION_ADD_COLUMN, &step);
}

int schema_t__removeColumnWith(schema_t *self, const char *table,
                               const char *column, json_t options) {

  char renamed[300];
  step_t step = {.table = table, .name = column};

  step.options = copyIn(self->record, options);

  /* `delay` renames it out of the way, under node's name for that */
  if (strcmp(textOf(yyjson_mut_obj_get(step.options, "columnStrategy")),
             "delay") == 0) {

    const char *given = textOf(yyjson_mut_obj_get(
        yyjson_mut_obj_get(step.options, "passthrough"), "column"));

    if (given[0] != '\0') {
      dbmWrite(renamed, sizeof renamed, TEXT`${given}`);
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
  step_t step = {.table = table, .name = column};
  step.spec = copyIn(schemaDoc(self), spec);
  return perform(self, ACTION_CHANGE_COLUMN, &step);
}

static int addIndex(schema_t *self, const char *table, const char *name,
                    json_t columns, bool unique) {

  step_t step = {.table = table, .name = name, .unique = unique};

  /* `"name"` and `["name"]` are the same index, kept as the second */
  if (strcmp(columns.kind(), "string") == 0) {
    step.spec = yyjson_mut_arr(schemaDoc(self));
    yyjson_mut_arr_add_strcpy(schemaDoc(self), step.spec, columns.text());
  } else {
    step.spec = copyIn(schemaDoc(self), columns);
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

  step.spec = copyIn(schemaDoc(self), mapping);
  step.options = copyIn(schemaDoc(self), rules);
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

  step_t step = {.table = table, .name = column, .other = kind};

  step.spec = entry;
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

  yyjson_mut_doc *doc = schemaDoc(self);
  yyjson_mut_val *entry = yyjson_mut_obj(doc);
  yyjson_mut_val *given = yyjson_mut_obj(doc);
  const char *release = self->state->releaseLabel;
  char to[300];
  char why[300];

  if (dbmReleaseOptions(options, doc, given, why, sizeof why)) {
    fail(self, TEXT`${why}`);
    return NULL;
  }

  dbmHiddenName(name, self->name, to, sizeof to);

  if (release != NULL)
    yyjson_mut_obj_add_strcpy(doc, entry, "r", release);
  else
    yyjson_mut_obj_add_null(doc, entry, "r");

  yyjson_mut_obj_add_strcpy(doc, entry, "to", to);

  if (yyjson_mut_obj_size(given) > 0)
    yyjson_mut_obj_add_val(doc, entry, "o", given);

  return entry;
}

int schema_t__deprecateTableWith(schema_t *self, const char *table,
                                 json_t options) {

  if (self->failed)
    return -1;

  if (tableIn(self, table) == NULL)
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

  if (tableIn(self, table) == NULL)
    return unknownTable(self, table, false);

  yyjson_mut_val *spec = yyjson_mut_obj_get(columnsOf(tableIn(self, table)),
                                            column);

  if (spec == NULL)
    return unknownColumn(self, table, column, false);

  yyjson_mut_val *entry = deprecation(self, column, options);

  if (entry == NULL)
    return -1;

  /* the application stops writing it: NOT NULL relaxed, by the whole spec,
     as MySQL changes a column */
  if (yyjson_mut_is_obj(spec) &&
      yyjson_mut_is_true(yyjson_mut_obj_get(spec, "notNull"))) {

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *relaxed = yyjson_mut_val_mut_copy(doc, spec);

    yyjson_mut_obj_put(relaxed, yyjson_mut_str(doc, "notNull"),
                       yyjson_mut_false(doc));
    yyjson_mut_doc_set_root(doc, relaxed);

    json_t changed = meta_jsonFromMut(doc);
    int answer = self->changeColumn(table, column, changed);

    changed.release();

    if (answer != 0)
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

  if (tableIn(self->db, table) != NULL)
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

  yyjson_mut_val *indexes = yyjson_mut_obj_get(
      yyjson_mut_obj_get(rootOf(schemaDoc(self->db)), "i"), table);

  if (self->db->failed)
    return -1;

  if (yyjson_mut_obj_get(indexes, name) != NULL)
    return known(self->db, TEXT`addIndex("${table}", "${name}")`);

  return DBM_ADOPTING(self->db,
                      schema_t__addIndex(self->db, table, name, columns));
}

int schema_adopt_t__addUniqueIndex(schema_adopt_t *self, const char *table,
                                   const char *name, json_t columns) {

  yyjson_mut_val *indexes = yyjson_mut_obj_get(
      yyjson_mut_obj_get(rootOf(schemaDoc(self->db)), "i"), table);

  if (self->db->failed)
    return -1;

  if (yyjson_mut_obj_get(indexes, name) != NULL)
    return known(self->db, TEXT`addIndex("${table}", "${name}")`);

  return DBM_ADOPTING(self->db,
                      schema_t__addUniqueIndex(self->db, table, name, columns));
}

int schema_adopt_t__addForeignKey(schema_adopt_t *self, const char *table,
                                  const char *referenced, const char *name,
                                  json_t mapping, json_t rules) {

  yyjson_mut_val *keys = yyjson_mut_obj_get(
      yyjson_mut_obj_get(rootOf(schemaDoc(self->db)), "f"), table);

  if (self->db->failed)
    return -1;

  if (yyjson_mut_obj_get(keys, name) != NULL)
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

static const char *argument(yyjson_mut_val *args, size_t at) {
  return textOf(yyjson_mut_arr_get(args, at));
}

/**
 * A table back as it was: from the columns kept in the record, then its
 * indexes and the keys its columns did not declare themselves.
 */
static int rebuildTable(schema_t *self, const char *t) {

  yyjson_mut_val *kept = rootOf(self->record);
  yyjson_mut_val *spec = yyjson_mut_obj_get(child(self->record, kept, "c"), t);

  if (spec == NULL)
    return fail(self, TEXT`the state does not say what ${t} looked like`);

  step_t create = {.table = t, .spec = spec};

  if (undo(self, ACTION_CREATE_TABLE, &create))
    return -1;

  /* the keys the columns declare were made with the table */
  yyjson_mut_val *keys = yyjson_mut_obj_get(child(self->record, kept, "f"), t);
  yyjson_mut_val *column;
  yyjson_mut_obj_iter walk;

  yyjson_mut_obj_iter_init(columnsOf(spec), &walk);

  while ((column = yyjson_mut_obj_iter_next(&walk)) != NULL) {

    yyjson_mut_val *declared = yyjson_mut_obj_get(
        yyjson_mut_obj_iter_get_val(column), "foreignKey");

    if (declared != NULL)
      removeKey(keys, textOf(yyjson_mut_obj_get(declared, "name")));
  }

  yyjson_mut_val *indexes = yyjson_mut_obj_get(child(self->record, kept, "i"), t);
  yyjson_mut_val *name;

  yyjson_mut_obj_iter_init(indexes, &walk);

  while ((name = yyjson_mut_obj_iter_next(&walk)) != NULL) {

    yyjson_mut_val *index = yyjson_mut_obj_iter_get_val(name);
    step_t step = {.table = t, .name = yyjson_mut_get_str(name)};

    step.spec = yyjson_mut_obj_get(index, "c");
    step.unique = yyjson_mut_is_true(yyjson_mut_obj_get(index, "u"));

    if (undo(self, ACTION_ADD_INDEX, &step))
      return -1;
  }

  yyjson_mut_obj_iter_init(keys, &walk);

  while ((name = yyjson_mut_obj_iter_next(&walk)) != NULL) {

    yyjson_mut_val *key = yyjson_mut_obj_iter_get_val(name);
    step_t step = {.table = t, .name = yyjson_mut_get_str(name)};

    step.other = textOf(yyjson_mut_obj_get(key, "rt"));
    step.spec = yyjson_mut_obj_get(key, "m");
    step.options = yyjson_mut_obj_get(key, "r");

    if (undo(self, ACTION_ADD_FOREIGN_KEY, &step))
      return -1;
  }

  return 0;
}

/** One recorded undo step, `{t, a, c}`. */
static int undoEntry(schema_t *self, yyjson_mut_val *entry) {

  yyjson_mut_val *kept = rootOf(self->record);
  long type = yyjson_mut_get_int(yyjson_mut_obj_get(entry, "t"));
  const char *named = textOf(yyjson_mut_obj_get(entry, "a"));
  yyjson_mut_val *args = yyjson_mut_obj_get(entry, "c");
  int found = (int)fromspelling(enum dbmAction, named);

  if (found < 0 || !(type in {0, 1, 2}))
    return fail(self, TEXT`Invalid state record, of type ${type}: ${named}`);

  dbm_action_t action = (dbm_action_t)found;
  step_t step = {.table = argument(args, 0)};

  /* a mark, put back as it was before */
  if (action == ACTION_SET_DEPRECATED) {

    yyjson_mut_val *column = yyjson_mut_arr_get(args, 2);

    step.other = argument(args, 0);
    step.table = argument(args, 1);
    step.name = column != NULL && yyjson_mut_is_str(column)
                    ? yyjson_mut_get_str(column)
                    : NULL;
    step.spec = yyjson_mut_arr_get(args, 3);
    return learn(self, action, &step);
  }

  /* 0 is called as it was recorded; 2, adopted, is only forgotten again */
  if (type == 0 || type == 2) {

    /* called as it was recorded */
    match (action) {
    case ACTION_RENAME_TABLE:
      step.name = argument(args, 1);
      break;
    case ACTION_RENAME_COLUMN:
      step.name = argument(args, 1);
      step.other = argument(args, 2);
      break;
    case ACTION_DROP_TABLE:
      break;
    case ACTION_REMOVE_COLUMN, ACTION_REMOVE_INDEX,
        ACTION_REMOVE_FOREIGN_KEY:
      step.name = argument(args, 1);
      break;
    default:
      return fail(self, TEXT`a ${named} step cannot be undone as it stands`);
    }

    return type == 2 ? learn(self, action, &step) : undo(self, action, &step);
  }

  /* rebuilt from what the record kept */
  step.name = argument(args, 1);

  match (action) {

  case ACTION_CREATE_TABLE:
    return rebuildTable(self, step.table);

  case ACTION_ADD_COLUMN, ACTION_CHANGE_COLUMN:
    step.spec = yyjson_mut_obj_get(
        yyjson_mut_obj_get(child(self->record, kept, "c"), step.table),
        step.name);

    if (step.spec == NULL)
      return fail(self, TEXT`the state does not say what ${step.table}.${step.name} looked like`);

    return undo(self, action, &step);

  case ACTION_ADD_INDEX: {

    yyjson_mut_val *index = yyjson_mut_obj_get(
        yyjson_mut_obj_get(child(self->record, kept, "i"), step.table),
        step.name);

    if (index == NULL) {
      dbmSay(stderr, TEXT`[WARN] There was an attempt to create the index "${step.name}" in the table "${step.table}". This index did not exist at the time of deleting it - recreate it by hand if it was made by hand.\n`);
      return 0;
    }

    step.spec = yyjson_mut_obj_get(index, "c");
    step.unique = yyjson_mut_is_true(yyjson_mut_obj_get(index, "u"));
    return undo(self, action, &step);
  }

  case ACTION_ADD_FOREIGN_KEY: {

    yyjson_mut_val *key = yyjson_mut_obj_get(
        yyjson_mut_obj_get(child(self->record, kept, "f"), step.table),
        step.name);

    if (key == NULL)
      return fail(self, TEXT`the state does not say what foreign key ${step.name} was`);

    step.other = textOf(yyjson_mut_obj_get(key, "rt"));
    step.spec = yyjson_mut_obj_get(key, "m");
    step.options = yyjson_mut_obj_get(key, "r");
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

  yyjson_mut_val *steps = yyjson_mut_obj_get(rootOf(self->record), "s");
  bool unlearn = self->unlearn;

  self->unlearn = true;

  while (yyjson_mut_arr_size(steps) > 0) {

    yyjson_mut_val *entry = yyjson_mut_arr_get_last(steps);

    if (undoEntry(self, entry))
      break;

    yyjson_mut_arr_remove_last(steps);

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
static long stepOf(yyjson_mut_val *entry) {

  yyjson_mut_val *n = yyjson_mut_obj_get(entry, "n");

  return n != NULL && yyjson_mut_is_int(n) ? (long)yyjson_mut_get_sint(n) : 0;
}

/**
 * The undo entries of the steps that changed the database, the rest let go:
 * every step up to `done`, and `failed` as well if its main statement went
 * through before it failed (`signaled`). node's executedSteps.
 */
static void keepExecuted(schema_t *self, long done, long failed,
                         bool signaled) {

  yyjson_mut_val *steps = yyjson_mut_obj_get(rootOf(self->record), "s");
  size_t at = 0;

  while (at < yyjson_mut_arr_size(steps)) {

    long n = stepOf(yyjson_mut_arr_get(steps, at));

    if (n <= done || (signaled && failed > done && n == failed))
      ++at;
    else
      yyjson_mut_arr_remove(steps, at);
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

  yyjson_mut_val *steps = yyjson_mut_obj_get(rootOf(self->record), "s");
  dbm_text_t said = {0};
  defer said.release();

  into[0] = '\0';

  for (size_t at = 0; at < yyjson_mut_arr_size(steps); ++at) {

    yyjson_mut_val *entry = yyjson_mut_arr_get(steps, at);
    yyjson_mut_val *args = yyjson_mut_obj_get(entry, "c");

    if (yyjson_mut_get_int(yyjson_mut_obj_get(entry, "t")) != 3)
      continue;

    said.append(TEXT`${said.length > 0 ? ", " : ""}step ${stepOf(entry)} ${textOf(yyjson_mut_obj_get(entry, "a"))}(`);

    for (size_t i = 0; i < yyjson_mut_arr_size(args); ++i) {
      yyjson_mut_val *arg = yyjson_mut_arr_get(args, i);
      if (yyjson_mut_is_str(arg))
        said.append(TEXT`${i > 0 ? ", " : ""}"${yyjson_mut_get_str(arg)}"`);
    }

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
