#ifndef DBM_PG_DRIVER_DEFINED
#define DBM_PG_DRIVER_DEFINED

/**
 * The PostgreSQL driver's pieces, for a driver that is PostgreSQL with
 * differences - CockroachDB, as `db-migrate-pg`'s `base` was for node.
 *
 *   driver_t *self = dbmPgConnect(config, why, room, "cockroachdb",
 *                                 sizeof(dbm_pg_t));
 *   self->mapDataType = crdbMapDataType;     // and its own `_super` is
 *                                            // dbmPgMapDataType
 */

#include <db_migrate_driver.h>

#include <libpq-fe.h>

typedef struct {
  driver_t base;
  PGconn *conn;
} dbm_pg_t;

/**
 * Connects and answers a driver with every PostgreSQL slot set, under
 * `name`, with `size` bytes - at least `sizeof(dbm_pg_t)` - for a driver
 * that keeps more.
 */
driver_t *dbmPgConnect(json_t config, char *why, size_t room, const char *name,
                       size_t size);

const char *dbmPgMapDataType(driver_t *self, const char *type);
int dbmPgColumnDef(driver_t *self, dbm_text_t *out, const char *table,
                   const char *column, json_t spec,
                   const dbm_column_options_t *options);
int dbmPgChangeColumn(driver_t *self, const char *table, const char *column,
                      json_t spec);

#endif
