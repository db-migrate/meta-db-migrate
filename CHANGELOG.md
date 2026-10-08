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
