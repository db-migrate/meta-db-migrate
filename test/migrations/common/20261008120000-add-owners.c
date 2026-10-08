#include <db_migrate.h>

static int up(migrator_t *db) {
  return db->createTable("owners", {
    id: {type: "int", primaryKey: true, autoIncrement: true},
    name: {type: "string", length: 64, notNull: true},
    created_at: {type: "datetime", notNull: true,
                 defaultValue: {special: "CURRENT_TIMESTAMP"}},
  });
}

static int down(migrator_t *db) {
  return db->dropTable("owners");
}

DBM_MIGRATION(up, down)
