/**
 * The command line, read the way node db-migrate reads it.
 *
 *   app migrate up                     everything not run yet
 *   app migrate up -c 2                the next two
 *   app migrate up 20261008120100      up to and including that one
 *   app migrate down                   the last one
 *   app migrate down 20261008120100    everything after that one
 *   app migrate sync 20261008120100    up or down, whichever gets there
 *   app migrate reset                  all of them, backwards
 *   app migrate check                  what would run
 *   app migrate create add-pets        a new file in migrations/
 *   app migrate up:billing             the scope in migrations/billing/
 *   app migrate create:billing add-tax a new one in that scope
 *
 *   -e, --env NAME       which entry of database.json (NODE_ENV, then dev)
 *   --config FILE        database.json somewhere else, or database.yml
 *   --dry-run            print the statements instead of sending them
 *   --sql-file           `create` writes an up and a down .sql file instead
 *   --sql                `create` writes one .sql file, `-- up` and `-- down`
 *   --migrations-dir D   where `create` writes (migrations)
 *
 * Without a database.json, a database.yml (or whatever a plugin reads) is
 * the configuration, and without that DATABASE_URL, as it is for node
 * db-migrate on every platform that sets one. A connection with a `tunnel`
 * is reached through it.
 */
#include <db_migrate_driver.h>
#include <db_migrate_plugin.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

typedef struct {
  const char *command;
  const char *env;
  const char *config;
  const char *dir;
  const char *name;

  /** `seed down pets`: the seed after down or reset. */
  const char *second;
  size_t count;
  bool countGiven;
  bool dryRun;
  bool sqlFile;
  bool sql;
  bool v2File;
  bool ignoreOnInit;
  const char *logLevel;
  const char *template;
  bool verbose;
  bool noTransactions;
  bool checkOnly;
  bool help;
  bool backupState;
  bool version;
  const char *table;

  /** `up:billing` - the directory under migrations/ the command works in. */
  const char *scope;

  /** node's state table, and how long a lock may go untouched before it is taken. */
  const char *stateTable;
  long lockTimeout;
  long lockInterval;
} options_t;

static int help(void) {

  dbmSay(stdout, TEXT`usage: migrate <command> [name] [options]

commands:
  up [name]          run what has not run, up to and including name
  down [name]        undo the last one, or everything after name
  sync name          up or down, whichever reaches name
  reset              undo everything
  check              list what would run
  fix                rebuild node's state from the v2 migrations that ran
                     (--backup-state keeps the old one)
  create name        a new migration in migrations/
                     (--sql, --sql-file, --v2-file, --template NAME)

  seed [name]        the seeds in seeds/ again: what they inserted removed, run
  seed down [name]   what the seeds inserted removed (seed reset: all of them)

  command:scope      the same in migrations/scope/ - up:billing, create:billing
  db:create name     a database, if it is not there yet
  db:drop name       a database, if it is there

options:
  -e, --env NAME              entry of database.json (NODE_ENV, defaultEnv, dev)
  --config FILE               database.json somewhere else, or a .yml
  -m, --migrations-dir DIR    where migrations are (migrations)
  -c, --count N               at most N migrations
  -t, --table NAME            the table the history is kept in (migrations)
  -s, --state-table NAME      node's state and lock table (migrations_state)
  --lock-timeout MS           a lock untouched this long is taken over (60000)
  --lock-interval MS          how often a waiting process looks (1000)
  --dry-run                   print the statements instead of sending them
  --check                     with up or sync: list what would run
  -v, --verbose               print every statement as it is sent
  --non-transactional         no transaction around a migration
  --sql-file                  create: an up and a down .sql file instead
  --sql                       create: one .sql file with an up and a down
                              section, as db-migrate-plugin-sql writes it
  --v2-file                   create: a v2 migration, undone by what it learns
  --ignore-on-init            create: an up that is skipped when run with it;
                              up: record those without running them
  --log-level LEVELS          what is printed: info|warn|error|sql
  --template NAME             create: what the plugin of that name writes
  -i, --version               print the version
  -h, --help                  this
`);
  return 0;
}

static int usage(void) {
  dbmSay(stderr, TEXT`usage: migrate up|down|sync|reset|check|create [name] [options] - --help says which\n`);
  return 2;
}

static bool readOptions(int argc, char **argv, options_t *into) {

  for (int i = 1; i < argc; ++i) {

    const char *word = argv[i];
    bool hasNext = i + 1 < argc;

    if (word in {"-e", "--env"} && hasNext)
      into->env = argv[++i];
    else if (strcmp(word, "--config") == 0 && hasNext)
      into->config = argv[++i];
    else if (word in {"-m", "--migrations-dir"} && hasNext)
      into->dir = argv[++i];
    else if (word in {"-t", "--table", "--migration-table"} && hasNext)
      into->table = argv[++i];
    else if (word in {"-s", "--state", "--state-table"} && hasNext)
      into->stateTable = argv[++i];
    else if (strcmp(word, "--lock-timeout") == 0 && hasNext)
      into->lockTimeout = strtol(argv[++i], NULL, 10);
    else if (strcmp(word, "--lock-interval") == 0 && hasNext)
      into->lockInterval = strtol(argv[++i], NULL, 10);
    else if (word in {"-v", "--verbose"})
      into->verbose = true;
    else if (strcmp(word, "--non-transactional") == 0)
      into->noTransactions = true;
    else if (strcmp(word, "--backup-state") == 0)
      into->backupState = true;
    else if (strcmp(word, "--check") == 0)
      into->checkOnly = true;
    else if (word in {"-h", "--help", "-?"})
      into->help = true;
    else if (word in {"-i", "--version"})
      into->version = true;
    else if (word in {"-c", "--count"} && hasNext) {
      into->count = strtoul(argv[++i], NULL, 10);
      into->countGiven = true;
    } else if (strcmp(word, "--dry-run") == 0)
      into->dryRun = true;
    else if (strcmp(word, "--sql-file") == 0)
      into->sqlFile = true;
    else if (strcmp(word, "--sql") == 0)
      into->sql = true;
    else if (strcmp(word, "--v2-file") == 0)
      into->v2File = true;
    else if (strcmp(word, "--ignore-on-init") == 0)
      into->ignoreOnInit = true;
    else if (strcmp(word, "--log-level") == 0 && hasNext)
      into->logLevel = argv[++i];
    /* node reads it into a setting nothing looks at; taken, so scripts work */
    else if (strcmp(word, "--ignore-completed-migrations") == 0)
      ;
    else if (strcmp(word, "--template") == 0 && hasNext)
      into->template = argv[++i];
    /* the launcher reads it; a program has its seeds compiled in */
    else if (strcmp(word, "--seeds-dir") == 0 && hasNext)
      ++i;
    else if (word[0] == '-') {
      dbmSay(stderr, TEXT`unknown option ${word}\n`);
      return false;
    } else if (into->command == NULL)
      into->command = word;
    else if (into->name == NULL)
      into->name = word;
    else if (into->second == NULL && strncmp(into->command, "seed", 4) == 0)
      into->second = word;
    else {
      dbmSay(stderr, TEXT`one argument too many: ${word}\n`);
      return false;
    }
  }

  return into->command != NULL || into->help || into->version;
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

/** The driver a URL's scheme names: `postgres://` is pg. */
static const char *driverOf(const char *url) {

  if (strncmp(url, "postgres://", 11) == 0 ||
      strncmp(url, "postgresql://", 13) == 0)
    return "pg";

  if (strncmp(url, "mysql://", 8) == 0)
    return "mysql";

  if (strncmp(url, "sqlite", 6) == 0)
    return "sqlite3";

  return "";
}

/**
 * One entry of database.json with `{"ENV": "NAME"}` values replaced by the
 * variable they name, which is how node db-migrate keeps passwords out of the
 * file. A string entry is a URL and stays one.
 */
/** Whether a value is `{"ENV": "NAME", ...}`. */
static bool namesVariable(json_t value) {
  return strcmp(value.kind(), "object") == 0 &&
         strcmp(value.ENV.kind(), "string") == 0;
}

/**
 * `{"ENV": "NAME", "default": value}`: the variable, or the default when it
 * is unset or empty - node 1.8's - or what is there, null when nothing is.
 */
static yyjson_mut_val *fromEnv(yyjson_mut_doc *doc, json_t entry) {

  const char *set = getenv(entry.ENV.text());
  json_t fallback = entry.get("default");

  if (set != NULL && set[0] != '\0')
    return yyjson_mut_strcpy(doc, set);

  if (strcmp(fallback.kind(), "nothing") != 0)
    return yyjson_val_mut_copy(doc, fallback.node);

  return set != NULL ? yyjson_mut_strcpy(doc, set) : yyjson_mut_null(doc);
}

/** A value with every `{"ENV": ...}` in it resolved, at any depth. */
static yyjson_mut_val *walked(yyjson_mut_doc *doc, json_t value) {

  if (namesVariable(value))
    return fromEnv(doc, value);

  if (strcmp(value.kind(), "object") == 0) {

    yyjson_mut_val *object = yyjson_mut_obj(doc);

    for (int i = 0; i < value.count(); ++i)
      yyjson_mut_obj_add(object, yyjson_mut_strcpy(doc, value.keyAt(i)),
                         walked(doc, value.get(value.keyAt(i))));
    return object;
  }

  if (strcmp(value.kind(), "array") == 0) {

    yyjson_mut_val *array = yyjson_mut_arr(doc);

    for (int i = 0; i < value.count(); ++i)
      yyjson_mut_arr_append(array, walked(doc, value.at(i)));
    return array;
  }

  return yyjson_val_mut_copy(doc, value.node);
}

static json_t resolved(json_t entry) {

  if (strcmp(entry.kind(), "object") != 0) {

    const char *url = entry.text();

    return {driver: driverOf(url), url: url};
  }

  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *object;

  /**
   * An environment that is a URL from a variable: the URL, and the keys
   * written beside it added to it - node 1.8 stopped dropping them.
   */
  if (namesVariable(entry)) {

    yyjson_mut_val *url = fromEnv(doc, entry);
    const char *said = yyjson_mut_is_str(url) ? yyjson_mut_get_str(url) : "";

    object = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, object, "driver", driverOf(said));
    yyjson_mut_obj_add(object, yyjson_mut_str(doc, "url"), url);

    for (int i = 0; i < entry.count(); ++i)
      if (!(entry.keyAt(i) in {"ENV", "default"}))
        yyjson_mut_obj_put(object, yyjson_mut_strcpy(doc, entry.keyAt(i)),
                           walked(doc, entry.get(entry.keyAt(i))));
  } else {
    object = walked(doc, entry);
  }

  yyjson_mut_doc_set_root(doc, object);
  return meta_jsonFromMut(doc);
}

static bool exists(const char *path) {
  struct stat seen;
  return stat(path, &seen) == 0;
}

/**
 * The file the configuration is in: the one --config names, database.json,
 * or database.yml and the other endings plugins read - in that order.
 */
static const char *configFile(const options_t *options, char *found,
                              size_t room) {

  const char *extension;

  if (options->config != NULL)
    return options->config;

  if (exists("database.json"))
    return "database.json";

  for (size_t at = 0; (extension = dbmConfigExtension(at)) != NULL; ++at) {
    dbmWrite(found, room, TEXT`database${extension}`);

    if (exists(found))
      return found;
  }

  return "database.json";
}

/** The whole file, read as JSON or by the plugin for its ending. */
static json_t configFileRead(const char *path, char *why, size_t room) {

  dbm_config_loader_t load = dbmConfigLoaderFor(path);

  if (load != NULL) {

    json_t read = meta_toJSON("null");

    if (!load(path, &read, why, room) && why[0] == '\0')
      dbmWrite(why, room, TEXT`${path} could not be read`);

    return read;
  }

  char *text = slurp(path);
  json_t file = meta_toJSON(text);
  free(text);

  const char *dot = strrchr(path, '.');

  if (file.refused != NULL && dbmPluginLoadError()[0] != '\0')
    dbmWrite(why, room, TEXT`${path} needs a plugin: ${dbmPluginLoadError()}`);
  else if (file.refused != NULL && dot != NULL && strcmp(dot, ".json") != 0)
    dbmWrite(why, room, TEXT`nothing in this program reads ${dot} files - link the plugin that does (build-app.sh <app> <out> <driver> ${strcmp(dot, ".yml") == 0 || strcmp(dot, ".yaml") == 0 ? "yaml" : "<plugin>"})`);
  else if (file.refused != NULL)
    dbmWrite(why, room, TEXT`${path} is not JSON: ${file.refused}`);

  return file;
}

/** The configuration to open, or one that answers `isNothing` and a reason. */
static json_t configuration(const options_t *options, char *why, size_t room) {

  char found[64];
  const char *path = configFile(options, found, sizeof found);

  if (!exists(path)) {

    const char *url = getenv("DATABASE_URL");

    if (options->config == NULL && url != NULL && url[0] != '\0')
      return {driver: driverOf(url), url: url};

    dbmWrite(why, room, TEXT`there is no ${path} here${options->config == NULL ? " and no DATABASE_URL" : ""}`);
    return meta_toJSON("null");
  }

  json_t file = configFileRead(path, why, room);
  defer file.release();

  if (why[0] != '\0')
    return meta_toJSON("null");

  const char *env = options->env;

  if (env == NULL)
    env = getenv("NODE_ENV");

  /* defaultEnv, from a variable too; unset, it is dev - or development */
  char chosen[256] = "";

  if (env == NULL || env[0] == '\0') {

    json_t fallback = file.defaultEnv;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *named = walked(doc, fallback);

    if (yyjson_mut_is_str(named))
      dbmWrite(chosen, sizeof chosen, TEXT`${yyjson_mut_get_str(named)}`);

    yyjson_mut_doc_free(doc);

    env = chosen[0] != '\0'                         ? chosen
          : file.get("dev").isNothing() &&
                  !file.get("development").isNothing() ? "development"
                                                       : "dev";
  }

  json_t entry = file.get(env);

  if (entry.isNothing()) {

    /* the ones there are, so the typo is visible next to the right word */
    dbm_text_t known = {0};
    defer known.release();

    for (int i = 0; i < file.count(); ++i)
      if (strcmp(file.keyAt(i), "defaultEnv") != 0)
        known.append(TEXT`${known.length > 0 ? ", " : ""}${file.keyAt(i)}`);

    dbmWrite(why, room, TEXT`${path} has no environment called ${env} - there is ${known.text != NULL ? known.text : "none"}`);
    return meta_toJSON("null");
  }

  return resolved(entry);
}

/**
 * One SQL file of a migration, with what node's template puts in it - and
 * for an up that --ignore-on-init skips, the line that says so first.
 */
static bool writeSql(const char *path, bool ignorable) {

  FILE *file = fopen(path, "wx");

  if (file == NULL) {
    perror(path);
    return false;
  }

  if (ignorable)
    dbmSay(file, TEXT`${DBM_IGNORE_ON_INIT_MARK}\n`);

  dbmSay(file, TEXT`/* Replace with your SQL commands */`);
  fclose(file);
  dbmSay(stdout, TEXT`[INFO] Created migration at ${path}\n`);
  return true;
}

/**
 * `migrations/sqls/<stamp>-<name>-up.sql` and `-down.sql`, where node
 * db-migrate's `create --sql-file` puts them. No file of code beside them:
 * node needs a stub to read them, and here the migration *is* the two files.
 */
static int createSqlFiles(const char *dir, const char *stamp,
                          const char *name, bool ignorable) {

  char sqls[512];
  char path[640];

  dbmWrite(sqls, sizeof sqls, TEXT`${dir}/sqls`);
  mkdir(sqls, 0755);

  dbmWrite(path, sizeof path, TEXT`${sqls}/${stamp}-${name}-up.sql`);

  if (!writeSql(path, ignorable))
    return 1;

  dbmWrite(path, sizeof path, TEXT`${sqls}/${stamp}-${name}-down.sql`);

  return writeSql(path, false) ? 0 : 1;
}

/** `migrations/20261008120000-add-pets.c`, with an up and a down to fill in. */
static int create(const options_t *options) {

  char stamp[32];
  char path[512];
  time_t now = time(NULL);
  struct tm utc;
  char scoped[512];
  const char *dir = options->dir != NULL ? options->dir : "migrations";

  /* `create:billing add-tax` goes to migrations/billing/ */
  if (options->scope != NULL && options->scope[0] != '\0') {
    mkdir(dir, 0755);
    dbmWrite(scoped, sizeof scoped, TEXT`${dir}/${options->scope}`);
    dir = scoped;
  }

  if (options->name == NULL) {
    dbmSay(stderr, TEXT`create needs a name: migrate create add-pets\n`);
    return 2;
  }

  gmtime_r(&now, &utc);
  strftime(stamp, sizeof stamp, "%Y%m%d%H%M%S", &utc);

  mkdir(dir, 0755);

  if (options->template != NULL) {

    dbm_template_t write = dbmTemplateNamed(options->template);

    if (write == NULL) {
      dbmSay(stderr, TEXT`[ERROR] there is no template called ${options->template} - a plugin registers one with dbmRegisterTemplate\n`);
      return 1;
    }

    return write(dir, stamp, options->name);
  }

  /* db-migrate-plugin-sql's: one file, with the template it writes */
  if (options->sql) {

    dbmWrite(path, sizeof path, TEXT`${dir}/${stamp}-${options->name}.sql`);

    FILE *file = fopen(path, "wx");

    if (file == NULL) {
      perror(path);
      return 1;
    }

    dbmSay(file, TEXT`-- up\n\n\n-- down\n\n`);
    fclose(file);
    dbmSay(stdout, TEXT`[INFO] Created migration at ${path}\n`);
    return 0;
  }

  if (options->sqlFile)
    return createSqlFiles(dir, stamp, options->name, options->ignoreOnInit);

  dbmWrite(path, sizeof path, TEXT`${dir}/${stamp}-${options->name}.c`);

  FILE *file = fopen(path, "wx");

  if (file == NULL) {
    perror(path);
    return 1;
  }

  if (options->v2File)
    dbmSay(file, TEXT`#include <db_migrate.h>

static int migrate(schema_t *db) {
  return 0;
}

DBM_MIGRATION_V2(migrate)
`);
  else if (options->ignoreOnInit)
    dbmSay(file, TEXT`#include <db_migrate.h>

static int up(migrator_t *db) {

  /* the database this is run on with --ignore-on-init has it already */
  if (db->ignoreOnInit)
    return 0;

  return 0;
}

static int down(migrator_t *db) {
  return 0;
}

DBM_MIGRATION(up, down)
`);
  else
    dbmSay(file, TEXT`#include <db_migrate.h>

static int up(migrator_t *db) {
  return 0;
}

static int down(migrator_t *db) {
  return 0;
}

DBM_MIGRATION(up, down)
`);

  fclose(file);
  dbmSay(stdout, TEXT`[INFO] Created migration at ${path}\n`);
  return 0;
}

/**
 * `db:create shop` and `db:drop shop`, as node does them: connected with
 * everything the configuration says except which database, since the one
 * being made does not exist yet - and if it exists already, that is fine.
 */
static int database(const options_t *options, json_t config) {

  char why[512] = "";
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *bare = yyjson_val_mut_copy(doc, config.node);

  if (yyjson_mut_is_obj(bare))
    yyjson_mut_obj_remove_key(bare, "database");

  yyjson_mut_doc_set_root(doc, bare);

  json_t without = meta_jsonFromMut(doc);
  defer without.release();

  driver_t *driver = dbmOpen(without, why, sizeof why);

  if (driver == NULL) {
    dbmSay(stderr, TEXT`[ERROR] ${why}\n`);
    return 1;
  }

  bool creating = strcmp(options->scope, "create") == 0;

  driver->dryRun = options->dryRun;
  driver->verbose = options->verbose;

  int answer = creating
                   ? driver->createDatabase(driver, options->name, true)
                   : driver->dropDatabase(driver, options->name, true);

  if (answer != 0)
    dbmSay(stderr, TEXT`[ERROR] could not ${creating ? "create" : "drop"} ${options->name}: ${driver->error}\n`);
  else
    dbmSay(stdout, TEXT`[INFO] ${creating ? "Created" : "Deleted"} database "${options->name}"\n`);

  dbmClose(driver);
  return answer == 0 ? 0 : 1;
}

/**
 * The connection, and node's state - the lock, and what v2 migrations
 * learned - through a connection of its own, so it outlives a migration's
 * transaction being rolled back on the other one. NULL, with the reason.
 */
static driver_t *connected(json_t config, const dbm_options_t *options,
                           bool readOnly, driver_t **stateDriver, char *why,
                           size_t room) {

  driver_t *driver = dbmOpen(config, why, room);

  if (driver == NULL)
    return NULL;

  if (options->migrationTable != NULL)
    driver->migrationTable = options->migrationTable;

  driver->verbose = options->verbose;

  *stateDriver = dbmOpen(config, why, room);

  if (*stateDriver == NULL) {
    dbmClose(driver);
    return NULL;
  }

  (*stateDriver)->verbose = options->verbose;
  driver->state = dbmStateOpen(
      *stateDriver,
      options->stateTable != NULL ? options->stateTable : "migrations_state",
      options->lockTimeout, options->lockInterval, readOnly);

  if (driver->state == NULL) {
    dbmWrite(why, room, TEXT`could not open the state table: ${(*stateDriver)->error}`);
    dbmClose(*stateDriver);
    dbmClose(driver);
    return NULL;
  }

  return driver;
}

static void disconnected(driver_t *driver, driver_t *stateDriver) {
  dbmStateClose(driver->state);
  dbmClose(stateDriver);
  dbmClose(driver);
}

/**
 * The scopes `all` runs in: the top level first, then every scope this
 * program has migrations of, in name order - `a`, `a/nested`, `b` - as
 * node walks the folders.
 */
static size_t scopesOf(const char **into, size_t room) {

  size_t count = 0;
  size_t total;
  const dbm_migration_t *migrations = dbmMigrations(&total);

  into[count++] = "";

  for (size_t i = 0; i < total && count < room; ++i) {

    static char names[256][256];
    const char *name = migrations[i].name;
    const char *slash = strrchr(name, '/');

    if (slash == NULL || slash == name)
      continue;

    char scope[256];
    size_t length = (size_t)(slash - name);

    if (length >= sizeof scope)
      continue;

    memcpy(scope, name, length);
    scope[length] = '\0';

    bool seen = false;

    for (size_t at = 1; at < count && !seen; ++at)
      seen = strcmp(into[at], scope) == 0;

    if (!seen) {
      memcpy(names[count], scope, length + 1);
      into[count] = names[count];
      ++count;
    }
  }

  /* migrations are sorted by name, so their scopes are too - but `a/x`
     sorts after `a/nested/y`'s scope only by luck; sort to be sure */
  for (size_t a = 2; a < count; ++a)
    for (size_t b = a; b > 1 && strcmp(into[b - 1], into[b]) > 0; --b) {
      const char *swap = into[b];
      into[b] = into[b - 1];
      into[b - 1] = swap;
    }

  return count;
}

/** Whether a configuration says anything but which database or schema. */
static bool connectsOnItsOwn(json_t scope) {

  for (int i = 0; i < scope.count(); ++i)
    if (!(scope.keyAt(i) in {"database", "schema"}))
      return true;

  return false;
}

/**
 * The connection a scope uses, node 1.1's way: `<migrations>/<scope>/config.json`
 * (or what build-app.sh compiled in) on top of the environment's. One that
 * says only which database or schema switches to it - for PostgreSQL a
 * database is a schema there - one that says more connects on its own,
 * inheriting what it does not say. Lock and state then live where it does.
 */
static json_t scopedConfiguration(const options_t *options, json_t config,
                                  const char *scope, char *why, size_t room) {

  char path[1024];
  char *text = NULL;
  const char *dir = options->dir != NULL ? options->dir : "migrations";

  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *merged = yyjson_val_mut_copy(doc, config.node);

  yyjson_mut_doc_set_root(doc, merged);

  if (scope[0] != '\0') {

    dbmWrite(path, sizeof path, TEXT`${dir}/${scope}/config.json`);
    text = slurp(path);

    if (text == NULL && dbmScopeConfig(scope) != NULL)
      text = strdup(dbmScopeConfig(scope));
  }

  if (text == NULL)
    return meta_jsonFromMut(doc);

  json_t read = meta_toJSON(text);
  free(text);
  defer read.release();

  if (read.refused != NULL || strcmp(read.kind(), "object") != 0) {
    dbmWrite(why, room, TEXT`${path} is not a JSON object${read.refused != NULL ? ": " : ""}${read.refused != NULL ? read.refused : ""}`);
    yyjson_mut_doc_free(doc);
    return meta_toJSON("null");
  }

  json_t scoped = resolved(read);
  defer scoped.release();

  if (options->verbose)
    dbmSay(stdout, TEXT`[INFO] loaded extra config for migration subfolder: "${scope}/config.json"\n`);

  const char *driver = config.driver;
  bool own = connectsOnItsOwn(scoped);

  for (int i = 0; i < scoped.count(); ++i) {

    const char *key = scoped.keyAt(i);
    json_t value = scoped.get(key);

    /* switching only: PostgreSQL has no databases to switch to, a schema */
    if (!own && strcmp(key, "database") == 0 &&
        (driver in {"pg", "postgres", "postgresql", "cockroachdb"}))
      key = "schema";

    /* SQLite switches nothing; a file of its own is a connection of its own */
    if (!own && strcmp(driver, "sqlite3") == 0)
      continue;

    yyjson_mut_obj_remove_key(merged, key);
    yyjson_mut_obj_add(merged, yyjson_mut_strcpy(doc, key),
                       yyjson_val_mut_copy(doc, value.node));
  }

  return meta_jsonFromMut(doc);
}

/** One command in one scope: its connection, its tunnel, its lock. */
static int inScope(options_t *options, json_t environment, const char *scope) {

  char why[512] = "";

  dbmUseScope(scope);

  json_t config = scopedConfiguration(options, environment, scope, why,
                                      sizeof why);
  defer config.releaseAt();

  dbm_tunnel_t *tunnel = NULL;

  /* through the tunnel, if the connection has one, for everything below */
  if (why[0] != '\0' || !dbmTunnelOpen(&config, &tunnel, why, sizeof why)) {
    dbmSay(stderr, TEXT`[ERROR] ${why}\n`);
    return 1;
  }

  defer dbmTunnelClose(tunnel);

  dbm_options_t settings = {
      .migrationTable = options->table,
      .stateTable = options->stateTable,
      .lockTimeout = options->lockTimeout,
      .lockInterval = options->lockInterval,
      .verbose = options->verbose,
  };
  driver_t *stateDriver;
  driver_t *driver = connected(
      config, &settings,
      options->dryRun || strcmp(options->command, "check") == 0, &stateDriver,
      why, sizeof why);

  if (driver == NULL) {
    dbmSay(stderr, TEXT`[ERROR] ${why}\n`);
    return 1;
  }

  int answer;

  driver->noTransactions = options->noTransactions;
  driver->ignoreOnInit = options->ignoreOnInit;

  /**
   * `down` undoes one unless told otherwise - but with a destination it
   * undoes everything after it, as node does, and a count limits that.
   */
  size_t downCount = options->countGiven ? options->count
                     : options->name != NULL ? 0
                                             : 1;

  if (strcmp(options->command, "up") == 0)
    answer = dbmUp(driver, options->count, options->name, options->dryRun);
  else if (strcmp(options->command, "down") == 0)
    answer = dbmDown(driver, downCount, options->name, options->dryRun);
  else if (strcmp(options->command, "sync") == 0)
    answer = dbmSync(driver, options->name, options->dryRun);
  else if (strcmp(options->command, "fix") == 0)
    answer = dbmFix(driver, options->backupState, options->dryRun);
  else if (strcmp(options->command, "reset") == 0)
    answer = dbmReset(driver, options->dryRun);
  else if (strcmp(options->command, "seed") == 0) {

    /* `seed [name]`, `seed down [name]`, `seed reset` - as node's */
    bool undo = options->name != NULL &&
                (options->name in {"down", "reset"});
    char why[600] = "";

    answer = dbmSeed(driver, driver->state,
                     undo ? options->second : options->name, undo,
                     options->dryRun, why, sizeof why);

    if (answer != 0)
      dbmSay(stderr, TEXT`[ERROR] ${why}\n`);
    else
      dbmSay(stdout, TEXT`[INFO] Done\n`);
  }
  else
    answer = dbmCheck(driver);

  disconnected(driver, stateDriver);
  return answer == 0 ? 0 : 1;
}

int dbmCli(int argc, char **argv) {

  options_t options = {0};
  char why[512] = "";

  if (!readOptions(argc, argv, &options))
    return usage();

  if (options.help)
    return help();

  if (options.logLevel != NULL)
    dbmSetLogLevel(options.logLevel);

  /* `up:billing` is the command `up` in the scope `billing` */
  char command[32] = "";
  const char *colon = options.command != NULL ? strchr(options.command, ':')
                                              : NULL;

  if (colon != NULL) {

    size_t length = (size_t)(colon - options.command);

    if (length >= sizeof command)
      return usage();

    memcpy(command, options.command, length);
    command[length] = '\0';
    options.command = command;
    options.scope = colon + 1;
  }

  dbmUseScope(options.scope);

  if (options.version) {
    dbmSay(stdout, TEXT`${DBM_VERSION}\n`);
    return 0;
  }

  if (strcmp(options.command, "create") == 0)
    return create(&options);

  if (!(options.command in {"up", "down", "reset", "check", "sync", "db",
                            "fix", "seed"}))
    return usage();

  if (strcmp(options.command, "db") == 0 &&
      (options.name == NULL || options.scope == NULL ||
       !(options.scope in {"create", "drop"}))) {
    dbmSay(stderr, TEXT`db:create or db:drop, and a name: migrate db:create shop\n`);
    return 2;
  }

  if (strcmp(options.command, "sync") == 0 && options.name == NULL) {
    dbmSay(stderr, TEXT`sync needs a destination: migrate sync 20261008120000\n`);
    return 2;
  }

  /* by reference: the tunnel below may put another configuration in its place */
  json_t config = configuration(&options, why, sizeof why);
  defer config.releaseAt();

  if (why[0] != '\0') {
    dbmSay(stderr, TEXT`[ERROR] ${why}\n`);
    return 1;
  }

  /* `up --check` is node's way of asking `check` */
  if (options.checkOnly && options.command in {"up", "sync"})
    options.command = "check";

  if (strcmp(options.command, "db") == 0) {

    dbm_tunnel_t *tunnel = NULL;

    if (!dbmTunnelOpen(&config, &tunnel, why, sizeof why)) {
      dbmSay(stderr, TEXT`[ERROR] ${why}\n`);
      return 1;
    }

    int answer = database(&options, config);

    dbmTunnelClose(tunnel);
    return answer;
  }

  /* `all`: the top level, then every scope, each on its own connection */
  if (options.scope != NULL && strcmp(options.scope, "all") == 0) {

    const char *scopes[256];
    size_t count = scopesOf(scopes, countof(scopes));

    for (size_t at = 0; at < count; ++at) {

      dbmSay(stdout, TEXT`[INFO] Enter scope "${scopes[at][0] != '\0' ? scopes[at] : "/"}"\n`);

      if (inScope(&options, config, scopes[at]) != 0)
        return 1;
    }

    return 0;
  }

  return inScope(&options, config, options.scope != NULL ? options.scope : "")
             ? 1
             : 0;
}

int dbmMigrateUp(json_t config, const dbm_options_t *options, char *why,
                 size_t room) {

  static const dbm_options_t defaults = {0};

  if (options == NULL)
    options = &defaults;

  why[0] = '\0';

  /* a copy, since the tunnel puts another in its place and frees this one */
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_doc_set_root(doc, yyjson_val_mut_copy(doc, config.node));

  json_t own = meta_jsonFromMut(doc);
  defer own.releaseAt();

  dbm_tunnel_t *tunnel = NULL;

  if (!dbmTunnelOpen(&own, &tunnel, why, room))
    return -1;

  defer dbmTunnelClose(tunnel);

  driver_t *stateDriver;
  driver_t *driver = connected(own, options, false, &stateDriver, why, room);

  if (driver == NULL)
    return -1;

  int answer = dbmUp(driver, 0, NULL, false);

  disconnected(driver, stateDriver);

  if (answer != 0)
    dbmWrite(why, room, TEXT`${dbmLastError()[0] != '\0' ? dbmLastError() : "migrating failed"}`);

  return answer == 0 ? 0 : -1;
}
