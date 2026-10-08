/**
 * CockroachDB's own: a uuid key that fills itself, a computed column, a table
 * whose rows expire, an index dropped through its table - and what it
 * inherits from PostgreSQL. The enum and the primary key are migrations of
 * their own, because CockroachDB applies a schema change when its
 * transaction commits.
 */
#include <db_migrate.h>
#include <db_migrate/cockroachdb.h>

static int up(migrator_t *db) {

  db->createSequence("pet_tag_no", {start: 1000, increment: 10});
  db->changeColumn("pets", "species", {type: "string", length: 32,
                                       defaultValue: "dog"});

  db->addColumn("pets", "name_length", {type: "computed", computedType: "int",
                                        function: "length(name)",
                                        stored: true});

  db->createTable("events", {
    columns: {
      id: {type: "uuid", primaryKey: true, autoIncrement: true},
      kind: {type: "string", length: 32, notNull: true},
      at: {type: "timestamptz", notNull: true, defaultValue: {special: "NOW"}},
    },
    expireAfter: "30 days",
  });

  db->addIndex("events", "events_kind_idx", ["kind"]);

  return 0;
}

static int down(migrator_t *db) {

  db->removeIndex("events", "events_kind_idx");
  db->dropTable("events");

  db->removeColumn("pets", "name_length");

  /**
   * Only the default goes back. Narrowing a VARCHAR again is a rewrite of
   * the column, which CockroachDB does only behind an experimental setting
   * and never inside a transaction - so a widening is one-way there.
   */
  db->changeColumn("pets", "species", {defaultValue: "cat"});
  db->dropSequence("pet_tag_no");

  return 0;
}

DBM_MIGRATION(up, down)
