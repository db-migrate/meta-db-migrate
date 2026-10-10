/**
 * The plugin registries: configuration loaders, tunnels, templates.
 *
 * node found its plugins in package.json and asked each for the hooks it
 * had. Here a plugin is linked in, or opened by the launcher, and registers
 * from its constructor - so by the time anything asks, it is in the tables.
 *
 * The shipped plugins are built beside the drivers, and the launcher opens
 * one the first time it is wanted: a `.yml` configuration opens
 * libdbmigrate-yaml.so, a `tunnelType: "x"` opens libdbmigrate-x.so. A
 * program built with its plugins linked never opens anything.
 */
#include <db_migrate_plugin.h>

#include <dlfcn.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *extension;
  dbm_config_loader_t load;
} dbm_loader_entry_t;

typedef struct {
  const char *type;
  dbm_tunnel_open_t open;
  dbm_tunnel_close_t close;
} dbm_tunnel_entry_t;

typedef struct {
  const char *name;
  dbm_template_t write;
} dbm_template_entry_t;

static dbm_loader_entry_t[] loaders;
static dbm_tunnel_entry_t[] tunnels;
static dbm_template_entry_t[] templates;

struct dbm_tunnel_t {
  dbm_tunnel_close_t close;
  void *handle;
};

void dbmRegisterConfigLoader(const char *extension, dbm_config_loader_t load) {
  loaders.push((dbm_loader_entry_t){extension, load});
}

void dbmRegisterTunnel(const char *type, dbm_tunnel_open_t open,
                       dbm_tunnel_close_t close) {
  tunnels.push((dbm_tunnel_entry_t){type, open, close});
}

void dbmRegisterTemplate(const char *name, dbm_template_t write) {
  templates.push((dbm_template_entry_t){name, write});
}

/** Why the last shipped plugin that is there could not be opened. */
static char shippedError[600];

const char *dbmPluginLoadError(void) {
  return shippedError;
}

/**
 * A shipped plugin, opened from where the launcher keeps the drivers. Quiet
 * when it is not there: the caller says what was missing, in its own words.
 * When it is there and does not open - a library it needs is not installed -
 * that is kept, because then the caller's words would be wrong.
 */
static void openShipped(const char *name) {

  char path[1024];
  struct stat seen;

  if (dbmDriverDirectory == NULL)
    return;

  dbmWrite(path, sizeof path, TEXT`${dbmDriverDirectory}/libdbmigrate-${name}.so`);

  if (stat(path, &seen) != 0)
    return;

  if (dlopen(path, RTLD_NOW | RTLD_GLOBAL) == NULL)
    dbmWrite(shippedError, sizeof shippedError,
             TEXT`the ${name} plugin could not be loaded: ${dlerror()}`);
}

static bool endsWith(const char *text, const char *end) {

  size_t length = strlen(text);
  size_t endLength = strlen(end);

  return length >= endLength && strcmp(text + length - endLength, end) == 0;
}

static dbm_config_loader_t loaderFor(const char *file) {

  for (entry in loaders)
    if (endsWith(file, entry->extension))
      return entry->load;

  return NULL;
}

/** The plugin that reads files with this ending: `.yml` is yaml's. */
static const char *pluginReading(const char *file) {

  if (endsWith(file, ".yml") || endsWith(file, ".yaml"))
    return "yaml";

  const char *dot = strrchr(file, '.');

  return dot != NULL ? dot + 1 : NULL;
}

dbm_config_loader_t dbmConfigLoaderFor(const char *file) {

  dbm_config_loader_t load = loaderFor(file);

  if (load != NULL)
    return load;

  const char *plugin = pluginReading(file);

  if (plugin == NULL || strcmp(plugin, "json") == 0)
    return NULL;

  openShipped(plugin);
  return loaderFor(file);
}

/** The shipped ones first, so a project without database.json finds them. */
static const char *const shippedExtensions[] = {".yml", ".yaml"};

const char *dbmConfigExtension(size_t at) {

  size_t shipped = sizeof shippedExtensions / sizeof shippedExtensions[0];

  if (at < shipped)
    return shippedExtensions[at];

  at -= shipped;
  return at < (size_t)loaders.count ? loaders.items[at].extension : NULL;
}

dbm_template_t dbmTemplateNamed(const char *name) {

  for (entry in templates)
    if (strcmp(entry->name, name) == 0)
      return entry->write;

  return NULL;
}

static dbm_tunnel_entry_t *tunnelOfType(const char *type) {

  for (entry in tunnels)
    if (strcmp(entry->type, type) == 0)
      return entry;

  return NULL;
}

/** The port a driver listens on when the connection does not say. */
static long defaultPort(const char *driver) {

  if (driver in {"pg", "postgres", "postgresql"})
    return 5432;

  if (strcmp(driver, "cockroachdb") == 0)
    return 26257;

  if (strcmp(driver, "mysql") == 0)
    return 3306;

  return 0;
}

bool dbmTunnelOpen(json_t *config, dbm_tunnel_t **opened, char *why,
                   size_t room) {

  json_t connection = *config;
  json_t tunnel = connection.tunnel;

  *opened = NULL;

  if (tunnel.isNothing())
    return true;

  if (strcmp(tunnel.kind(), "object") != 0) {
    dbmWrite(why, room, TEXT`tunnel is an object: {"host": "bastion", "username": "deploy"}`);
    return false;
  }

  const char *host = connection.host;
  long port = connection.port.isNothing() ? defaultPort(connection.driver)
                                          : connection.port.number();

  if (host[0] == '\0' || port <= 0) {
    dbmWrite(why, room, TEXT`a tunnel goes to the connection's host and port, and this one names ${host[0] == '\0' ? "no host" : "no port"}`);
    return false;
  }

  /* node's default is ssh, and so is anything that says it */
  const char *type = tunnel.tunnelType;

  if (type[0] == '\0')
    type = "ssh";

  dbm_tunnel_entry_t *entry = tunnelOfType(type);

  if (entry == NULL) {
    openShipped(type);
    entry = tunnelOfType(type);
  }

  if (entry == NULL && shippedError[0] != '\0') {
    dbmWrite(why, room, TEXT`${shippedError}`);
    return false;
  }

  if (entry == NULL) {
    dbmWrite(why, room, TEXT`there is no ${type} tunnel in this program - link the plugin that makes one`);
    return false;
  }

  void *handle = NULL;
  long local = entry->open(tunnel, host, port, &handle, why, room);

  if (local < 0) {
    if (why[0] == '\0')
      dbmWrite(why, room, TEXT`the ${type} tunnel to ${host}:${port} did not open`);
    return false;
  }

  /* the connection, pointed at the tunnel's end; the tunnel is not its business */
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *copy = yyjson_mut_val_mut_copy(doc, connection.node);

  yyjson_mut_obj_remove_key(copy, "tunnel");
  yyjson_mut_obj_remove_key(copy, "host");
  yyjson_mut_obj_remove_key(copy, "port");
  yyjson_mut_obj_add_str(doc, copy, "host", "127.0.0.1");
  yyjson_mut_obj_add_int(doc, copy, "port", local);
  yyjson_mut_doc_set_root(doc, copy);

  connection.release();
  *config = meta_jsonFromMut(doc);

  *opened = malloc(sizeof **opened);
  (*opened)->close = entry->close;
  (*opened)->handle = handle;
  return true;
}

void dbmTunnelClose(dbm_tunnel_t *tunnel) {

  if (tunnel == NULL)
    return;

  tunnel->close(tunnel->handle);
  free(tunnel);
}
