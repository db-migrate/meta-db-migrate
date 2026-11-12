/**
 * A covering, partial, unique index and an inverted one from a v2 migration,
 * which the next one removes: undoing that creates them again with
 * everything they said - STORING, WHERE, INVERTED.
 */
#include <db_migrate.h>

static int migrate(schema_t *db) {

  db->addColumn("v2_kennels", "traits", {type: "jsonb"});
  db->addIndexWith("v2_kennels", "v2_kennels_cover", ["name"],
                   {unique: true, where: "capacity > 0",
                    storing: ["capacity", "traits"]});

  return db->addIndexWith("v2_kennels", "v2_kennels_traits", ["traits"],
                          {inverted: true});
}

DBM_MIGRATION_V2(migrate)
