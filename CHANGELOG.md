## 0.7.0 (2026-11-12)

What node db-migrate 1.8.2, db-migrate-cockroachdb 5.10.0,
db-migrate-base 2.5.1 and db-migrate-mysql 3.2.1 changed, followed, and
covering indexes besides. The ABI is 2.

### Features

* **index:** `db->addIndexWith(table, name, columns, options)`, node's
  addIndex with an options object, on v1 and v2 migrations and under
  `db->adopt()`: `unique`, `where` for a partial index (PostgreSQL,
  CockroachDB, SQLite), `inverted` (CockroachDB) and `storing`, the
  columns a covering index keeps - CockroachDB's `STORING`, PostgreSQL's
  `INCLUDE`. What a database cannot say is refused with a reason, and so
  is an option nobody knows
* **v2:** an index's options are learned as node 1.8.2 keeps them, `u`,
  `v` and `w`, and its stored columns as a list under `s`, so undoing a
  `removeIndex` or a `dropTable` creates it again with all of them, from
  the state either tool wrote

### Bug Fixes

* **base:** a type nobody recognised keeps its quoted parts as they are,
  `enum('calm', 'Wild')` rather than `ENUM(CALM, WILD)`
* **mysql:** a table's character set is written before its collation,
  which older servers otherwise reset to the character set's default

### Breaking changes

* **driver:** `driver_t`'s `addIndex` takes the index's options as a
  document instead of `bool unique`, and `partialIndexes`,
  `invertedIndexes` and `indexStoring` say which of them a database has;
  `DBM_ABI` is 2, so an object built against 0.6 headers fails to link
  (`dbm_needs_abi_1`) instead of calling a driver the wrong way
* **driver:** `driver_t` ends in sixteen reserved slots. A driver puts
  `driver_t` first in a struct of its own, so any field added to it moved
  the driver's fields; what is added later takes a reserved slot instead,
  and the ABI stays 2

## 0.6.2 (2026-10-30)

The same db-migrate, written for meta's `json_t` as it is since `90db752`;
the ABI stays 1.

### Bug Fixes

* **walker:** the names read before a failure are released too, and those
  replaced are released before they are
* **dml:** the default options of an instruction are released after it
* **seed:** the document saying an insert has no values is released
* **launcher:** the rc document is released when main returns
* **ssh:** a tunnel that gets no memory fails like the other paths

### Build

* **meta:** built and tested with meta `7fda349` (`wxone/meta:7fda349`)
  rather than `51d441c`: the documents are `json_t` throughout, read by key
  and subscript and written with `set` and `&`, where the type's own fields
  are `_node` and `_refused` since `90db752`. A program on that meta that
  links `libdbmigrate-core.a` and its driver links these, which 0.6.1, built
  before, did not match
* meta from `b83c159` on hides its runtime's names in the object that links
  it, and the launcher's drivers, shared objects reaching into the launcher,
  do not link against an installed meta built that way; not yet followed,
  see README

## 0.6.1 (2026-10-11)

What node db-migrate 1.8.1 fixed, followed; the ABI stays 1.

### Bug Fixes

* **work:** a job keeps its scope, `sc` as node 1.8.1 writes it, and runs
  in it; `down` of a scope reverts the jobs of that scope only, `down` of
  the top level no longer stops at the job of a scope sharing its database;
  `work:<scope>` runs the jobs of a scope with a database of its own, and
  `work:all` is refused
* **state:** `fix --backup-state` keeps the jobs, seeds, purges, backups and
  the records of dml migrations, all but the schema
* **dml:** reverting a soft delete takes its mark out of the flag as a
  whole, not out of `#10` when it is `#1`
* **build:** builds with meta f4e60f8, whose ownership pass read three
  correct things as a use after free; CI and the release take meta from
  `wxone/meta:f4e60f8` rather than `latest`, which already releases
  documents on its own and is not yet followed here

## 0.6.0 (2026-10-10)

What node db-migrate added from 1.1.0 to 1.8.0, followed.

### Features

* **v2:** `db->adopt()` declares objects made outside v2 migrations;
  dropping one the schema does not know is refused with node's words, or
  done with `{ irreversible: true }`, which down refuses and a failure does
  not roll back
* **scope:** `all` runs the top level and every scope, nested ones too; a
  scope's `config.json` switches the database or schema, or connects on its
  own with its own lock and state; PostgreSQL takes `schema` as search_path
* **config:** `{"ENV": "X", "default": ...}` at any depth, `defaultEnv`
  too; rc files - `.db-migraterc` from here upwards, `~/.db-migraterc`,
  `/etc`, `$config`, `db-migrate_<key>` variables - JSON or INI;
  `--ignore-completed-migrations` does what it does in node
* **v1:** `DBM_MIGRATION_NO_TRANSACTION` and `DBM_MIGRATION_WITH(up, down,
  {transactions: false})` run a migration without a transaction
* **seed:** static seeds in `seeds/*.c`: `seed [name]`, `seed down [name]`,
  `seed reset`, their rows flagged and remembered in `__dbmigrate_seeds__`
* **dml:** data migrations, `DBM_MIGRATION_DML`: insert, update, delete in
  copy or soft mode, purge, runSql with its revert, dropBackups - each step
  recorded with what reverts it, backups, batches each in a transaction,
  continued after an interruption, rolled back after a failure
* **work:** background migrations, `{background: true}`, registered as jobs
  by up and run by `migrate work` (`--parallel`, `--pause`, `--batch`,
  `--interval`, `--job-timeout`, `--watch`), paused while migrations run,
  reverted first by down; `dbmWork` and `dbmWorkStop` for a program
* **release:** releases, `db->deprecateTable`, `db->deprecateColumn`,
  `db->dropDeprecated`; renamed with the next release, dropped after
  `releases` of them; soft deleted rows purged and backups dropped when due;
  `fix` learns the releases again
* **status:** `migrate status`, `dbmStatus`: pending, release, lock, jobs,
  and what is deprecated or due
* **abi:** `DBM_ABI` - a program built against other headers fails to link
  instead of misreading a struct; the SBOM names it as `db-migrate:abi`,
  and meta's runtime ABI as `meta:runtime-abi`

### Bug Fixes

* **state:** the lock's heartbeat and the migration no longer use the state's
  connection at the same time - with a short `--lock-interval` PostgreSQL
  said "another command is already in progress" and MySQL's client crashed
* **v2:** `changeColumn` is undone to the column exactly as it was

Where node has a bug the data or the state would feel, this does what node
meant: a soft delete's mark `#1` is not found in `#10`; a flag MySQL would cut
off is refused; a background job recovered by rolling back keeps its record,
one recorded already is not run twice, one of a scope runs in its scope;
`work --dry-run` works.

### ⚠ Requirements

* meta `9bcdd6b` or newer: `json_t` documents are mutable, and the runtime
  has its ABI number

## 0.5.0 (2026-10-09)

### Build

* **build:** meta is asked what meta knows - where its runtime is
  (`meta -print-config`) and which libraries a source needs through meta's
  headers (`meta -print-flags`) - instead of three tables kept by hand
* **deps:** a `.pc` file for every library `tools/deps.sh` builds;
  `pkg-config --static` works out what they pull in, so `build-app.sh
  --static` needs `pkg-config` on the build host
* **sbom:** meta's version comes from `meta --version`

### ⚠ Requirements

* meta `ed340ea` or newer

## 0.4.0 (2026-10-09)

### Features

* **sql:** migrations in one SQL file with `-- up` and `-- down` sections, as
  db-migrate-plugin-sql writes them for node db-migrate 1.0 - read by the
  launcher, embedded by build-app.sh, made by `create --sql`, refused with
  the plugin's words when malformed. A project moves between node with the
  plugin and this with the same files

## 0.3.0 (2026-10-09)

What node db-migrate changed up to 1.0.0-beta.38 and since, followed.

### Features

* **v2:** a run that dies halfway is resumed by the next one, as node does
  it: the lock row says which migration runs which way, and for every step
  whether it started, was learned and was done, with the file's hash. By
  default the steps that ran are skipped; `DBM_MIGRATION_V2_RECOVERY(migrate,
  "rollback")` undoes them and runs it again; a run that was rolling back is
  rolled back further; a changed file is not skipped blind. Works with node:
  either resumes what the other left
* **log:** a failed migration says which one, at which step, the statement
  with a marker at the position the database names, and the driver's
  diagnostic fields - pg's code, detail, hint and the rest, mysql's errno and
  SQL state, SQLite's code
* **cli:** `seed`, `undo-seed` and `reset-seed` say that seeders are not
  supported, as node 1.0 does

### Bug Fixes

* **v2:** a failure is rolled back by exactly the steps that reached the
  database, the failed one included if its table or column was made before a
  foreign key failed - every undo step carries its step number (`n`)
* **v2:** removing a second column, index or foreign key from one table in a
  migration no longer forgets the first, so its rollback can put it back
* **state:** writing the lock row keeps every field in it, node's included,
  instead of only the ones this knew
* **sqlite3:** a busy timeout (`busyTimeout`, 10000 ms), so the migrations'
  connection and the state's do not fail with "database is locked" when they
  write at once

## 0.2.0 (2026-10-09)

### Features

* **api:** `dbmMigrateUp`, migrating from inside a program: the connection,
  node's state on a second one, the lock with its heartbeat, up, and
  everything closed again - what `up` does, without a command line
* **api:** `dbmSetLogger`, the lines that went to stdout and stderr into the
  program's own log; `dbmLastError`, the last error said
* **build:** `libdbmigrate-core.a`, the core without meta's runtime, for a
  program that links `libmeta_runtime.a` itself - an nginx module
* **sbom:** in the shape of wx1-keyagent's, so one nests in the other:
  `pkg:generic` purls with their download URL, scopes, glibc as excluded
  with the version the release asks for, the shipped files by their hashes,
  the git commit

## 0.1.0 (2026-10-08)

### Features

* **release:** meta comes with it, at `/opt/meta` - the compiler, its runtime
  headers and `libmeta_runtime.a` - so the launcher and `build-app.sh` need
  nothing beside the release but a C compiler
* **release:** the launcher's drivers and plugins carry libpq, OpenSSL,
  SQLite and libyaml inside them (`build.sh` with `DBM_STATIC_DEPS`), so it
  runs on a host that has none of them; mysql still needs libmysqlclient
* **deps:** SQLite 3.53.4 built from its amalgamation, position independent
* **sbom:** meta as a component of its own; meta and its runtime carry an
  annotation that they are meant to become open source, next to the
  proprietary license that applies today

### Bug Fixes

* **plugins:** a shipped plugin that is there but cannot be loaded says why,
  instead of claiming nothing reads the configuration file

### CI

* `actions/checkout` and `actions/setup-node` v7, on Node 24

## 0.0.1 (2026-10-08)

The first release: db-migrate rewritten in meta. Programs ship with their
migrations compiled in, as one binary.

### Features

* **migrations** as meta code (`DBM_MIGRATION(up, down)`), as v2 migrations
  undone from what they learned (`DBM_MIGRATION_V2`), and as plain SQL files
* **drivers:** PostgreSQL, CockroachDB, MySQL, SQLite, each with the generic
  SQL of db-migrate-base and its own differences
* **commands and options as in node db-migrate:** up, down, sync, reset,
  check, create (code, `--sql-file`, `--v2-file`, `--template`), fix,
  db:create, db:drop, scopes, `--dry-run`, `--ignore-on-init`, `--log-level`
  and the rest
* **compatible with node db-migrate's database state:** the migrations table,
  `migrations_state`, the migration lock and v2's learned state - a project
  moves from one to the other and back
* **plugins:** `database.yml`, the ssh tunnel, and config loaders, tunnels
  and templates of a project's own (`plugins/`)
* **development launcher** `meta-migrate`: compiles only the migrations the
  database says have to run, an edit in about 0.15 s
* **`build-app.sh --static`:** programs that need glibc and nothing else
* **releases** with the static libraries, `THIRD_PARTY_NOTICES` and a
  CycloneDX SBOM; built on Ubuntu 24.04 against glibc 2.39
