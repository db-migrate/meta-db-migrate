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

## In a program

```c
int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "migrate") == 0)
    return dbmCli(argc - 1, argv + 1);   /* up, down [-c n], reset, check, create */
  ...
}
```

`./build-app.sh <app> <output> [driver...]` lowers the app and
`migrations/*.c` and links `libdbmigrate.a` with `--whole-archive`, together
with only the drivers you name (default: `pg`). A program on SQLite does not
need libpq installed. Drivers and migrations register
themselves through constructors, and nothing else references them, so a
linker that only pulls in referenced objects would drop them.

Configuration works as in node db-migrate:

- `database.json` with environments (`-e`, `NODE_ENV`, `defaultEnv`, `dev`)
- `{"ENV": "NAME"}` for values taken from the environment
- `DATABASE_URL` when there is no file

## During development

```sh
build/meta-migrate up
build/meta-migrate down
# edit the migration
build/meta-migrate up        # compiles only the edited file, ~0.15 s
```

- The launcher keeps each migration as a `.so` under `.meta-migrate/<hash>/`.
- It loads the driver the configuration names (`libdbmigrate-pg.so`) only at
  that point, so libpq is only needed where PostgreSQL is used.
- Apart from that it runs the same `dbmCli` as the shipped program.

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
./build.sh                    # libdbmigrate.a, libdbmigrate-pg.so, meta-migrate
test/run.sh                   # against PostgreSQL on 127.0.0.1:55432
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
- up, down, reset, check, create, dry run, development launcher
- Missing: v2 / state travel, seeds, plugins, MongoDB
