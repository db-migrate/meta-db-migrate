/**
 * A migration in a scope: migrations/billing/, run by `up:billing` and
 * recorded as `billing/20261008121000-invoices`, the way node does it.
 */
#include <db_migrate.h>

static int up(migrator_t *db) {
  return db->createTable("invoices", {
    id: {type: "int", primaryKey: true, autoIncrement: true},
    total: {type: "decimal", precision: 10, scale: 2},
  });
}

static int down(migrator_t *db) {
  return db->dropTable("invoices");
}

DBM_MIGRATION(up, down)
