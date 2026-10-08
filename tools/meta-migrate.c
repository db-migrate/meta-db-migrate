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
 *
 * Where meta, its runtime headers, db-migrate's headers and the drivers are
 * is baked in when this is built, and can be moved with META_ROOT,
 * DBM_INCLUDE and DBM_DRIVERS.
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
static bool build(const char *source, const char *cache, char *object,
                  size_t room) {

  char lowered[2048];
  char directory[1024];
  char log[2048];
  const char *base = strrchr(source, '/');
  const char *root = setting("META_ROOT", DBM_META_ROOT);
  const char *include = setting("DBM_INCLUDE", DBM_INCLUDE_DIR);
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

  dbmWrite(directory, sizeof directory,
           TEXT`${cache}/${zeroed(hex(hash), 16)}`);
  dbmWrite(object, room, TEXT`${directory}/${stem}.so`);

  if (stat(object, &seen) == 0)
    return true;

  dbmSay(stdout, TEXT`[INFO] Compiling ${base}\n`);

  mkdir(directory, 0755);
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
static bool compileAndOpen(const char *source) {

  char object[2400];

  mkdir(".meta-migrate", 0755);

  if (!build(source, ".meta-migrate", object, sizeof object))
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
static bool listAll(const char *dir) {

  DIR *listing = opendir(dir);

  if (listing == NULL) {
    dbmSay(stderr, TEXT`[ERROR] there is no ${dir} directory here\n`);
    return false;
  }

  for (struct dirent *entry = readdir(listing); entry != NULL;
       entry = readdir(listing)) {

    size_t length = strlen(entry->d_name);
    char path[1024];

    if (length < 3 || strcmp(entry->d_name + length - 2, ".c") != 0)
      continue;

    dbmWrite(path, sizeof path, TEXT`${dir}/${entry->d_name}`);
    dbmRegisterLazily(path, compileAndOpen);
  }

  closedir(listing);

  /* and the ones written as SQL, where `create --sql-file` puts them */
  char sqls[1024];

  dbmWrite(sqls, sizeof sqls, TEXT`${dir}/sqls`);
  listing = opendir(sqls);

  if (listing == NULL)
    return true;

  for (struct dirent *entry = readdir(listing); entry != NULL;
       entry = readdir(listing)) {

    size_t length = strlen(entry->d_name);
    char path[1100];

    if (length < 8 || strcmp(entry->d_name + length - 7, "-up.sql") != 0)
      continue;

    dbmWrite(path, sizeof path, TEXT`${sqls}/${entry->d_name}`);
    dbmRegisterLazily(path, dbmLoadSqlFiles);
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
static int embedSql(const char *dir, const char *into) {

  char sqls[1024];
  FILE *out = fopen(into, "w");

  if (out == NULL) {
    perror(into);
    return 1;
  }

  dbmWrite(sqls, sizeof sqls, TEXT`${dir}/sqls`);

  fputs("/* written by meta-migrate embed-sql; do not edit */\n"
        "#include <db_migrate.h>\n\n"
        "__attribute__((constructor)) static void dbmEmbeddedSql(void) {\n",
        out);

  DIR *listing = opendir(sqls);

  for (struct dirent *entry = listing != NULL ? readdir(listing) : NULL;
       entry != NULL; entry = readdir(listing)) {

    size_t length = strlen(entry->d_name);
    char up[1100];
    char down[1100];

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
      fclose(out);
      closedir(listing);
      free(downText);
      return 1;
    }

    dbmSay(out, TEXT`  dbmRegisterSql("${entry->d_name}",\n    `);
    literal(out, upText);
    fputs(",\n    ", out);

    if (downText != NULL)
      literal(out, downText);
    else
      fputs("0", out);

    fputs(");\n", out);
    free(upText);
    free(downText);
  }

  if (listing != NULL)
    closedir(listing);

  fputs("}\n", out);
  fclose(out);
  return 0;
}

int main(int argc, char **argv) {

  if (argc == 4 && strcmp(argv[1], "embed-sql") == 0)
    return embedSql(argv[2], argv[3]);


  const char *dir = "migrations";
  bool creating = false;

  for (int i = 1; i < argc; ++i) {

    if ((argv[i] in {"-m", "--migrations-dir"}) && i + 1 < argc)
      dir = argv[i + 1];

    if (strcmp(argv[i], "create") == 0)
      creating = true;
  }

  dbmDriverDirectory = setting("DBM_DRIVERS", DBM_DRIVER_DIR);

  if (!creating && !listAll(dir))
    return 1;

  return dbmCli(argc, argv);
}
