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
  size_t count;
  bool countGiven;
  bool dryRun;
  bool sqlFile;
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
                     (--sql-file, --v2-file or --template NAME for another kind)

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
    else if (word[0] == '-') {
      dbmSay(stderr, TEXT`unknown option ${word}\n`);
      return false;
    } else if (into->command == NULL)
      into->command = word;
    else if (into->name == NULL)
      into->name = word;
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
static json_t resolved(json_t entry) {

  if (strcmp(entry.kind(), "object") != 0) {

    const char *url = entry.text();

    return {driver: driverOf(url), url: url};
  }

  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *object = yyjson_mut_obj(doc);

  for (int i = 0; i < entry.count(); ++i) {

    const char *key = entry.keyAt(i);
    json_t value = entry.get(key);
    const char *variable = value.ENV;

    if (strcmp(value.kind(), "object") == 0 && variable[0] != '\0') {

      const char *set = getenv(variable);

      yyjson_mut_obj_add(object, yyjson_mut_strcpy(doc, key),
                         set != NULL ? yyjson_mut_strcpy(doc, set)
                                     : yyjson_mut_null(doc));
      continue;
    }

    yyjson_mut_obj_add(object, yyjson_mut_strcpy(doc, key),
                       yyjson_val_mut_copy(doc, value.node));
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

  if (file.refused != NULL && dot != NULL && strcmp(dot, ".json") != 0)
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

  if (env == NULL || env[0] == '\0') {
    const char *fallback = file.defaultEnv;
    env = fallback[0] != '\0' ? fallback : "dev";
  }

  json_t entry = file.get(env);

  if (entry.isNothing()) {

    /* the ones there are, so the typo is visible next to the right word */
    dbm_text_t known = {0};
    defer known.release();

    for (int i = 0; i < file.count(); ++i)
      if (strcmp(file.keyAt(i), "defaultEnv") != 0)
        known.append(TEXT`${i > 0 ? ", " : ""}${file.keyAt(i)}`);

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
                            "fix"}))
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

  dbm_tunnel_t *tunnel = NULL;

  /* through the tunnel, if the connection has one, for everything below */
  if (why[0] != '\0' || !dbmTunnelOpen(&config, &tunnel, why, sizeof why)) {
    dbmSay(stderr, TEXT`[ERROR] ${why}\n`);
    return 1;
  }

  defer dbmTunnelClose(tunnel);

  if (strcmp(options.command, "db") == 0)
    return database(&options, config);

  driver_t *driver = dbmOpen(config, why, sizeof why);

  if (driver == NULL) {
    dbmSay(stderr, TEXT`[ERROR] ${why}\n`);
    return 1;
  }

  int answer;

  if (options.table != NULL)
    driver->migrationTable = options.table;

  driver->verbose = options.verbose;
  driver->noTransactions = options.noTransactions;
  driver->ignoreOnInit = options.ignoreOnInit;

  /**
   * node's state - the lock, and what v2 migrations learned - through a
   * connection of its own, so it outlives a migration's transaction being
   * rolled back on the other one.
   */
  driver_t *stateDriver = dbmOpen(config, why, sizeof why);

  if (stateDriver == NULL) {
    dbmSay(stderr, TEXT`[ERROR] ${why}\n`);
    dbmClose(driver);
    return 1;
  }

  stateDriver->verbose = options.verbose;
  driver->state = dbmStateOpen(
      stateDriver, options.stateTable != NULL ? options.stateTable
                                              : "migrations_state",
      options.lockTimeout, options.lockInterval,
      options.dryRun || strcmp(options.command, "check") == 0);

  if (driver->state == NULL) {
    dbmSay(stderr, TEXT`[ERROR] could not open the state table: ${stateDriver->error}\n`);
    dbmClose(stateDriver);
    dbmClose(driver);
    return 1;
  }

  /* `up --check` is node's way of asking `check` */
  if (options.checkOnly && options.command in {"up", "sync"})
    options.command = "check";

  /**
   * `down` undoes one unless told otherwise - but with a destination it
   * undoes everything after it, as node does, and a count limits that.
   */
  size_t downCount = options.countGiven ? options.count
                     : options.name != NULL ? 0
                                            : 1;

  if (strcmp(options.command, "up") == 0)
    answer = dbmUp(driver, options.count, options.name, options.dryRun);
  else if (strcmp(options.command, "down") == 0)
    answer = dbmDown(driver, downCount, options.name, options.dryRun);
  else if (strcmp(options.command, "sync") == 0)
    answer = dbmSync(driver, options.name, options.dryRun);
  else if (strcmp(options.command, "fix") == 0)
    answer = dbmFix(driver, options.backupState, options.dryRun);
  else if (strcmp(options.command, "reset") == 0)
    answer = dbmReset(driver, options.dryRun);
  else
    answer = dbmCheck(driver);

  dbmStateClose(driver->state);
  dbmClose(stateDriver);
  dbmClose(driver);
  return answer == 0 ? 0 : 1;
}
