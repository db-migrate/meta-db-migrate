/**
 * A data migration: the reason migrations are code and not a list of
 * statements. Every row is read, a slug is made in the program, and written
 * back with the value as a parameter rather than in the text.
 */
#include <db_migrate.h>

#include <ctype.h>
#include <string.h>

static void slugify(const char *name, char *into, size_t room) {

  size_t at = 0;

  for (; *name != '\0' && at + 1 < room; ++name)
    if (isalnum((unsigned char)*name))
      into[at++] = (char)tolower((unsigned char)*name);

  into[at] = '\0';
}

static int up(migrator_t *db) {

  json_t pets = db->all(SQL`select id, name from pets order by id`);
  defer pets.release();

  for (int i = 0; i < pets.count(); ++i) {

    char slug[64];
    long id = pets[i].id;

    slugify(pets[i].name, slug, sizeof slug);
    db->run(SQL`update pets set slug = ${slug} where id = ${id}`);
  }

  return 0;
}

static int down(migrator_t *db) {
  return db->run(SQL`update pets set slug = null`);
}

DBM_MIGRATION(up, down)
