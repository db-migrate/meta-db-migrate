/** The indexes the migration before made, removed - and learned with their options. */
#include <db_migrate.h>

static int migrate(schema_t *db) {
  db->removeIndex("v2_kennels", "v2_kennels_traits");
  return db->removeIndex("v2_kennels", "v2_kennels_cover");
}

DBM_MIGRATION_V2(migrate)
