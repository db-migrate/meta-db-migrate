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
 * transaction around it - its rollback is this. The step that failed is left
 * out of that, since it never happened.
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

  int counter;
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
  return value.node != NULL ? yyjson_val_mut_copy(doc, value.node)
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
  yyjson_mut_arr_append(steps, entry);
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

static int needTable(schema_t *self, const char *table) {

  if (tableIn(self, table) == NULL)
    return fail(self, TEXT`There is no ${table} table in schema!`);

  return 0;
}

static int needColumn(schema_t *self, const char *table, const char *column) {

  yyjson_mut_val *spec = tableIn(self, table);

  if (spec == NULL)
    return fail(self, TEXT`There is no ${table} table in schema!`);

  if (yyjson_mut_obj_get(spec, column) == NULL &&
      yyjson_mut_obj_get(columnsOf(spec), column) == NULL)
    return fail(self, TEXT`There is no ${column} column in schema!`);

  return 0;
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
      return fail(self, TEXT`There is no index ${step->name} in ${t} table!`);

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

    if (needTable(self, t))
      return -1;

    yyjson_mut_val *key = yyjson_mut_obj_get(yyjson_mut_obj_get(f, t),
                                             step->name);

    if (key == NULL)
      return fail(self, TEXT`There is no foreign key ${step->name} in ${t} table!`);

    yyjson_mut_val *was = child(mod, child(mod, kept, "f"), t);

    put(mod, was, step->name, key);
    removeKey(yyjson_mut_obj_get(f, t), step->name);

    /* node records nothing here, and so could not put the key back */
    const char *args[] = {t, step->name};
    record(self, 1, ACTION_ADD_FOREIGN_KEY, args, 2, NULL);
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
static int perform(schema_t *self, dbm_action_t action, step_t *step) {

  if (self->failed)
    return -1;

  /* node's fix chain: learned first, then the step counted and written */
  if (self->fixing)
    return learn(self, action, step) || travel(self) ? -1 : 0;

  if (travel(self) || learn(self, action, step) || save(self))
    return -1;

  self->sent = false;

  if (send(self, action, step))
    return -1;

  self->sent = true;
  return 0;
}

/* ------------------------------------------------------------------ */
/* what a v2 migration calls                                          */
/* ------------------------------------------------------------------ */

int schema_t__createTable(schema_t *self, const char *table, json_t spec) {

  step_t step = {.table = table};

  step.spec = copyIn(schemaDoc(self), spec);

  /* node's convention: every table a v2 migration makes carries this column */
  if (yyjson_mut_obj_get(step.spec, "__dbmigrate__flag") == NULL) {
    yyjson_mut_val *flag = yyjson_mut_obj(schemaDoc(self));
    yyjson_mut_obj_add_str(schemaDoc(self), flag, "type", "string");
    yyjson_mut_obj_add_val(schemaDoc(self), step.spec, "__dbmigrate__flag",
                           flag);
  }

  return perform(self, ACTION_CREATE_TABLE, &step);
}

int schema_t__dropTable(schema_t *self, const char *table) {
  step_t step = {.table = table};
  return perform(self, ACTION_DROP_TABLE, &step);
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

  return perform(self, ACTION_REMOVE_COLUMN, &step);
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
  step_t step = {.table = table, .name = name};
  return perform(self, ACTION_REMOVE_FOREIGN_KEY, &step);
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

  if (found < 0 || !(type in {0, 1}))
    return fail(self, TEXT`Invalid state record, of type ${type}: ${named}`);

  dbm_action_t action = (dbm_action_t)found;
  step_t step = {.table = argument(args, 0)};

  if (type == 0) {

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

    return undo(self, action, &step);
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
 * stopped. `skipLast` leaves out the newest one: a step that was recorded and
 * then failed to send, which there is nothing to undo of.
 */
static int undoAll(schema_t *self, bool skipLast) {

  yyjson_mut_val *steps = yyjson_mut_obj_get(rootOf(self->record), "s");
  bool unlearn = self->unlearn;

  self->unlearn = true;

  if (skipLast && yyjson_mut_arr_size(steps) > 0) {
    yyjson_mut_arr_remove_last(steps);

    if (save(self))
      return -1;
  }

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
static const char *keyOf(const char *name) {

  const char *slash = strrchr(name, '/');

  return slash != NULL ? slash + 1 : name;
}

/** The record as stored, or a new one - `{}` is what node stores at first. */
static yyjson_mut_doc *recordFrom(const char *text) {

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

/**
 * A v2 migration up: its record begun, its steps run and learned, and on a
 * failure everything it did so far undone from that record - there is no
 * transaction to roll back. Answers 0 when it ran; the reason for anything
 * else is in `why`.
 */
int dbmUpV2(driver_t *driver, dbm_state_t *state, const char *name,
            dbm_v2_t migrate, char *why, size_t room) {

  schema_t db = {.driver = driver, .state = state, .key = keyOf(name),
                 .dry = driver->dryRun};
  char *stored = db.dry ? NULL : dbmStateBegin(state, db.key);

  if (!db.dry && stored == NULL) {
    dbmWrite(why, room, TEXT`could not begin the state of ${name}: ${state->db->error}`);
    return -1;
  }

  db.record = recordFrom(stored);
  free(stored);

  int answer = migrate(&db);

  if (answer != 0 && !db.failed)
    fail(&db, TEXT`the migration answered non-zero without saying why`);

  if (db.failed) {

    char reason[512];

    dbmWrite(reason, sizeof reason, TEXT`${db.error}`);
    dbmSay(stderr, TEXT`[ERROR] An error occured. Rolling back ${keyOf(name)}: ${reason}\n`);

    db.failed = false;

    if (undoAll(&db, !db.sent))
      dbmSay(stderr, TEXT`[ERROR] and undoing it failed too: ${db.error}\n`);

    if (!db.dry) {
      dbmStateForget(state, db.key);
      dbmStateProgress(state, -1, 1);
    }

    dbmWrite(why, room, TEXT`${reason}`);
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
int dbmFixV2(driver_t *driver, dbm_state_t *state, const char *name,
             dbm_v2_t migrate, char *why, size_t room) {

  schema_t db = {.driver = driver, .state = state, .key = keyOf(name),
                 .dry = driver->dryRun, .fixing = true};

  if (!db.dry) {

    char *stored = dbmStateBegin(state, db.key);

    if (stored == NULL) {
      dbmWrite(why, room, TEXT`could not begin the state of ${name}: ${state->db->error}`);
      return -1;
    }

    free(stored);
  }

  db.record = recordFrom(NULL);

  int answer = migrate(&db);

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
int dbmDownV2(driver_t *driver, dbm_state_t *state, const char *name,
              char *why, size_t room) {

  schema_t db = {.driver = driver, .state = state, .key = keyOf(name),
                 .dry = driver->dryRun, .unlearn = true};
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

  if (!db.dry && dbmStateProgress(state, 0, 0)) {
    free(stored);
    dbmWrite(why, room, TEXT`${state->db->error}`);
    return -1;
  }

  db.record = recordFrom(stored);
  free(stored);

  int answer = undoAll(&db, false);

  if (answer == 0 && !db.dry)
    answer = dbmStateForget(state, db.key) || dbmStateProgress(state, -1, 1);

  if (answer != 0)
    dbmWrite(why, room, TEXT`${db.failed ? db.error : state->db->error}`);

  yyjson_mut_doc_free(db.record);
  return answer != 0 ? -1 : 0;
}
