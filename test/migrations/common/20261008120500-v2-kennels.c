/**
 * A v2 migration: it says only what it builds, and `down` is learned - kept
 * in node's state table and run backwards, in node's format.
 */
#include <db_migrate.h>

static int migrate(schema_t *db) {

  db->createTable("v2_kennels", {
    id: {type: "int", primaryKey: true, autoIncrement: true},
    name: {type: "string", length: 32},
  });

  db->addIndex("v2_kennels", "v2_kennels_name_idx", ["name"]);
  db->addColumn("v2_kennels", "size", {type: "int"});

  return db->renameColumn("v2_kennels", "size", "capacity");
}

DBM_MIGRATION_V2(migrate)
