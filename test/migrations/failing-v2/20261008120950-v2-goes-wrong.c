/**
 * A v2 migration that fails halfway. There is no transaction around it - its
 * rollback is its own record run backwards - so what it built before the
 * failure has to be gone afterwards on every database, MySQL included.
 */
#include <db_migrate.h>

static int migrate(schema_t *db) {

  db->createTable("v2_half_done", {id: {type: "int", primaryKey: true}});
  db->addColumn("v2_half_done", "extra", {type: "int"});

  /* no such table in the schema the v2 migrations built: learning fails */
  return db->addForeignKey("v2_half_done", "nowhere", "v2_nowhere_fk",
                           {extra: "id"}, {});
}

DBM_MIGRATION_V2(migrate)
