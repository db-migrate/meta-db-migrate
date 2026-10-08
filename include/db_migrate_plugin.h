#ifndef DB_MIGRATE_PLUGIN_DEFINED
#define DB_MIGRATE_PLUGIN_DEFINED

/**
 * Plugins: what node db-migrate's hooks did, compiled in.
 *
 * A plugin is code in the program, like a driver, and says what it does from
 * its own constructor - nothing has to name it anywhere else:
 *
 *   static bool readToml(const char *file, json_t *config, char *why,
 *                        size_t room) { ... }
 *
 *   __attribute__((constructor)) static void registerToml(void) {
 *     dbmRegisterConfigLoader(".toml", readToml);
 *   }
 *
 * The shipped ones are built beside the drivers and linked by name
 * (`build-app.sh . ./app pg yaml`). A project's own go in its `plugins/`
 * directory as meta sources: the launcher compiles and opens them before it
 * reads anything else, and build-app.sh compiles them in.
 *
 * node's hooks, and what became of them:
 *
 *   init:config:overwrite:require    dbmRegisterConfigLoader
 *   connection:tunnel:<type>         dbmRegisterTunnel (ssh is built in)
 *   create:template                  dbmRegisterTemplate
 *   file:hook:require                - it told node which files to require
 *   init:api:*                       - the programmatic API is C, not hooks
 */

#include "db_migrate.h"

/* ------------------------------------------------------- configuration */

/**
 * Reads a configuration file of the kind it was registered for into the
 * same shape database.json has - environments, each a connection - and
 * answers whether it could, with the reason in `why` when not.
 */
typedef bool (*dbm_config_loader_t)(const char *file, json_t *config, char *why,
                                    size_t room);

/**
 * For files ending in `extension` (".yml"). `--config x.yml` uses it, and so
 * does a project without database.json that has `database<extension>`.
 */
void dbmRegisterConfigLoader(const char *extension, dbm_config_loader_t load);

/** The loader for a file, by its ending, or NULL. */
dbm_config_loader_t dbmConfigLoaderFor(const char *file);

/**
 * Why a shipped plugin that the launcher has could not be opened - a library
 * it needs is not installed - or "" when none failed.
 */
const char *dbmPluginLoadError(void);

/** The registered endings, for a project that has no database.json. */
const char *dbmConfigExtension(size_t at);

/* ------------------------------------------------------------- tunnels */

/**
 * Opens a tunnel described by a connection's `tunnel` object to `host:port`,
 * and answers the port on 127.0.0.1 the connection goes to instead - or -1,
 * with the reason in `why`. `handle` is what `close` is given afterwards.
 */
typedef long (*dbm_tunnel_open_t)(json_t tunnel, const char *host, long port,
                                  void **handle, char *why, size_t room);
typedef void (*dbm_tunnel_close_t)(void *handle);

/** For `tunnel: {tunnelType: "<type>", ...}`; node's default type is ssh. */
void dbmRegisterTunnel(const char *type, dbm_tunnel_open_t open,
                       dbm_tunnel_close_t close);

/** A tunnel that is open, for dbmTunnelClose. */
typedef struct dbm_tunnel_t dbm_tunnel_t;

/**
 * What node does with a connection that has a `tunnel`: opens it to the
 * connection's host and port, and points the connection at the tunnel's end
 * instead - `*config` is replaced by that. Without a `tunnel` nothing happens
 * and `*opened` stays NULL. False, with the reason, when it could not.
 */
bool dbmTunnelOpen(json_t *config, dbm_tunnel_t **opened, char *why,
                   size_t room);

void dbmTunnelClose(dbm_tunnel_t *tunnel);

/* ----------------------------------------------------------- templates */

/**
 * Writes a new migration for `create --template <name>`: into `dir`, named
 * `<stamp>-<title>`, in whatever form it makes them. Answers 0 when it did.
 */
typedef int (*dbm_template_t)(const char *dir, const char *stamp,
                              const char *title);

void dbmRegisterTemplate(const char *name, dbm_template_t write);

/** The template of that name, or NULL. */
dbm_template_t dbmTemplateNamed(const char *name);

#endif
