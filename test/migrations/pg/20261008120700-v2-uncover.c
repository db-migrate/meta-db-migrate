/** The index the migration before made, removed - and learned with its options. */
#include <db_migrate.h>

static int migrate(schema_t *db) {
  return db->removeIndex("v2_kennels", "v2_kennels_cover");
}

DBM_MIGRATION_V2(migrate)
