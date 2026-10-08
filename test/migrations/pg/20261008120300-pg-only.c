/**
 * What only PostgreSQL has, reached through the header that adds it.
 */
#include <db_migrate.h>
#include <db_migrate/pg.h>

static int up(migrator_t *db) {

  db->createSequence("pet_tag_no", {start: 1000, increment: 10});
  db->changeColumn("pets", "species", {type: "string", length: 32,
                                       defaultValue: "dog"});

  db->createEnum("pet_size", ["small", "large"]);
  db->addColumn("pets", "size", {type: "pet_size", defaultValue: "small"});

  return 0;
}

static int down(migrator_t *db) {

  db->removeColumn("pets", "size");
  db->dropEnum("pet_size");

  db->changeColumn("pets", "species", {type: "string", length: 16,
                                       defaultValue: "cat"});
  db->dropSequence("pet_tag_no");

  return 0;
}

DBM_MIGRATION(up, down)
