/**
 * A primary key changed in place, in a migration of its own: CockroachDB
 * rebuilds the table behind it and does not take that in a transaction that
 * also changed the table.
 */
#include <db_migrate.h>
#include <db_migrate/cockroachdb.h>

static int up(migrator_t *db) {
  return db->changePrimaryKey("events", ["kind", "id"]);
}

static int down(migrator_t *db) {
  return db->changePrimaryKey("events", ["id"]);
}

DBM_MIGRATION(up, down)
