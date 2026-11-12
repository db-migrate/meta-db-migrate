/**
 * A covering, partial, unique index from a v2 migration, which the next one
 * removes: undoing that creates it again with everything it said. The stored
 * columns are PostgreSQL's INCLUDE.
 */
#include <db_migrate.h>

static int migrate(schema_t *db) {
  return db->addIndexWith("v2_kennels", "v2_kennels_cover", ["name"],
                          {unique: true, where: "capacity > 0",
                           storing: ["capacity"]});
}

DBM_MIGRATION_V2(migrate)
