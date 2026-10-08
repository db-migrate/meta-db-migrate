#include <db_migrate.h>
#include <db_migrate/cockroachdb.h>

static int up(migrator_t *db) {
  return db->addColumn("pets", "size", {type: "enum", enumName: "pet_size",
                                        defaultValue: "small"});
}

static int down(migrator_t *db) {
  return db->removeColumn("pets", "size");
}

DBM_MIGRATION(up, down)
