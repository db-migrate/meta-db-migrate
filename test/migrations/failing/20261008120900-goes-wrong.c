/**
 * Fails halfway. The table it creates first has to be gone afterwards, and
 * so does any record of this migration - which is what running it inside
 * one transaction with its record buys.
 */
#include <db_migrate.h>

static int up(migrator_t *db) {

  db->createTable("half_done", {id: {type: "int", primaryKey: true}});

  /* this fails, and the steps after it do nothing */
  db->runSql("insert into table_that_does_not_exist values (1)");
  db->createTable("never_made", {id: {type: "int", primaryKey: true}});

  return 0;
}

static int down(migrator_t *db) {
  return db->dropTable("half_done");
}

DBM_MIGRATION(up, down)
