/**
 * SPDX-FileCopyrightText: 2023 MBition GmbH
 **/

#include "collectd.h"

#include "plugin.h"
#include "utils/common/common.h"

#include <stdlib.h>
#include <systemd/sd-daemon.h>

#define PLUGIN_NAME "sdnotify"
#define LOG_KEY PLUGIN_NAME " plugin: "

//------------------------------------------------------------------------------
static const char *config_keys[] = {"ReadyOnInit", "WatchdogOnRead",
                                    "WatchdogOnWrite", "StoppingOnShutdown"};

static bool ready_on_init = true;
static bool watchdog_on_read = true;
static bool watchdog_on_write = false;
static bool stopping_on_shutdown = true;

//------------------------------------------------------------------------------
static void sdnotify_printenv() {
  extern char **environ;
  for (char **env = environ; *env; ++env) {
    DEBUG(LOG_KEY "%s", *env);
  }
}

//------------------------------------------------------------------------------
static void sdnotify_disable_all() {
  ready_on_init = false;
  watchdog_on_read = false;
  watchdog_on_write = false;
  stopping_on_shutdown = false;
}

//------------------------------------------------------------------------------
static int sdnotify_watchdog_check() {

  uint64_t watchdog_usec = 0UL;
  const int watchdog_ret = sd_watchdog_enabled(0, &watchdog_usec);

  if (watchdog_ret < 0) {
    WARNING(LOG_KEY "could not fetch watchdog information (error %d), "
                    "disable notifications",
            watchdog_ret);
    sdnotify_disable_all();
    return 1;
  }

  if (watchdog_ret == 0) {
    WARNING(LOG_KEY "watchdog not enabled, disable notifications");
    sdnotify_disable_all();
    return 2;
  }

  cdtime_t watchdog_interval = US_TO_CDTIME_T(watchdog_usec);
  cdtime_t update_interval = plugin_get_interval();
  if (watchdog_interval < update_interval) {
    ERROR(LOG_KEY
          "collectd interval with %.1fs is too large (expectation: <%.1fs)",
          CDTIME_T_TO_DOUBLE(update_interval),
          CDTIME_T_TO_DOUBLE(watchdog_interval));
    sdnotify_disable_all();
    return 3;
  }

  NOTICE(LOG_KEY "collectd interval: %.1fs, watchdog expectation: <%.1fs",
         CDTIME_T_TO_DOUBLE(update_interval),
         CDTIME_T_TO_DOUBLE(watchdog_interval));

  return 0;
}

//------------------------------------------------------------------------------
static int sdnotify_send(const char *msg) {
  DEBUG(LOG_KEY "send message '%s'", msg);
  int ret = sd_notify(false, msg);
  if (ret < 0) {
    WARNING(LOG_KEY "sending message '%s' failed with %d (%s)", msg, ret,
            strerror(ret));
    return ret;
  }
  if (ret == 0) {
    WARNING(LOG_KEY "skip sending message '%s', no socket", msg);
  }
  return 0;
}

//------------------------------------------------------------------------------
static int sdnotify_config(char const *key, char const *value) {
  sdnotify_printenv();

  if (strcasecmp(key, "ReadyOnInit") == 0) {
    ready_on_init = IS_TRUE(value);
    return 0;
  } else if (strcasecmp(key, "WatchdogOnRead") == 0) {
    watchdog_on_read = IS_TRUE(value);
    return 0;
  } else if (strcasecmp(key, "WatchdogOnWrite") == 0) {
    watchdog_on_write = IS_TRUE(value);
    return 0;
  } else if (strcasecmp(key, "StoppingOnShutdown") == 0) {
    stopping_on_shutdown = IS_TRUE(value);
    return 0;
  }

  return -1;
}

//------------------------------------------------------------------------------
static int sdnotify_init(void) {

  int check = sdnotify_watchdog_check();
  if (check < 0) {
    return check;
  }

  if (ready_on_init) {
    sdnotify_send("READY=1");
  }

  return 0;
}

//------------------------------------------------------------------------------
static int sdnotify_read(void) {
  if (watchdog_on_read) {
    sdnotify_send("WATCHDOG=1");
  }
  return 0;
}

//------------------------------------------------------------------------------
static int sdnotify_write(const data_set_t *ds, const value_list_t *vl,
                          __attribute__((unused)) user_data_t *user) {
  if (watchdog_on_write) {
    sdnotify_send("WATCHDOG=1");
  }
  return 0;
}

//------------------------------------------------------------------------------
static int sdnotify_shutdown(void) {
  if (stopping_on_shutdown) {
    sdnotify_send("STOPPING=1");
  }
  return 0;
}

//------------------------------------------------------------------------------
void module_register(void) {
  plugin_register_init("sdnotify", sdnotify_init);
  plugin_register_config("sdnotify", sdnotify_config, config_keys,
                         STATIC_ARRAY_SIZE(config_keys));
  plugin_register_read("sdnotify", sdnotify_read);
  plugin_register_write("sdnotify", sdnotify_write, NULL);
  plugin_register_shutdown("sdnotify", sdnotify_shutdown);
}
