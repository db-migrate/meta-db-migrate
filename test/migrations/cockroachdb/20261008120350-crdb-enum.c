/**
 * An enum type on its own: the column that uses it is the next migration, so
 * that dropping that column has been committed before this drops the type -
 * on CockroachDB a column dropped from an existing table holds on to its type
 * until then.
 */
#include <db_migrate.h>
#include <db_migrate/cockroachdb.h>

static int up(migrator_t *db) {
  return db->createEnum("pet_size", ["small", "large"]);
}

static int down(migrator_t *db) {
  return db->dropEnum("pet_size");
}

DBM_MIGRATION(up, down)
