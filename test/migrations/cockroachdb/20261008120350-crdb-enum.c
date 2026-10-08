/**
 * An enum created and used in the same migration, in one transaction, both
 * ways: the type and the table that uses it are made together, and dropped
 * together.
 *
 * What CockroachDB (measured on v24.3) does not take in one transaction is
 * dropping a *column* of an existing table and then its type - the column is
 * gone only when the transaction commits, and until then the type is still in
 * use. Dropping the whole table and then the type is fine.
 */
#include <db_migrate.h>
#include <db_migrate/cockroachdb.h>

static int up(migrator_t *db) {

  db->createEnum("pet_size", ["small", "large"]);

  db->createTable("kennels", {
    id: {type: "uuid", primaryKey: true, autoIncrement: true},
    size: {type: "enum", enumName: "pet_size", notNull: true,
           defaultValue: "small"},
  });

  return db->insert("kennels", {size: "large"});
}

static int down(migrator_t *db) {

  db->dropTable("kennels");
  return db->dropEnum("pet_size");
}

DBM_MIGRATION(up, down)
