#ifndef DB_MIGRATE_COCKROACHDB_DEFINED
#define DB_MIGRATE_COCKROACHDB_DEFINED

/**
 * What only CockroachDB has, as methods of `migrator_t` - and everything
 * PostgreSQL has, because CockroachDB speaks it: sequences and enums come in
 * with this header too.
 */

#include "pg.h"

/** `db->changePrimaryKey("pets", ["owner_id", "id"])` */
int migrator_t__changePrimaryKey(migrator_t *self, const char *table,
                                 json_t columns);

#endif
