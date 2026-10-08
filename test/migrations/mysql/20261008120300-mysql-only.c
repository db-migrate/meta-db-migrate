/**
 * MySQL's own: table options, unsigned and sized text, a boolean that is a
 * TINYINT, a column restated whole, an index and a foreign key dropped by
 * MySQL's own statements.
 */
#include <db_migrate.h>

static int up(migrator_t *db) {

  db->createTable("audit", {
    columns: {
      id: {type: "int", primaryKey: true, autoIncrement: true,
           unsigned: true},
      note: {type: "text", length: 70000},
      flag: {type: "boolean", notNull: true, defaultValue: true},
      created: {type: "datetime", defaultValue: {special: "CURRENT_TIMESTAMP"},
                comment: "when it happened"},
    },
    engine: "InnoDB",
    charset: "utf8mb4",
  });

  db->changeColumn("pets", "species", {type: "string", length: 32,
                                       defaultValue: "dog"});

  db->removeForeignKey("pets", "pets_owner_fk");
  db->addForeignKey("pets", "owners", "pets_owner_restrict",
                    {owner_id: "id"}, {onDelete: "RESTRICT"});

  db->addIndex("audit", "audit_flag_idx", ["flag"]);

  return 0;
}

static int down(migrator_t *db) {

  db->removeIndex("audit", "audit_flag_idx");

  db->removeForeignKey("pets", "pets_owner_restrict");
  db->addForeignKey("pets", "owners", "pets_owner_fk", {owner_id: "id"},
                    {onDelete: "CASCADE"});

  db->changeColumn("pets", "species", {type: "string", length: 16,
                                       defaultValue: "cat"});

  db->dropTable("audit");

  return 0;
}

DBM_MIGRATION(up, down)
