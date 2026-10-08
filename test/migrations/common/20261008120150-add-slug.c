/**
 * The column on its own, before the migration that fills it.
 *
 * On CockroachDB a column added to a table that existed before the
 * transaction cannot be written to until the transaction commits - only a
 * table created in the same transaction can - so adding it and filling it are
 * two migrations. Everywhere else that is merely tidy.
 */
#include <db_migrate.h>

static int up(migrator_t *db) {
  return db->addColumn("pets", "slug", {type: "string", length: 64});
}

static int down(migrator_t *db) {
  return db->removeColumn("pets", "slug");
}

DBM_MIGRATION(up, down)
