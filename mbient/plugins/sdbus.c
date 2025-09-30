/**
 * The MIT License
 *
 * Copyright (C) 2020 MBition GmbH
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *   Frank Bielig <frank dot bielig at mbition.io>
 */

#include "plugin.h"
#include "utils/common/common.h"
#include "utils_time.h"
#include "collectd.h"

#include "daemon/utils_time.h"
#include "systemd/sd-bus.h"
#include "utils/latency/latency.h"

/* ************************************************************************* */
/* types */
/* ************************************************************************* */

typedef enum { TARGET_LOCAL_USER = 1, TARGET_LOCAL_SYSTEM = 2 } sdbus_bind_t;

/* ------------------------------------------------------------------------- */
typedef struct sdbus_latency_s {

  gauge_t value;
  latency_counter_t *history;

} sdbus_latency_t;

/* ------------------------------------------------------------------------- */
typedef struct sdbus_metric_s {

  sdbus_latency_t user_local_latency;
  sdbus_latency_t user_peer_latency;
  sdbus_latency_t system_local_latency;
  sdbus_latency_t system_peer_latency;

  derive_t user_messages;
  derive_t system_messages;
} sdbus_metric_t;

/* ------------------------------------------------------------------------- */
typedef struct client_latency_info_s {
  const char *name;
  sd_bus **bus;
  const char *destination;
  const char *path;
  const char *interface;
  const char *member;
} client_latency_info_t;

/* ------------------------------------------------------------------------- */
typedef struct server_info_s {
  sd_bus **bus;
  sdbus_bind_t bus_type;
  pthread_t thread;
  pthread_mutex_t lock;
  bool created;
  bool running;
  bool shutdown;
} server_info_t;

/* ************************************************************************* */
/* constants */
/* ************************************************************************* */

#define ENABLE_COUNT 0
#define ENABLE_LATENCY 1
#define ENABLE_MONITOR 1

#define LOG_KEY "sdbus: "
#define LOG_KEY_NAMES LOG_KEY "sdbus_names - "
#define LOG_KEY_SERVER LOG_KEY "server - "
#define LOG_KEY_MONITOR LOG_KEY "monitor - "

#define SERVER_DESTINATION "org.collectd.SDBus"
#define SERVER_MEMBER "/org/collectd/SDBus"
#define SERVER_INTERFACE "org.collectd.SDBus"
#define SERVER_METHOD_PING "LocalPing"

#define DBUS_DESTINATION "org.freedesktop.DBus"
#define DBUS_MEMBER "/org/freedesktop/DBus"

#define PEER_INTERFACE "org.freedesktop.DBus.Peer"
#define PEER_METHOD_PING "Ping"

#define MONIT_SERVICE "org.freedesktop.DBus.Monitoring"
#define MONIT_METHOD_BECOME "BecomeMonitor"

#define PLUGIN_KEY "sdbus"
#define PLUGIN_NAME PLUGIN_KEY

#define COUNT_INTERVAL 60000 // milliseconds

#define CHECK_ERROR(r, msg)                                                    \
  if (r < 0) {                                                                 \
    WARNING(LOG_KEY "%s: %s (%d)", msg, strerror(-r), r);                      \
    goto error;                                                                \
  }

/* ************************************************************************* */
/* global variables */
/* ************************************************************************* */

static sd_bus *bus_user = NULL;
static sd_bus *bus_system = NULL;
static sd_bus *bus_user_server = NULL;
static sd_bus *bus_system_server = NULL;
static sd_bus *bus_user_monitor = NULL;
static sd_bus *bus_system_monitor = NULL;

static server_info_t user_server = {.bus = &bus_user_server,
                                    .bus_type = TARGET_LOCAL_USER,
                                    .lock = PTHREAD_MUTEX_INITIALIZER,
                                    .created = false,
                                    .running = false,
                                    .shutdown = false};
static server_info_t system_server = {.bus = &bus_system_server,
                                      .bus_type = TARGET_LOCAL_SYSTEM,
                                      .lock = PTHREAD_MUTEX_INITIALIZER,
                                      .created = false,
                                      .running = false,
                                      .shutdown = false};
static server_info_t user_monitor = {.bus = &bus_user_monitor,
                                     .bus_type = TARGET_LOCAL_USER,
                                     .lock = PTHREAD_MUTEX_INITIALIZER,
                                     .created = false,
                                     .running = false,
                                     .shutdown = false};
static server_info_t system_monitor = {.bus = &bus_system_monitor,
                                       .bus_type = TARGET_LOCAL_SYSTEM,
                                       .lock = PTHREAD_MUTEX_INITIALIZER,
                                       .created = false,
                                       .running = false,
                                       .shutdown = false};

static client_latency_info_t user_local_ping = {.name = "user-local",
                                                .bus = &bus_user,
                                                .destination =
                                                    SERVER_DESTINATION,
                                                .member = SERVER_MEMBER,
                                                .interface = SERVER_INTERFACE,
                                                .path = SERVER_METHOD_PING};
static client_latency_info_t user_peer_ping = {.name = "user-peer",
                                               .bus = &bus_user,
                                               .destination = DBUS_DESTINATION,
                                               .member = DBUS_MEMBER,
                                               .interface = PEER_INTERFACE,
                                               .path = PEER_METHOD_PING};
static client_latency_info_t system_local_ping = {.name = "system-local",
                                                  .bus = &bus_system,
                                                  .destination =
                                                      SERVER_DESTINATION,
                                                  .member = SERVER_MEMBER,
                                                  .interface = SERVER_INTERFACE,
                                                  .path = SERVER_METHOD_PING};
static client_latency_info_t system_peer_ping = {.name = "system-peer",
                                                 .bus = &bus_system,
                                                 .destination =
                                                     DBUS_DESTINATION,
                                                 .member = DBUS_MEMBER,
                                                 .interface = PEER_INTERFACE,
                                                 .path = PEER_METHOD_PING};

static pthread_mutex_t sdbus_metric_lock;
static sdbus_metric_t *sdbus_metric = 0;
static cdtime_t sdbus_count_last_measurement = 0;

/* ************************************************************************* */
/* helper functions */
/* ************************************************************************* */

static sdbus_metric_t *sdbus_metric_create() {
  sdbus_metric_t *metric = calloc(1, sizeof(*metric));
  if (metric == NULL) {
    ERROR(LOG_KEY "calloc of mectric failed.");
    return NULL;
  }
  metric->user_local_latency.history = latency_counter_create();
  metric->user_peer_latency.history = latency_counter_create();
  metric->system_local_latency.history = latency_counter_create();
  metric->system_peer_latency.history = latency_counter_create();

  return metric;
}

/* ------------------------------------------------------------------------- */
static void sdbus_metric_destroy(sdbus_metric_t **metric) {
  if (metric == NULL)
    return;
  if (*metric == NULL)
    return;

  pthread_mutex_lock(&sdbus_metric_lock);

  latency_counter_destroy((*metric)->system_local_latency.history);
  latency_counter_destroy((*metric)->system_peer_latency.history);
  latency_counter_destroy((*metric)->user_local_latency.history);
  latency_counter_destroy((*metric)->user_peer_latency.history);

  sfree(*metric);
  pthread_mutex_unlock(&sdbus_metric_lock);
}

/* ------------------------------------------------------------------------- */
static char **strv_free(char **strv) {
  char **str;

  if (!strv)
    return NULL;

  for (str = strv; *str; str++)
    free(*str);

  free(strv);
  return NULL;
}

/* ------------------------------------------------------------------------- */
static derive_t strv_length(char *const *strv) {
  derive_t n = 0;

  if (!strv)
    return 0;

  for (; *strv; strv++)
    n++;

  return n;
}

/* ------------------------------------------------------------------------- */
static int sdbus_acquire(sd_bus **bus, sdbus_bind_t type, bool is_monitor) {
  int r;
  const char *addr = NULL;

  switch (type) {
  case TARGET_LOCAL_USER:
    addr = getenv("DBUS_SESSION_BUS_ADDRESS");
    break;
  case TARGET_LOCAL_SYSTEM:
    addr = "unix:path=/run/dbus/system_bus_socket";
    break;
  default:
    ERROR(LOG_KEY "invalid bus type %d", type);
    return -1;
  }
  if (!addr) {
    ERROR(LOG_KEY "no address found for bus %d", type);
    return -1;
  }

  r = sd_bus_new(bus);
  CHECK_ERROR(r, LOG_KEY "failed to allocate bus");

  if (is_monitor) {
    r = sd_bus_set_monitor(*bus, true);
    CHECK_ERROR(r, LOG_KEY "failed to set monitor mode");

    r = sd_bus_negotiate_creds(*bus, true, _SD_BUS_CREDS_ALL);
    CHECK_ERROR(r, LOG_KEY "failed to enable credentials");

    r = sd_bus_negotiate_timestamp(*bus, true);
    CHECK_ERROR(r, LOG_KEY "failed to enable timestamps");

    r = sd_bus_negotiate_fds(*bus, true);
    CHECK_ERROR(r, LOG_KEY "failed to enable fds");
  }

  r = sd_bus_set_bus_client(*bus, true);
  CHECK_ERROR(r, LOG_KEY "failed to set bus client");

  r = sd_bus_set_address(*bus, addr);
  if (r < 0) {
    WARNING(LOG_KEY "failed to set address to '%s': %s", addr, strerror(-r));
    return r;
  }

  r = sd_bus_start(*bus);
  CHECK_ERROR(r, LOG_KEY "failed to start bus");

  return 0;

error:
  if (*bus) {
    sd_bus_unrefp(bus);
    *bus = NULL;
  }
  return r;
}

/* ------------------------------------------------------------------------- */
static int sdbus_close(sd_bus **bus) {
  if (*bus != NULL) {
    sd_bus_close(*bus);
    sd_bus_unref(*bus);
    *bus = NULL;
  }

  return 0;
}

/* ------------------------------------------------------------------------- */
static char **sdbus_names(sd_bus *bus, bool activatable) {
  int r;
  char **result = NULL;
  if (activatable)
    r = sd_bus_list_names(bus, NULL, &result);
  else
    r = sd_bus_list_names(bus, &result, NULL);

  switch (r) {
  case 0:
    break;
  case -EINVAL:
    ERROR(LOG_KEY_NAMES "bus or both acquired and activatable were NULL.");
    break;
  case -ENOPKG:
    ERROR(LOG_KEY_NAMES "The bus cannot be resolved.");
    break;
  case -ECHILD:
    ERROR(LOG_KEY_NAMES "The bus was created in a different process.");
    break;
  case -ENOMEM:
    ERROR(LOG_KEY_NAMES "Memory allocation failed.");
    break;
  case -ENOTCONN:
    ERROR(LOG_KEY_NAMES "The bus is not connected.");
    break;
  default:
    ERROR(LOG_KEY_NAMES "failed list names: %d", r);
    break;
  }
  if (r < 0)
    return NULL;

  return result;
}

/* ------------------------------------------------------------------------- */
static const char *sdbus_error_message(const sd_bus_error *e, int error) {

  if (e) {
    if (sd_bus_error_has_name(e, SD_BUS_ERROR_ACCESS_DENIED))
      return "Access denied";

    if (e->message)
      return e->message;
  }

  return strerror(abs(error));
}

/* ------------------------------------------------------------------------- */
static int sdbus_count_active(sd_bus *bus, derive_t *unique,
                              derive_t *acquired) {
  char **names = sdbus_names(bus, false);
  char **strv;

  if (names == NULL)
    return -1;

  *unique = 0;
  *acquired = 0;

  for (strv = names; *strv; strv++) {
    if ((*strv)[0] == ':') {
      ++*unique;
    } else {
      ++*acquired;
    }
  }

  strv_free(names);

  return 0;
}

/* ------------------------------------------------------------------------- */
static int sdbus_count_activatable(sd_bus *bus, derive_t *activatable) {
  char **names = sdbus_names(bus, true);
  if (names == NULL)
    return -1;

  *activatable = strv_length(names);
  strv_free(names);

  return 0;
}

/* ------------------------------------------------------------------------- */
static cdtime_t sdbus_call(sd_bus *bus, const char *destination,
                           const char *member, const char *interface,
                           const char *method) {

  sd_bus_error error = SD_BUS_ERROR_NULL;
  sd_bus_message *m = NULL;
  cdtime_t latency = ~0;
  int r;

  DEBUG(LOG_KEY "call of 'busctl call %s %s %s %s' via %p", destination, member,
        interface, method, bus);

  if (bus == NULL) {
    ERROR(LOG_KEY "call of 'busctl call %s %s %s %s' failed with invalid bus",
          destination, member, interface, method);
    return 0;
  }

  cdtime_t start = cdtime();
  r = sd_bus_call_method(bus, destination, member, interface, method, &error,
                         &m, "");
  if (r >= 0) {
    latency = cdtime() - start;
  } else {
    ERROR(LOG_KEY
          "call of 'busctl call %s %s %s %s' failed with %d (%s) via %p",
          destination, member, interface, method, r,
          sdbus_error_message(&error, r), bus);
  }

  sd_bus_error_free(&error);
  sd_bus_message_unref(m);

  return latency;
}

/* ************************************************************************* */
/* sdbus server */
/* ************************************************************************* */

static int server_method_ping(sd_bus_message *m, void *userdata,
                              sd_bus_error *ret_error) {

  DEBUG(LOG_KEY_SERVER "ping");
  return sd_bus_reply_method_return(m, "");
}

static const sd_bus_vtable server_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD(SERVER_METHOD_PING, "", "", server_method_ping,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};

/* ------------------------------------------------------------------------- */
static void *server_main(void *args) {
  sd_bus_slot *slot = NULL;
  server_info_t *info = (server_info_t *)args;
  int r;

  r = sd_bus_add_object_vtable(*info->bus, &slot, SERVER_MEMBER,
                               SERVER_INTERFACE, server_vtable, NULL);

  if (r < 0) {
    WARNING(LOG_KEY_SERVER "#%d failed to add object: %s", info->bus_type,
            strerror(-r));
    goto finish;
  }

  r = sd_bus_request_name(*info->bus, SERVER_DESTINATION, 0);
  if (r < 0) {
    WARNING(LOG_KEY_SERVER "#%d failed to acquire service name: %s",
            info->bus_type, strerror(-r));
    goto finish;
  }

  pthread_mutex_lock(&info->lock);
  info->running = true;
  pthread_mutex_unlock(&info->lock);

  while (true) {
    pthread_mutex_lock(&info->lock);
    if (info->shutdown) {
      pthread_mutex_unlock(&info->lock);
      break;
    }
    pthread_mutex_unlock(&info->lock);

    DEBUG(LOG_KEY_SERVER "#%d process", info->bus_type);
    r = sd_bus_process(*info->bus, NULL);
    if (r < 0) {
      WARNING(LOG_KEY_SERVER "#%d failed to process bus: %s", info->bus_type,
              strerror(-r));
      goto finish;
    }
    if (r > 0)
      continue;

    DEBUG(LOG_KEY_SERVER "#%d wait", info->bus_type);
    r = sd_bus_wait(*info->bus, (uint64_t)1000000); // usec
    if (r < 0) {
      WARNING(LOG_KEY_SERVER "#%d failed to wait on bus: %s", info->bus_type,
              strerror(-r));
      goto finish;
    }
  }

finish:
  WARNING(LOG_KEY_SERVER "#%d finished", info->bus_type);
  sd_bus_slot_unref(slot);
  return NULL;
}

/* ------------------------------------------------------------------------- */
static void server_start(server_info_t *info) {
  int status;

  if (*info->bus && !info->running) {
    pthread_mutex_lock(&info->lock);

    pthread_attr_t pattr;
    status = pthread_attr_init(&pattr);
    if (status != 0) {
      ERROR(LOG_KEY_SERVER "#%d error in pthread_attr_init", info->bus_type);
      pthread_mutex_unlock(&info->lock);
      return;
    }
    status = pthread_attr_setdetachstate(&pattr, PTHREAD_CREATE_JOINABLE);
    if (status != 0) {
      ERROR(LOG_KEY_SERVER "#%d error in pthread_attr_setdetachstate",
            info->bus_type);
      pthread_mutex_unlock(&info->lock);
      return;
    }

    DEBUG(LOG_KEY_SERVER "#%d create thread", info->bus_type);
    status = pthread_create(&info->thread, &pattr, server_main, info);
    pthread_attr_destroy(&pattr);
    if (status != 0) {
      ERROR(LOG_KEY_SERVER "#%d could not start thread", info->bus_type);
      pthread_mutex_unlock(&info->lock);
      return;
    }

    info->created = true;
    DEBUG(LOG_KEY_SERVER "#%d running", info->bus_type);
    pthread_mutex_unlock(&info->lock);
  }
}

/* ------------------------------------------------------------------------- */
static void server_shutdown(server_info_t *info) {

  if (!info->created) {
    DEBUG(LOG_KEY_MONITOR "#%d skip shutdown sequence", info->bus_type);
    return;
  }

  DEBUG(LOG_KEY_SERVER "#%d start shutdown sequence", info->bus_type);
  pthread_mutex_lock(&info->lock);
  info->shutdown = true;
  pthread_mutex_unlock(&info->lock);

  DEBUG(LOG_KEY_SERVER "#%d join thread %p", info->bus_type,
        (void *)info->thread);
  pthread_join(info->thread, NULL);

  pthread_mutex_lock(&info->lock);
  info->created = false;
  info->running = false;
  pthread_mutex_unlock(&info->lock);
  DEBUG(LOG_KEY_SERVER "#%d shutdown completed", info->bus_type);
}

/* ************************************************************************* */
/* sdbus server */
/* ************************************************************************* */

/* ------------------------------------------------------------------------- */
static void *monitor_main(void *args) {
  int r;
  sd_bus_error error = SD_BUS_ERROR_NULL;
  sd_bus_message *init_message = NULL;
  uint32_t flags = 0;
  const char *unique_name = NULL;
  derive_t *counter = NULL;

  server_info_t *info = (server_info_t *)args;

  INFO(LOG_KEY_MONITOR "#%d monitor main", info->bus_type);
  switch (info->bus_type) {
  case TARGET_LOCAL_USER:
    counter = &sdbus_metric->user_messages;
    break;
  case TARGET_LOCAL_SYSTEM:
    counter = &sdbus_metric->system_messages;
    break;
  default:
    ERROR(LOG_KEY_MONITOR "#%d invalid bus type", info->bus_type);
    return NULL;
  }

  r = sd_bus_message_new_method_call(*info->bus, &init_message,
                                     DBUS_DESTINATION, DBUS_MEMBER,
                                     MONIT_SERVICE, MONIT_METHOD_BECOME);
  if (r < 0) {
    WARNING(LOG_KEY_MONITOR "#%d failed to create message: %s", info->bus_type,
            strerror(-r));
    goto finish;
  }
  r = sd_bus_message_open_container(init_message, 'a', "s");
  if (r < 0) {
    WARNING(LOG_KEY_MONITOR "#%d failed to open container: %s", info->bus_type,
            strerror(-r));
    goto finish;
  }
  r = sd_bus_message_close_container(init_message);
  if (r < 0) {
    WARNING(LOG_KEY_MONITOR "#%d failed to close container: %s", info->bus_type,
            strerror(-r));
    goto finish;
  }
  r = sd_bus_message_append_basic(init_message, 'u', &flags);
  if (r < 0) {
    WARNING(LOG_KEY_MONITOR "#%d failed to append flags: %s", info->bus_type,
            strerror(-r));
    goto finish;
  }
  r = sd_bus_call(*info->bus, init_message, 1000000, &error, NULL);
  if (r < 0) {
    ERROR(LOG_KEY_MONITOR
          "#%d call of 'busctl call %s %s %s %s' failed with %d (%s) via %p",
          info->bus_type, DBUS_DESTINATION, DBUS_MEMBER, MONIT_SERVICE,
          MONIT_METHOD_BECOME, r, sdbus_error_message(&error, r), *info->bus);
    goto finish;
  }

  r = sd_bus_get_unique_name(*info->bus, &unique_name);
  if (r < 0) {
    WARNING(LOG_KEY_MONITOR "#%d failed to get unique name: %s", info->bus_type,
            strerror(-r));
    goto finish;
  }

  INFO(LOG_KEY_MONITOR "#%d monitoring on bus %s activated", info->bus_type,
       unique_name);

  pthread_mutex_lock(&info->lock);
  info->running = true;
  pthread_mutex_unlock(&info->lock);

  while (true) {
    pthread_mutex_lock(&info->lock);
    if (info->shutdown) {
      pthread_mutex_unlock(&info->lock);
      break;
    }
    pthread_mutex_unlock(&info->lock);

    sd_bus_message *m = NULL;

    DEBUG(LOG_KEY_MONITOR "#%d process", info->bus_type);

    r = sd_bus_process(*info->bus, &m);
    if (r < 0) {
      WARNING(LOG_KEY_MONITOR "#%d failed to process bus: %s", info->bus_type,
              strerror(-r));
      goto finish;
    }

    if (m) {
      ++*counter;

      INFO(LOG_KEY_MONITOR "#%d received message %lu from %s: %s %s %s %s (%s)",
           info->bus_type, *counter, sd_bus_message_get_sender(m),
           sd_bus_message_get_destination(m), sd_bus_message_get_path(m),
           sd_bus_message_get_interface(m), sd_bus_message_get_member(m),
           sd_bus_message_get_signature(m, 1));

      if (sd_bus_message_is_signal(m, "org.freedesktop.DBus.Local",
                                   "Disconnected") > 0) {
        INFO(LOG_KEY_MONITOR "#%d connection terminated, exiting.",
             info->bus_type);
        goto finish;
      }
      sd_bus_message_unref(m);
      continue;
    }

    if (r > 0)
      continue;

    DEBUG(LOG_KEY_MONITOR "#%d wait", info->bus_type);
    r = sd_bus_wait(*info->bus, (uint64_t)1000000); // usec
    if (r < 0) {
      WARNING(LOG_KEY_MONITOR "#%d failed to wait on bus: %s", info->bus_type,
              strerror(-r));
      goto finish;
    }
  }

finish:
  DEBUG(LOG_KEY_MONITOR "#%d finished", info->bus_type);
  sd_bus_message_unref(init_message);
  sd_bus_error_free(&error);
  return NULL;
}

/* ------------------------------------------------------------------------- */
static void monitor_start(server_info_t *info) {
  int status;

  if (*info->bus && !info->running) {
    pthread_mutex_lock(&info->lock);

    pthread_attr_t pattr;
    status = pthread_attr_init(&pattr);
    if (status != 0) {
      ERROR(LOG_KEY_MONITOR "#%d error in pthread_attr_init", info->bus_type);
      pthread_mutex_unlock(&info->lock);
      return;
    }
    status = pthread_attr_setdetachstate(&pattr, PTHREAD_CREATE_JOINABLE);
    if (status != 0) {
      ERROR(LOG_KEY_MONITOR "#%d error in pthread_attr_setdetachstate",
            info->bus_type);
      pthread_mutex_unlock(&info->lock);
      return;
    }

    DEBUG(LOG_KEY_MONITOR "#%d create thread", info->bus_type);
    status = pthread_create(&info->thread, &pattr, monitor_main, info);
    pthread_attr_destroy(&pattr);
    if (status != 0) {
      ERROR(LOG_KEY_MONITOR "#%d could not start thread", info->bus_type);
      pthread_mutex_unlock(&info->lock);
      return;
    }

    info->created = true;
    INFO(LOG_KEY_MONITOR "#%d running", info->bus_type);
    pthread_mutex_unlock(&info->lock);
  }
}

/* ------------------------------------------------------------------------- */
static void monitor_shutdown(server_info_t *info) {

  if (!info->created) {
    DEBUG(LOG_KEY_MONITOR "#%d skip shutdown sequence", info->bus_type);
    return;
  }

  DEBUG(LOG_KEY_MONITOR "#%d start shutdown sequence", info->bus_type);

  pthread_mutex_lock(&info->lock);
  info->shutdown = true;
  pthread_mutex_unlock(&info->lock);

  DEBUG(LOG_KEY_MONITOR "#%d join thread %p", info->bus_type,
        (void *)info->thread);
  pthread_join(info->thread, NULL);

  pthread_mutex_lock(&info->lock);
  info->created = false;
  info->running = false;
  pthread_mutex_unlock(&info->lock);

  DEBUG(LOG_KEY_MONITOR "#%d shutdown completed", info->bus_type);
}

/* ------------------------------------------------------------------------- */
static void monitor_submit(const char *instance, derive_t value) {
  value_list_t vl = VALUE_LIST_INIT;
  value_t values[] = {
      {.derive = value},
  };

  vl.values = values;
  vl.values_len = STATIC_ARRAY_SIZE(values);

  sstrncpy(vl.plugin, PLUGIN_KEY, sizeof(vl.plugin));
  sstrncpy(vl.type, "sdbus_messages", sizeof(vl.type));
  sstrncpy(vl.type_instance, instance, sizeof(vl.type_instance));

  plugin_dispatch_values(&vl);
}

/* ************************************************************************* */
/* collection service functions */
/* ************************************************************************* */

static void sdbus_submit_count(const char *instance, derive_t unique,
                               derive_t acquired, derive_t activatable) {
  DEBUG(LOG_KEY "%s bus - unique=%lu, acquired=%lu, activatable=%lu),",
        instance, unique, acquired, activatable);

  value_list_t vl = VALUE_LIST_INIT;
  value_t values[] = {
      {.gauge = unique},
      {.gauge = acquired},
      {.gauge = activatable},
  };

  vl.values = values;
  vl.values_len = STATIC_ARRAY_SIZE(values);

  sstrncpy(vl.plugin, PLUGIN_KEY, sizeof(vl.plugin));
  sstrncpy(vl.type, "sdbus_count", sizeof(vl.type));
  sstrncpy(vl.type_instance, instance, sizeof(vl.type_instance));

  plugin_dispatch_values(&vl);
}

/* ------------------------------------------------------------------------- */
static int sdbus_count(void) {
  derive_t unique;
  derive_t acquried;
  derive_t activatable;

  cdtime_t now = cdtime();
  if (CDTIME_T_TO_MS(now - sdbus_count_last_measurement) < COUNT_INTERVAL)
    return 1;
  sdbus_count_last_measurement = now;

  if (bus_user != NULL) {
    if (sdbus_count_active(bus_user, &unique, &acquried))
      return -1;
    if (sdbus_count_activatable(bus_user, &activatable))
      return -1;
    sdbus_submit_count("user", unique, acquried, activatable);
  }

  if (bus_system != NULL) {
    if (sdbus_count_active(bus_system, &unique, &acquried))
      return -1;
    if (sdbus_count_activatable(bus_system, &activatable))
      return -1;
    sdbus_submit_count("system", unique, acquried, activatable);
  }

  return 0;
}

/* ------------------------------------------------------------------------- */
static void sdbus_latency_submit(const char *instance, gauge_t value,
                                 latency_counter_t *hist) {
  value_list_t vl = VALUE_LIST_INIT;
  value_t values[] = {
      {.gauge = value},
  };

  vl.values = values;
  vl.values_len = STATIC_ARRAY_SIZE(values);

  sstrncpy(vl.plugin, PLUGIN_KEY, sizeof(vl.plugin));
  sstrncpy(vl.type, "sdbus_latency", sizeof(vl.type));
  sstrncpy(vl.type_instance, instance, sizeof(vl.type_instance));

  plugin_dispatch_values(&vl);
}

/* ------------------------------------------------------------------------- */
static void sdbus_latency(client_latency_info_t *client,
                          sdbus_latency_t *metric) {
  if (*client->bus == NULL)
    return;

  cdtime_t latency =
      sdbus_call(*client->bus, client->destination, client->member,
                 client->interface, client->path);
  if (latency == ~0) {
    WARNING(LOG_KEY "latency %s failed", client->name);
    return;
  }

  metric->value = CDTIME_T_TO_US(latency) / 1000.0;
  latency_counter_add(metric->history, latency);
  DEBUG(LOG_KEY "%s latency %.1fms", client->name, metric->value);

  sdbus_latency_submit(client->name, metric->value, metric->history);
}

/* ------------------------------------------------------------------------- */
static cdtime_t read_time_used = 0;
static int sdbus_read(void) {
  cdtime_t now = cdtime();

  if (ENABLE_COUNT) {
    sdbus_count();
  }

  pthread_mutex_lock(&sdbus_metric_lock);

  if (ENABLE_LATENCY) {
    sdbus_latency(&user_local_ping, &sdbus_metric->user_local_latency);
    sdbus_latency(&user_peer_ping, &sdbus_metric->user_peer_latency);
    sdbus_latency(&system_local_ping, &sdbus_metric->system_peer_latency);
    sdbus_latency(&system_peer_ping, &sdbus_metric->system_peer_latency);
  }
  if (ENABLE_MONITOR) {
    if (user_monitor.running)
      monitor_submit("user", sdbus_metric->user_messages);
    if (system_monitor.running)
      monitor_submit("system", sdbus_metric->system_messages);
  }

  pthread_mutex_unlock(&sdbus_metric_lock);

  cdtime_t used = cdtime() - now;
  read_time_used += used;

  DEBUG(LOG_KEY "used time %lums (total: %lums)", CDTIME_T_TO_MS(used),
        CDTIME_T_TO_MS(read_time_used));

  return 0;
}

/* ************************************************************************* */
/* configuration */
/* ************************************************************************* */

/* ------------------------------------------------------------------------- */
static int sdbus_config(oconfig_item_t *ci) {
  INFO(LOG_KEY "configuration");
  return 0;
}

/* ------------------------------------------------------------------------- */
static int sdbus_init(void) {
  notification_t n = {NOTIF_OKAY, cdtime(), "", "",  PLUGIN_NAME,
                      "",         "",       "", NULL};
  ssnprintf(n.message, NOTIF_MAX_MSG_LEN, "collectd version: plugin %s %s",
            PLUGIN_NAME, PACKAGE_VERSION);
  plugin_dispatch_notification(&n);
  if (n.meta != NULL)
    plugin_notification_meta_free(n.meta);

  pthread_mutex_lock(&sdbus_metric_lock);
  sdbus_metric = sdbus_metric_create();
  if (!sdbus_metric) {
    pthread_mutex_unlock(&sdbus_metric_lock);
    return -1;
  }

  DEBUG(LOG_KEY "initialize user bus");
  if (sdbus_acquire(&bus_user, TARGET_LOCAL_USER, false) == 0) {
    if (ENABLE_LATENCY) {
      DEBUG(LOG_KEY "initialize user server bus");
      if (sdbus_acquire(&bus_user_server, TARGET_LOCAL_USER, false) != 0)
        WARNING(LOG_KEY "could not connect to user server bus");
    }
    if (ENABLE_MONITOR) {
      DEBUG(LOG_KEY "initialize user monitor bus");
      if (sdbus_acquire(&bus_user_monitor, TARGET_LOCAL_USER, true) != 0)
        WARNING(LOG_KEY "could not connect to user monitor bus");
    }
  } else {
    WARNING(LOG_KEY "could not connect to user bus");
  }

  DEBUG(LOG_KEY "initialize system bus");
  if (sdbus_acquire(&bus_system, TARGET_LOCAL_SYSTEM, false) == 0) {
    if (ENABLE_LATENCY) {
      DEBUG(LOG_KEY "initialize system server bus");
      if (sdbus_acquire(&bus_system_server, TARGET_LOCAL_SYSTEM, false) != 0)
        WARNING(LOG_KEY "enabling system server failed");
    }
    if (ENABLE_MONITOR) {
      DEBUG(LOG_KEY "initialize system monitor bus");
      if (sdbus_acquire(&bus_system_monitor, TARGET_LOCAL_SYSTEM, true) != 0)
        WARNING(LOG_KEY "enabling system monitor failed");
    }
  } else {
    WARNING(LOG_KEY "could not connect to system bus");
  }

  if (ENABLE_LATENCY) {
    server_start(&user_server);
    server_start(&system_server);
  }
  if (ENABLE_MONITOR) {
    monitor_start(&system_monitor);
    monitor_start(&user_monitor);
  }

  pthread_mutex_unlock(&sdbus_metric_lock);
  return 0;
}

/* ------------------------------------------------------------------------- */
static int sdbus_shutdown(void) {
  if (ENABLE_MONITOR) {
    monitor_shutdown(&system_monitor);
    monitor_shutdown(&user_monitor);
    sdbus_close(&bus_system_monitor);
    sdbus_close(&bus_user_monitor);
  }
  if (ENABLE_LATENCY) {
    server_shutdown(&system_server);
    server_shutdown(&user_server);
    sdbus_close(&bus_system_server);
    sdbus_close(&bus_user_server);
  }
  sdbus_close(&bus_system);
  sdbus_close(&bus_user);

  sdbus_metric_destroy(&sdbus_metric);

  return 0;
}

/* ------------------------------------------------------------------------- */
void module_register(void) {
  plugin_register_complex_config(PLUGIN_KEY, sdbus_config);
  plugin_register_init(PLUGIN_KEY, sdbus_init);
  plugin_register_read(PLUGIN_KEY, sdbus_read);
  plugin_register_shutdown(PLUGIN_KEY, sdbus_shutdown);
}
