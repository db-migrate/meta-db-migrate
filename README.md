# db-migrate for meta

db-migrate rewritten in meta. A program written in meta ships with its
migrations compiled in, as one binary. During development, a launcher compiles
each migration on its own, so `down`, edit, `up` takes a fraction of a second.

The migrations table has the same columns and the same names as node
db-migrate's (`/20261008120000-add-pets`). A database migrated by one of them
can be carried on by the other.

## A migration

```c
#include <db_migrate.h>

static int up(migrator_t *db) {

  db->createTable("pets", {
    id:       {type: "int", primaryKey: true, autoIncrement: true},
    owner_id: {type: "int", notNull: true,
               foreignKey: {table: "owners", mapping: "id",
                            rules: {onDelete: "CASCADE"}}},
    name:     {type: "string", length: 48, notNull: true},
    traits:   "jsonb",
  });

  db->addIndex("pets", "pets_name_idx", ["name"]);

  json_t pets = db->all(SQL`select id, name from pets`);
  defer pets.release();

  for (int i = 0; i < pets.count(); ++i)
    db->run(SQL`update pets set slug = ${slugify(pets[i].name)}
                where id = ${(long)pets[i].id}`);

  return 0;
}

static int down(migrator_t *db) {
  return db->dropTable("pets");
}

DBM_MIGRATION(up, down)
```

- **The specs are documents, not structs.** What a column can say belongs to
  the driver, so `{...}` and `[...]` passed where a `json_t` is expected
  become JSON. A literal made only of constants is built once. One that
  contains variables is built per call and freed afterwards.
- **Errors stick.** The first failing call records its reason, and every later
  call does nothing. A migration therefore reads as a plain list of steps,
  and the walker rolls back the whole migration together with its record.
  A migration fails on its own with a template:
  ``return db->fail(TEXT`no owner called ${name}`);``
- **Data migrations are code.** `db->all` returns rows as JSON, and `db->run`
  takes the `SQL` literal, whose values never end up in the SQL text.
- **Driver-specific features need their header.** With
  `#include <db_migrate/pg.h>`, `db->createSequence(...)` becomes available.
  Without the header the compiler does not know the method. Run against a
  different database, the call fails with a clear reason.

A v2 migration (node's `_meta: {version: 2}`) has no `down`. It is undone from
what it learned while running up, recorded in node's format, so node can undo it
and vice versa:

```c
static int migrate(schema_t *db) {
  db->createTable("owners", {id: {type: "int", primaryKey: true}, name: "string"});
  return db->addColumn("owners", "email", {type: "string", length: 120});
}

DBM_MIGRATION_V2(migrate)
```

Like in node, it runs outside a transaction. Every step is recorded in
`migrations_state` before the next one starts, and a failure is undone step by
step.

Migrations can also be plain SQL, as node's `create --sql-file` creates them:
`migrations/sqls/<stamp>-<name>-up.sql` and `-down.sql`. No code file is
needed next to them. Text containing only comments (the placeholder) does
nothing.

## Commands

```
up [name]          run what has not run, up to and including name
down [name]        undo the last one, or everything after name
sync name          up or down, whichever reaches name
reset              undo everything
check              list what would run
fix                rebuild node's state from the v2 migrations that ran
create name        a new migration (--sql-file, --v2-file, --template NAME)
db:create name     create a database, if it is not there yet
db:drop name       drop a database, if it is there
command:scope      the same in migrations/<scope>/ - up:billing, create:billing
```

Options as in node: `-e/--env`, `--config`, `-m/--migrations-dir`,
`-c/--count`, `-t/--table`, `-s/--state-table`, `--lock-timeout`,
`--lock-interval`, `--backup-state`, `--dry-run`, `--check`, `-v/--verbose`,
`--non-transactional`, `--sql-file`, `--v2-file`, `--template`, `-h`, `-i`. A
destination can be abbreviated: `up 20261008` runs everything up to that day.

While it migrates, a process holds node's migration lock: the
`__dbmigrate_state__` row in `migrations_state`, renewed every third of
`--lock-timeout`. A second process, node or this one, waits. It only takes the
lock over after the row has stayed unchanged for the whole timeout (60 s by
default), measured on its own monotonic clock. The state lives on a
connection of its own, so it survives a migration's rollback.

Scopes are subfolders of `migrations/`. Their migrations are recorded as
`billing/<name>` and those at the top level as `/<name>`, exactly as in node.
Each command only sees its own scope.

## In a program

```c
int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "migrate") == 0)
    return dbmCli(argc - 1, argv + 1);   /* every command above */
  ...
}
```

`./build-app.sh <app> <output> [driver or plugin...]` lowers the app, its
`plugins/` and its migrations, including scopes, embeds the SQL migrations as
strings, and links
`libdbmigrate.a` with `--whole-archive`, together with only the drivers you
name (default: `pg`). A program on SQLite does not
need libpq installed. Drivers and migrations register
themselves through constructors, and nothing else references them, so a
linker that only pulls in referenced objects would drop them.

Configuration works as in node db-migrate:

- `database.json` with environments (`-e`, `NODE_ENV`, `defaultEnv`, `dev`)
- `database.yml` (or `.yaml`) when there is no `database.json`, or any
  `--config` file whose ending a plugin reads
- `{"ENV": "NAME"}` for values taken from the environment
- `DATABASE_URL` when there is no file
- `tunnel: {...}` in a connection, to reach it through ssh (see Plugins)

## During development

```sh
build/meta-migrate up
build/meta-migrate down
# edit the migration
build/meta-migrate up        # compiles only the edited file, ~0.15 s
```

- The launcher reads the migration files by name only and compiles exactly
  those the database says need to run. `check` compiles nothing.
- Each one is kept as a `.so` under `.meta-migrate/<hash>/`.
- It loads the driver the configuration names (`libdbmigrate-pg.so`) only at
  that point, so libpq is only needed where PostgreSQL is used.
- Apart from that it runs the same `dbmCli` as the shipped program.

## Plugins

What node's plugins were for is either built in or a plugin compiled into the
program. A plugin registers from its constructor, the same way a driver does,
so nothing else has to name it.

| node | here |
|---|---|
| plugin-yaml (`init:config:overwrite:require`) | shipped plugin `yaml` (libyaml), registered for `.yml`/`.yaml` |
| plugin-tunnel-ssh (`connection:tunnel:ssh`) | built in: `tunnel` in a connection |
| `connection:tunnel:<type>` | `dbmRegisterTunnel("<type>", open, close)` |
| `init:config:overwrite:require` | `dbmRegisterConfigLoader(".ext", load)` |
| `create:template` | `dbmRegisterTemplate("name", write)`, used by `create x --template name` |

See `include/db_migrate_plugin.h`.

- **YAML** reads like js-yaml's `safeLoad`: unquoted `5432` is a number,
  quoted text stays text, and anchors, aliases and `<<` merges work. YAML is
  a plugin rather than part of the core, so only a program that reads YAML
  needs libyaml: `./build-app.sh . ./app pg yaml`. The launcher loads
  `libdbmigrate-yaml.so` the first time it meets a `.yml`.
- **The ssh tunnel** is the system's `ssh -N -L`, so `~/.ssh/config`, the
  agent and `known_hosts` apply. node's tunnel-ssh checked no host key at
  all. The keys are the ones plugin-tunnel-ssh took:

  ```yaml
  prod:
    driver: pg
    host: db.internal          # as the bastion sees it
    database: shop
    tunnel:
      host: bastion.example
      username: deploy
      privateKeyPath: /home/deploy/.ssh/id_ed25519
      # port (22), privateKey, password, passphrase, localHost, localPort,
      # keepaliveInterval, readyTimeout (20000), options: {Key: value} -> -o
  ```

  Passwords and passphrases reach ssh through `SSH_ASKPASS` and the
  environment, never through the command line. Without one, ssh runs in batch
  mode and fails instead of waiting at a prompt. `DBM_SSH` names a different
  ssh binary.
- **A project's own plugins** go in `plugins/*.c`. The launcher compiles and
  opens them before it reads anything else, and `build-app.sh` compiles them
  in.

## Drivers

A driver is a `driver_t` that `dbmDriverNew` has filled with the generic
versions, the counterpart of `db-migrate-base`. The driver overrides only what
its database does differently. To call the generic version (`_super`), it
calls the generic function by name. The generic functions call each other
only through `self`, so overriding `columnDef` changes what `createTable`
writes. See `include/db_migrate_driver.h`.

| Driver | database.json | Special features (header) | Notes |
|---|---|---|---|
| `pg` | `host`, `port`, `user`, `password`, `database`, `sslmode` | sequences, enums (`db_migrate/pg.h`) | DDL is transactional |
| `cockroachdb` | as pg | additionally `changePrimaryKey`, `uuid` keys, computed columns, row TTL (`db_migrate/cockroachdb.h`) | extends pg. A column added to an existing table cannot be used until its transaction commits, so adding a column and filling it are two migrations |
| `mysql` | `host`, `port`, `user`, `password`, `database`, `socketPath` | table options (`engine`, `charset`), `unsigned`, `after`, `comment` | MySQL commits before every DDL statement. A failed migration keeps the DDL it already ran, only its record is rolled back |
| `sqlite3` | `filename` | – | foreign keys are written into the column (`REFERENCES`). Changing a column in place is not possible and is refused with a reason |

`cockroachdb` extends `pg` the way db-migrate-cockroachdb extends db-migrate-pg:
`dbmPgConnect` returns a fully configured pg driver, and Cockroach replaces
individual slots in it (`src/drivers/pg_driver.h`).

## Building and testing

```sh
./build.sh                    # libdbmigrate.a, a driver library each, meta-migrate
test/run.sh                   # against all four databases
test/plugins.sh               # yaml, plugins/, the ssh tunnel (own sshd)
test/compat.sh                # node db-migrate and this on the same database
```

`test/run.sh [driver...]` runs the same cycle against every database: up,
down, reset, a migration that fails partway, a dry run, and the launcher.
`test/migrations/common` is shared, and each driver has its own folder. The
databases (`test/database.json`) are started by the commands at the top of
`test/run.sh`. SQLite only needs a file.

## Status

- PostgreSQL, CockroachDB, MySQL, SQLite: createTable, dropTable,
  renameTable, add/remove/rename/changeColumn, indexes, foreign keys, insert,
  run/all/runSql, plus each database's own features (see table)
- up/down/sync with destinations, reset, check, create (code, SQL, v2,
  templates), scopes, db:create/db:drop, node's options, development launcher
- v2 migrations, node's state and lock, `fix`, in node's format
- plugins: YAML, ssh tunnel, config loaders, tunnels and templates of your own
- Missing: seeds (not working in node either at the moment),
  `ignore-on-init`, MongoDB
