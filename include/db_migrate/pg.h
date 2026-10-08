#ifndef DB_MIGRATE_PG_DEFINED
#define DB_MIGRATE_PG_DEFINED

/**
 * What only PostgreSQL - and CockroachDB, which speaks it - has, as methods
 * of `migrator_t`.
 *
 * Including this is what makes `db->createSequence(...)` exist: a migration
 * that does not include it cannot call it, and the compiler says so rather
 * than a database at run time. Called against another database, they fail
 * the migration with a reason that names both.
 */

#include "../db_migrate.h"

/** `db->createSequence("order_no", {start: 1000, increment: 1})` */
int migrator_t__createSequence(migrator_t *self, const char *name, json_t spec);
int migrator_t__dropSequence(migrator_t *self, const char *name);

/** `db->createEnum("size", ["small", "large"])` */
int migrator_t__createEnum(migrator_t *self, const char *name, json_t values);
int migrator_t__renameEnum(migrator_t *self, const char *name, const char *to);
int migrator_t__addEnumType(migrator_t *self, const char *name,
                            const char *value);

/** CockroachDB only: PostgreSQL cannot take a value out of an enum. */
int migrator_t__dropEnumType(migrator_t *self, const char *name,
                             const char *value);
int migrator_t__dropEnum(migrator_t *self, const char *name);

#endif
