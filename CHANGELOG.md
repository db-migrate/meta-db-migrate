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
