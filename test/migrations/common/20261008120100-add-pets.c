#include <db_migrate.h>

/* what a pet's name may be, said once and used twice */
static const long nameLength = 48;

static int up(migrator_t *db) {

  db->createTable("pets", {
    id: {type: "int", primaryKey: true, autoIncrement: true},
    owner_id: {type: "int", notNull: true,
               foreignKey: {name: "pets_owner_fk", table: "owners",
                            mapping: "id", rules: {onDelete: "CASCADE"}}},
    name: {type: "string", length: nameLength, notNull: true},
    species: {type: "string", length: 16, defaultValue: "cat"},
    vaccinated: {type: "boolean", notNull: true, defaultValue: false},
    traits: "json",
  });

  db->addIndex("pets", "pets_name_idx", ["name"]);
  db->addUniqueIndex("pets", "pets_owner_name_idx", ["owner_id", "name"]);

  db->insert("owners", {name: "tobi"});

  /* asked rather than assumed: CockroachDB's SERIAL is not 1, 2, 3 */
  json_t owner = db->all(SQL`select id from owners where name = ${"tobi"}`);
  defer owner.release();

  long ownerId = owner[0].id;

  const char *names[] = {"Kurbel", "Mausi", "O'Malley"};

  for (int i = 0; i < 3; ++i)
    db->insert("pets", {owner_id: ownerId, name: names[i],
                        vaccinated: i % 2 == 0,
                        traits: {lazy: i == 1, legs: 4}});

  return 0;
}

static int down(migrator_t *db) {
  return db->dropTable("pets");
}

DBM_MIGRATION(up, down)
