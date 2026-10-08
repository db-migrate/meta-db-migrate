/**
 * SQLite's own: a foreign key that has to be said on the column, a rename
 * and a drop of a column - and the one thing it cannot do, refused with a
 * reason rather than half done.
 */
#include <db_migrate.h>

static int up(migrator_t *db) {

  db->createTable("toys", {
    id: {type: "int", primaryKey: true, autoIncrement: true},
    pet_id: {type: "int", notNull: true,
             foreignKey: {table: "pets", mapping: "id",
                          rules: {onDelete: "CASCADE"}}},
    name: {type: "string", length: 32},
  });

  db->renameColumn("pets", "slug", "handle");
  db->addColumn("pets", "scratch", {type: "int"});
  db->removeColumn("pets", "scratch");

  return 0;
}

static int down(migrator_t *db) {

  db->renameColumn("pets", "handle", "slug");
  db->dropTable("toys");

  return 0;
}

DBM_MIGRATION(up, down)
