/**
 * meta-migrate: the development launcher.
 *
 *   meta-migrate up
 *   meta-migrate down
 *   meta-migrate up            # after editing the migration: only it is rebuilt
 *
 * A program ships with its migrations compiled in. While a migration is being
 * written that is the wrong shape - `down`, edit, `up` should not rebuild an
 * application - so this does what a dynamic language does with a file: reads
 * it when it is asked to run it.
 *
 * The migrations directory is read by name only. Which migrations run is the
 * database's to say, and only those are compiled - each lowered by meta into a
 * shared object of its own, kept under `.meta-migrate/` by a hash of its text,
 * so one that has not changed is opened as it was. Opening one runs its
 * `DBM_MIGRATION` constructor, which registers it the same way it registers in
 * a program it is linked into, so from there on this is the same code path as
 * the shipped binary: `dbmCli` with the same arguments.
 *
 * Drivers are loaded the same way and only when the configuration names one,
 * so libpq is needed on a machine that talks to PostgreSQL and nowhere else.
 * The shipped plugins too: libyaml only where there is a database.yml.
 *
 * A project's own plugins, in plugins/, are compiled the same way as its
 * migrations and opened first, every time - they may be what reads the
 * configuration.
 *
 * Where meta, its runtime headers, db-migrate's headers and the drivers are
 * is baked in when this is built, and can be moved with META_ROOT,
 * DBM_INCLUDE and DBM_DRIVERS. Where the baked-in place is gone - a release,
 * unpacked somewhere else - they are looked for beside this program:
 *
 *   <prefix>/bin/meta-migrate
 *   <prefix>/include/            db-migrate's headers
 *   <prefix>/lib/                the drivers and plugins
 *   <prefix>/lib/meta/           meta and its runtime/include
 */
#include <db_migrate.h>

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef DBM_META_ROOT
#define DBM_META_ROOT "."
#endif

#ifndef DBM_INCLUDE_DIR
#define DBM_INCLUDE_DIR "include"
#endif

#ifndef DBM_DRIVER_DIR
#define DBM_DRIVER_DIR "."
#endif

static const char *setting(const char *variable, const char *fallback) {

  const char *set = getenv(variable);

  return set != NULL && set[0] != '\0' ? set : fallback;
}

/** `<prefix>` of this program, from /proc/self/exe: the directory above bin/. */
static const char *prefix(void) {

  static char found[1024];
  ssize_t length;

  if (found[0] != '\0')
    return found;

  length = readlink("/proc/self/exe", found, sizeof found - 1);

  if (length <= 0) {
    found[0] = '\0';
    return ".";
  }

  found[length] = '\0';

  for (int up = 0; up < 2; ++up) {
    char *slash = strrchr(found, '/');

    if (slash == NULL)
      return ".";

    *slash = '\0';
  }

  return found;
}

/**
 * A directory: the variable, else what was baked in if it is still there,
 * else `<prefix>/<beside>`.
 */
static const char *place(const char *variable, const char *baked,
                         const char *beside, char *into, size_t room) {

  const char *set = getenv(variable);
  struct stat seen;

  if (set != NULL && set[0] != '\0')
    return set;

  if (stat(baked, &seen) == 0)
    return baked;

  dbmWrite(into, room, TEXT`${prefix()}/${beside}`);
  return into;
}

static const char *metaRoot(void) {
  static char found[1100];
  return place("META_ROOT", DBM_META_ROOT, "lib/meta", found, sizeof found);
}

static const char *includeDirectory(void) {
  static char found[1100];
  return place("DBM_INCLUDE", DBM_INCLUDE_DIR, "include", found, sizeof found);
}

static const char *driverDirectory(void) {
  static char found[1100];
  return place("DBM_DRIVERS", DBM_DRIVER_DIR, "lib", found, sizeof found);
}

/** FNV-1a over the file, enough to tell an edited migration from the last one. */
static unsigned long long hashOf(const char *path, bool *ok) {

  unsigned long long hash = 1469598103934665603ULL;
  FILE *file = fopen(path, "rb");
  int c;

  *ok = file != NULL;

  if (file == NULL)
    return 0;

  while ((c = fgetc(file)) != EOF) {
    hash ^= (unsigned char)c;
    hash *= 1099511628211ULL;
  }

  fclose(file);
  return hash;
}

/** Runs a program, its output into `log`. Answers whether it succeeded. */
static bool run(char *const *argv, const char *log) {

  /* or the child's freopen writes what this process had buffered, again */
  fflush(stdout);
  fflush(stderr);

  pid_t child = fork();
  int status;

  if (child < 0)
    return false;

  if (child == 0) {

    FILE *into = freopen(log, "w", stdout);

    if (into != NULL)
      dup2(fileno(stdout), fileno(stderr));

    execvp(argv[0], argv);
    _exit(127);
  }

  if (waitpid(child, &status, 0) < 0)
    return false;

  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void show(const char *log) {

  FILE *file = fopen(log, "r");
  char line[512];

  if (file == NULL)
    return;

  while (fgets(line, sizeof line, file) != NULL)
    fputs(line, stderr);

  fclose(file);
}

/** Whether the lowered file says meta could not write part of it back. */
static bool refused(const char *lowered) {

  FILE *file = fopen(lowered, "r");
  char line[512];
  bool found = false;

  if (file == NULL)
    return true;

  while (!found && fgets(line, sizeof line, file) != NULL)
    if (strncmp(line, "#error meta cannot write back", 29) == 0) {
      fputs(line, stderr);
      found = true;
    }

  fclose(file);
  return found;
}

/**
 * The shared object for one migration, compiled if this text has not been
 * compiled before. Answers false when it could not be, having said why.
 *
 * Each text gets a directory named by its hash, and the lowered file inside
 * keeps the migration's own name. That is not tidiness: `DBM_MIGRATION`
 * names the migration by `__FILE__`, which the C compiler expands, and it
 * expands it to the file it was given - so a lowered file called after its
 * hash recorded a migration under a name that changed with every edit.
 */
/** `mkdir -p`, for a scope that is more than one directory deep. */
static void makeDirectories(char *path) {

  for (char *at = path + 1; *at != '\0'; ++at)
    if (*at == '/') {
      *at = '\0';
      mkdir(path, 0755);
      *at = '/';
    }

  mkdir(path, 0755);
}

static bool build(const char *source, const char *name, const char *under,
                  const char *cache, char *object, size_t room) {

  char lowered[2048];
  char hashed[1024];
  char directory[1600];
  char log[2048];
  const char *base = strrchr(source, '/');
  const char *root = metaRoot();
  const char *include = includeDirectory();
  char meta[1024];
  char runtime[1024];
  char includeFlag[1100];
  char runtimeFlag[1100];
  bool ok;
  unsigned long long hash = hashOf(source, &ok);
  struct stat seen;

  base = base != NULL ? base + 1 : source;

  /* the name without `.c`, which the object and the log are called after */
  char stem[512];
  size_t stemLength = strlen(base) - 2;

  if (stemLength >= sizeof stem)
    stemLength = sizeof stem - 1;

  memcpy(stem, base, stemLength);
  stem[stemLength] = '\0';

  if (!ok) {
    dbmSay(stderr, TEXT`[ERROR] cannot read ${source}\n`);
    return false;
  }

  /**
   * Under migrations/<scope>/ inside the hash's directory, because the C
   * compiler expands __FILE__ to the path it was given and DBM_MIGRATION
   * reads the scope off it. The same text in two scopes is two objects.
   */
  const char *slash = strrchr(name, '/');
  char scope[512] = "";

  if (name[0] != '/' && slash != NULL) {
    memcpy(scope, name, (size_t)(slash - name));
    scope[slash - name] = '\0';
  }

  dbmWrite(hashed, sizeof hashed, TEXT`${cache}/${zeroed(hex(hash), 16)}`);
  dbmWrite(directory, sizeof directory,
           TEXT`${hashed}/${under}${scope[0] != '\0' ? "/" : ""}${scope}`);
  dbmWrite(object, room, TEXT`${directory}/${stem}.so`);

  if (stat(object, &seen) == 0)
    return true;

  dbmSay(stdout, TEXT`[INFO] Compiling ${base}\n`);

  makeDirectories(directory);
  dbmWrite(lowered, sizeof lowered, TEXT`${directory}/${base}`);
  dbmWrite(log, sizeof log, TEXT`${directory}/${stem}.log`);
  dbmWrite(meta, sizeof meta, TEXT`${root}/meta`);
  dbmWrite(runtime, sizeof runtime, TEXT`${root}/runtime/include`);
  dbmWrite(includeFlag, sizeof includeFlag, TEXT`-I${include}`);
  dbmWrite(runtimeFlag, sizeof runtimeFlag, TEXT`-I${runtime}`);

  char *lower[] = {meta, "-s", "-emit", lowered, "-I", (char *)include,
                   (char *)source, NULL};

  if (!run(lower, log) || refused(lowered)) {
    dbmSay(stderr, TEXT`[ERROR] meta could not lower ${base}:\n`);
    show(log);
    return false;
  }

  /* a partial object must never be mistaken for a finished one */
  char partial[2500];

  dbmWrite(partial, sizeof partial, TEXT`${object}.partial`);

  char *compile[] = {(char *)setting("CC", "cc"), "-std=gnu11", "-shared",
                     "-fPIC", "-g", "-Wall", includeFlag, runtimeFlag,
                     "-o", partial, lowered, NULL};

  if (!run(compile, log)) {
    dbmSay(stderr, TEXT`[ERROR] ${base} does not compile:\n`);
    show(log);
    return false;
  }

  if (rename(partial, object) != 0) {
    dbmSay(stderr, TEXT`[ERROR] cannot keep ${object}: ${strerror(errno)}\n`);
    return false;
  }

  return true;
}

/** The migrations directory, compiled where it has to be and opened. */
/**
 * One migration, compiled if this text has not been before, and opened. The
 * walker asks for it when the database says it has to run - not before.
 */
static bool compileAndOpen(const char *source, const char *name) {

  char object[2400];

  mkdir(".meta-migrate", 0755);

  if (!build(source, name, "migrations", ".meta-migrate", object,
             sizeof object))
    return false;

  /**
   * Lazily, and into the global table: a migration that calls a driver's
   * own method - `db->createSequence` - names a function the driver brings,
   * and the driver is opened before any migration is, from the
   * configuration. Its symbols are resolved when they are first called.
   */
  if (dlopen(object, RTLD_LAZY | RTLD_GLOBAL) == NULL) {
    dbmSay(stderr, TEXT`[ERROR] cannot open ${object}: ${dlerror()}\n`);
    return false;
  }

  return true;
}

/**
 * The migrations directory, by name only. Which of them are compiled is the
 * database's to decide: the walker reads what has run, and loads exactly the
 * ones it is about to run or undo. `check` compiles nothing at all.
 */
/**
 * One directory of migrations - migrations/ itself, or a scope under it - by
 * name only: the code files, and the SQL in its sqls/. `as` is what the names
 * are read from, `migrations[/scope]`, whatever the directory is called.
 */
static void listOne(const char *dir, const char *as) {

  char sqls[1100];
  DIR *listing = opendir(dir);

  for (struct dirent *entry = listing != NULL ? readdir(listing) : NULL;
       entry != NULL; entry = readdir(listing)) {

    size_t length = strlen(entry->d_name);
    char path[1100];
    char named[1100];

    if (length < 3 || strcmp(entry->d_name + length - 2, ".c") != 0)
      continue;

    dbmWrite(path, sizeof path, TEXT`${dir}/${entry->d_name}`);
    dbmWrite(named, sizeof named, TEXT`${as}/${entry->d_name}`);
    dbmRegisterLazily(named, path, compileAndOpen);
  }

  if (listing != NULL)
    closedir(listing);

  /* and the ones written as SQL, where `create --sql-file` puts them */
  dbmWrite(sqls, sizeof sqls, TEXT`${dir}/sqls`);
  listing = opendir(sqls);

  for (struct dirent *entry = listing != NULL ? readdir(listing) : NULL;
       entry != NULL; entry = readdir(listing)) {

    size_t length = strlen(entry->d_name);
    char path[1200];
    char named[1200];

    if (length < 8 || strcmp(entry->d_name + length - 7, "-up.sql") != 0)
      continue;

    dbmWrite(path, sizeof path, TEXT`${sqls}/${entry->d_name}`);
    dbmWrite(named, sizeof named, TEXT`${as}/sqls/${entry->d_name}`);
    dbmRegisterLazily(named, path, dbmLoadSqlFiles);
  }

  if (listing != NULL)
    closedir(listing);
}

/**
 * The migrations directory and every scope in it, by name only. Which of
 * them are compiled is the database's to decide: the walker reads what has
 * run, and loads exactly the ones it is about to run or undo. `check`
 * compiles nothing at all.
 */
static bool listAll(const char *dir) {

  DIR *listing = opendir(dir);

  if (listing == NULL) {
    dbmSay(stderr, TEXT`[ERROR] there is no ${dir} directory here\n`);
    return false;
  }

  listOne(dir, "migrations");

  for (struct dirent *entry = readdir(listing); entry != NULL;
       entry = readdir(listing)) {

    char path[1100];
    char as[1100];
    struct stat seen;

    if (entry->d_name[0] == '.' || strcmp(entry->d_name, "sqls") == 0)
      continue;

    dbmWrite(path, sizeof path, TEXT`${dir}/${entry->d_name}`);

    if (stat(path, &seen) != 0 || !S_ISDIR(seen.st_mode))
      continue;

    dbmWrite(as, sizeof as, TEXT`migrations/${entry->d_name}`);
    listOne(path, as);
  }

  closedir(listing);
  return true;
}

/** A file's bytes as the inside of a C string literal, a line per line. */
static void literal(FILE *out, const char *text) {

  fputs("\"", out);

  for (const unsigned char *at = (const unsigned char *)text; *at != 0; ++at) {

    switch (*at) {
    case '\\':
      fputs("\\\\", out);
      break;
    case '"':
      fputs("\\\"", out);
      break;
    case '\n':
      fputs("\\n\"\n    \"", out);
      break;
    case '\t':
      fputs("\\t", out);
      break;
    case '\r':
      fputs("\\r", out);
      break;
    default:

      if (*at < 0x20 || *at == 0x7f)
        dbmSay(out, TEXT`\\${zeroed(octal(*at), 3)}`);
      else
        fputc(*at, out);
    }
  }

  fputs("\"", out);
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

/**
 * `meta-migrate embed-sql <migrations> <out.c>`: the SQL migrations as C, so
 * a program that ships them is still one file. build-app.sh calls it.
 *
 * The output is plain C for the C compiler - string literals and one
 * constructor registering them - so it needs no lowering and has no runtime
 * dependency on the directory it was made from.
 */
/** The pairs in one sqls/ directory, named from `as` - `migrations[/scope]`. */
static bool embedOne(FILE *out, const char *dir, const char *as) {

  char sqls[1100];
  DIR *listing;
  bool ok = true;

  dbmWrite(sqls, sizeof sqls, TEXT`${dir}/sqls`);
  listing = opendir(sqls);

  for (struct dirent *entry = listing != NULL ? readdir(listing) : NULL;
       ok && entry != NULL; entry = readdir(listing)) {

    size_t length = strlen(entry->d_name);
    char up[1200];
    char down[1200];

    if (length < 8 || strcmp(entry->d_name + length - 7, "-up.sql") != 0)
      continue;

    dbmWrite(up, sizeof up, TEXT`${sqls}/${entry->d_name}`);
    dbmWrite(down, sizeof down, TEXT`${up}`);
    dbmWrite(down + strlen(up) - 7, sizeof down - (strlen(up) - 7),
             TEXT`-down.sql`);

    char *upText = slurp(up);
    char *downText = slurp(down);

    if (upText == NULL) {
      dbmSay(stderr, TEXT`[ERROR] cannot read ${up}\n`);
      ok = false;
    } else {

      dbmSay(out, TEXT`  dbmRegisterSql("${as}/sqls/${entry->d_name}",\n    `);
      literal(out, upText);
      fputs(",\n    ", out);

      if (downText != NULL)
        literal(out, downText);
      else
        fputs("0", out);

      fputs(");\n", out);
    }

    free(upText);
    free(downText);
  }

  if (listing != NULL)
    closedir(listing);

  return ok;
}

/**
 * `meta-migrate embed-sql <migrations> <out.c>`: the SQL migrations as C, so
 * a program that ships them is still one file. build-app.sh calls it.
 *
 * The output is plain C for the C compiler - string literals and one
 * constructor registering them - so it needs no lowering and has no runtime
 * dependency on the directory it was made from. Scopes come along, named the
 * way the launcher names them.
 */
static int embedSql(const char *dir, const char *into) {

  FILE *out = fopen(into, "w");
  DIR *listing = opendir(dir);
  bool ok;

  if (out == NULL) {
    perror(into);
    return 1;
  }

  fputs("/* written by meta-migrate embed-sql; do not edit */\n"
        "#include <db_migrate.h>\n\n"
        "__attribute__((constructor)) static void dbmEmbeddedSql(void) {\n",
        out);

  ok = embedOne(out, dir, "migrations");

  for (struct dirent *entry = listing != NULL ? readdir(listing) : NULL;
       ok && entry != NULL; entry = readdir(listing)) {

    char path[1100];
    char as[1100];
    struct stat seen;

    if (entry->d_name[0] == '.' || strcmp(entry->d_name, "sqls") == 0)
      continue;

    dbmWrite(path, sizeof path, TEXT`${dir}/${entry->d_name}`);

    if (stat(path, &seen) != 0 || !S_ISDIR(seen.st_mode))
      continue;

    dbmWrite(as, sizeof as, TEXT`migrations/${entry->d_name}`);
    ok = embedOne(out, path, as);
  }

  if (listing != NULL)
    closedir(listing);

  fputs("}\n", out);
  fclose(out);
  return ok ? 0 : 1;
}

/**
 * The project's plugins, compiled if they changed and opened - before
 * anything else, because one may be what reads the configuration. Opened
 * now, not lazily: what a plugin does is register, from its constructor.
 */
static bool openPlugins(void) {

  DIR *listing = opendir("plugins");
  struct dirent *entry;
  bool ok = true;

  if (listing == NULL)
    return true;

  mkdir(".meta-migrate", 0755);

  while (ok && (entry = readdir(listing)) != NULL) {

    size_t length = strlen(entry->d_name);
    char source[1024];
    char name[600];
    char object[2400];

    if (length < 3 || strcmp(entry->d_name + length - 2, ".c") != 0)
      continue;

    dbmWrite(source, sizeof source, TEXT`plugins/${entry->d_name}`);
    dbmWrite(name, sizeof name, TEXT`/${entry->d_name}`);

    ok = build(source, name, "plugins", ".meta-migrate", object, sizeof object);

    if (ok && dlopen(object, RTLD_NOW | RTLD_GLOBAL) == NULL) {
      dbmSay(stderr, TEXT`[ERROR] cannot open ${object}: ${dlerror()}\n`);
      ok = false;
    }
  }

  closedir(listing);
  return ok;
}

int main(int argc, char **argv) {

  if (argc == 4 && strcmp(argv[1], "embed-sql") == 0)
    return embedSql(argv[2], argv[3]);

  const char *dir = "migrations";
  const char *command = NULL;
  bool asking = false;

  /**
   * The command the way dbmCli reads it: the first word that is not an
   * option or an option's value. Only the commands that run migrations need
   * the directory - `--help`, `create`, `db:create` and no command at all
   * must work where there is none yet.
   */
  for (int i = 1; i < argc; ++i) {

    const char *word = argv[i];

    if ((word in {"-m", "--migrations-dir"}) && i + 1 < argc)
      dir = argv[++i];
    else if ((word in {"-e", "--env", "--config", "-c", "--count", "-t",
                       "--table", "--migration-table", "-s", "--state",
                       "--state-table", "--lock-timeout", "--lock-interval",
                       "--template", "--log-level"}) && i + 1 < argc)
      ++i;
    else if (word in {"-h", "--help", "-?", "-i", "--version"})
      asking = true;
    else if (word[0] != '-' && command == NULL)
      command = word;
  }

  dbmDriverDirectory = driverDirectory();

  if (!asking && !openPlugins())
    return 1;

  bool needsMigrations =
      !asking && command != NULL && strncmp(command, "create", 6) != 0 &&
      strncmp(command, "db:", 3) != 0;

  if (needsMigrations && !listAll(dir))
    return 1;

  return dbmCli(argc, argv);
}
