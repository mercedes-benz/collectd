/**
 * SPDX-FileCopyrightText: 2020 MBition GmbH
 **/

extern "C" {
#include "collectd.h"

#include "plugin.h"
#include "utils/common/common.h"

#include "utils/format_graphite/format_graphite.h"
#include "utils/format_json/format_json.h"
}

#if defined HAVE_DLT_DLT_H
#include "dlt/dlt.h"
#endif

#if defined HAVE_ARA_LOG_LOGGING_H
#include <iostream>
#include "ara/core/abort.h"
#include "ara/core/initialization.h"
#include "ara/core/result.h"
#include "ara/log/logging.h"
#endif

#if HAVE_REGEX_H
#include <regex.h>
#endif

#include <algorithm>
#include <cctype>
#include <string>

#define WL_BUF_SIZE 16384
#define WL_FORMAT_JSON 1
#define WL_FORMAT_GRAPHITE 2
#define WL_FORMAT_GRAPHITE_SHORT 3

#if defined HAVE_ARA_LOG_LOGGING_H
ara::log::Logger *ara_logger = nullptr;
#endif

/* ************************************************************************** */
/* constants */
/* ************************************************************************** */
#define PLUGIN_NAME "write_dlt"
#define LOG_KEY PLUGIN_NAME " plugin: "

static int wdlt_format = WL_FORMAT_GRAPHITE;
static char wdlt_appid[] = "CLTD";
static const char *wdlt_name = PLUGIN_NAME " plugin";

/* ************************************************************************** */
/* dlt context management */
/* ************************************************************************** */

#if defined HAVE_DLT_DLT_H
typedef struct wdlt_context_info_s {
  DltContext context;
  struct wdlt_context_info_s *_next;
} wdlt_context_info_t;

static DltContext *jsonContext;
static DltContext *graphiteTagContext;
static DltContext *graphiteShortContext;
static DltContext *notifyContext;
static DltContext *logContext;

static wdlt_context_info_t *wdlt_contexts;

static const size_t MAX_LINE_LEN = DLT_USER_BUF_MAX_SIZE;

#else

static const size_t MAX_LINE_LEN = 128;

#endif

/* ************************************************************************** */
/* other */
/* ************************************************************************** */

static int log_verbosity = 0;
static std::string log_target = "SERV";

/* -------------------------------------------------------------------------- */
#if defined HAVE_DLT_DLT_H
DltContext *wdlt_context_get(const char *name, const char *description) {
  DltReturnValue dlt_ret;
  wdlt_context_info_t *ci;

  if (name == NULL) {
    name = "NULL";
  }

  /* find existing context */
  for (ci = wdlt_contexts; ci != NULL; ci = ci->_next) {
    if (strncmp(ci->context.contextID, name, 4) == 0) {
      return &ci->context;
    }
  }

  /* create new context */
  ci = (wdlt_context_info_t *)calloc(1, sizeof(*ci));

  INFO("%s: register DLT context '%s' (%p)", wdlt_name, name, &ci->context);
  dlt_ret = dlt_register_context(&ci->context, name, description);
  if (dlt_ret != DLT_RETURN_OK) {
    ERROR("%s: creating DLT context '%s' failed", wdlt_name, name);
    sfree(ci);
    return NULL;
  }

  if (wdlt_contexts == NULL) {
    wdlt_contexts = ci;
  } else {
    ci->_next = wdlt_contexts;
    wdlt_contexts = ci;
  }

  return &ci->context;
}
#endif

/* -------------------------------------------------------------------------- */
#if defined HAVE_DLT_DLT_H
void wdlt_context_clear() {
  DltReturnValue dlt_ret;

  while (wdlt_contexts != NULL) {
    wdlt_context_info_t *to_delete = wdlt_contexts;
    wdlt_contexts = to_delete->_next;
    INFO("%s: unregister DLT context '%.4s' (%p)", wdlt_name,
         to_delete->context.contextID, &to_delete->context);
    dlt_ret = dlt_unregister_context(&to_delete->context);
    if (dlt_ret != DLT_RETURN_OK) {
      ERROR("%s: unregistering DLT context failed", wdlt_name);
    }
    free(to_delete);
  }
}
#endif

/* ************************************************************************** */
/* level list */
/* ************************************************************************** */

#if defined HAVE_DLT_DLT_H
typedef struct level_entry_s {
#if HAVE_REGEX_H
  regex_t re;
#endif
  DltLogLevelType dlt_level;
  struct level_entry_s *_next;
} level_entry_t;

static level_entry_t *level_list_begin = NULL;
static level_entry_t *level_list_end = NULL;
#endif

/* -------------------------------------------------------------------------- */
#if defined HAVE_DLT_DLT_H
static void wdlt_level_list_add(const char *regexp, const char *level) {
  int status;

  level_entry_t *level_entry = (level_entry_t *)malloc(sizeof(level_entry_t));
  if (level_entry == NULL) {
    ERROR("%s: level_list_add: malloc failed.", wdlt_name);
    return;
  }

#if HAVE_REGEX_H
  if (regexp != NULL) {
    status = regcomp(&level_entry->re, regexp, REG_EXTENDED | REG_NOSUB);
    if (status != 0) {
      DEBUG("%s: compiling the regular expression \"%s\" failed.", wdlt_name,
            regexp);
      regfree(&level_entry->re);
      sfree(level_entry);
      return;
    }
  }
#else
  if (regexp != NULL) {
    ERROR("%s: ps_list_register: "
          "Regular expression \"%s\" found in config "
          "file, but support for regular expressions "
          "has been disabled at compile time.",
          wdlt_name, regexp);
    sfree(level_entry);
    return;
  }
#endif

  level_entry->dlt_level = DLT_LOG_INFO;
  if (level != NULL) {
    if (strcasecmp(level, "DEFAULT") == 0) {
      level_entry->dlt_level = DLT_LOG_DEFAULT;
    } else if (strcasecmp(level, "OFF") == 0) {
      level_entry->dlt_level = DLT_LOG_OFF;
    } else if (strcasecmp(level, "FATAL") == 0) {
      level_entry->dlt_level = DLT_LOG_FATAL;
    } else if (strcasecmp(level, "ERROR") == 0) {
      level_entry->dlt_level = DLT_LOG_ERROR;
    } else if (strcasecmp(level, "WARN") == 0) {
      level_entry->dlt_level = DLT_LOG_WARN;
    } else if (strcasecmp(level, "INFO") == 0) {
      level_entry->dlt_level = DLT_LOG_INFO;
    } else if (strcasecmp(level, "DEBUG") == 0) {
      level_entry->dlt_level = DLT_LOG_DEBUG;
    } else if (strcasecmp(level, "VERBOSE") == 0) {
      level_entry->dlt_level = DLT_LOG_VERBOSE;
    }
  }

  DEBUG("%s: add DLT level match '%s' --> %s (%d)", wdlt_name, regexp, level,
        level_entry->dlt_level);
  if (level_list_begin == NULL) {
    level_list_begin = level_entry;
    level_list_end = level_entry;
  } else {
    level_list_end->_next = level_entry;
    level_list_end = level_entry;
  }
}
#endif

/* -------------------------------------------------------------------------- */
#if defined HAVE_DLT_DLT_H
static void wdlt_level_list_clear() {
  DEBUG("%s: level_list_clear: begin", wdlt_name);
  while (level_list_begin != NULL) {
    level_entry_t *level_entry_to_delete = level_list_begin;
    level_list_begin = level_entry_to_delete->_next;
    regfree(&level_entry_to_delete->re);
    free(level_entry_to_delete);
  }
  level_list_end = NULL;
}
#endif

/* -------------------------------------------------------------------------- */
#if defined HAVE_DLT_DLT_H
static DltLogLevelType wdlt_level_list_get(const char *message) {
#if HAVE_REGEX_H
  level_entry_t *me;
  for (me = level_list_begin; me != NULL; me = me->_next) {
    if (regexec(&me->re, message, 0, NULL, 0) == 0) {
      return me->dlt_level;
    }
  }
#endif
  return DLT_LOG_INFO;
}
#endif

/* ************************************************************************** */
/* context list */
/* ************************************************************************** */

#if defined HAVE_DLT_DLT_H
typedef struct context_entry_s {
#if HAVE_REGEX_H
  regex_t re;
#endif
  DltContext *dlt_context;
  struct context_entry_s *_next;
} context_entry_t;

static context_entry_t *context_list_begin = NULL;
static context_entry_t *context_list_end = NULL;
#endif

/* -------------------------------------------------------------------------- */
#if defined HAVE_DLT_DLT_H
static void wdlt_context_list_add(const char *regexp, const char *context) {
  int status;

  context_entry_t *context_entry =
      (context_entry_s *)calloc(1, sizeof(context_entry_t));
  if (context_entry == NULL) {
    ERROR("%s: context_list_add: malloc failed.", wdlt_name);
    return;
  }

#if HAVE_REGEX_H
  if (regexp != NULL) {
    status = regcomp(&context_entry->re, regexp, REG_EXTENDED | REG_NOSUB);
    if (status != 0) {
      DEBUG("%s: compiling the regular expression \"%s\" failed.", wdlt_name,
            regexp);
      regfree(&context_entry->re);
      sfree(context_entry);
      return;
    }
  }
#else
  if (regexp != NULL) {
    ERROR("%s: ps_list_register: "
          "Regular expression \"%s\" found in config "
          "file, but support for regular expressions "
          "has been disabled at compile time.",
          wdlt_name, regexp);
    sfree(context_entry);
    return;
  }
#endif

  context_entry->dlt_context = wdlt_context_get(context, "dynamic");
  DEBUG("%s: add DLT context match '%s' --> %s", wdlt_name, regexp, context);

  if (context_list_begin == NULL) {
    context_list_begin = context_entry;
    context_list_end = context_entry;
  } else {
    context_list_end->_next = context_entry;
    context_list_end = context_entry;
  }
}
#endif

/* -------------------------------------------------------------------------- */
#if defined HAVE_DLT_DLT_H
static void wdlt_context_list_clear() {
  DEBUG("%s: context_list_clear: begin", wdlt_name);
  while (context_list_begin != NULL) {
    context_entry_t *context_entry_to_delete = context_list_begin;
    context_list_begin = context_entry_to_delete->_next;
    regfree(&context_entry_to_delete->re);
    free(context_entry_to_delete);
  }
  context_list_end = NULL;
}
#endif

/* -------------------------------------------------------------------------- */
#if defined HAVE_DLT_DLT_H
static DltContext *wdlt_context_list_get(const char *message, DltContext *def) {
#if HAVE_REGEX_H
  context_entry_t *me;
  for (me = context_list_begin; me != NULL; me = me->_next) {
    if (regexec(&me->re, message, 0, NULL, 0) == 0) {
      return me->dlt_context;
    }
  }
#endif
  return def;
}
#endif

/* ************************************************************************** */
/* output functions */
/* ************************************************************************** */

/* -------------------------------------------------------------------------- */
static int wdlt_write_graphite(char *buffer, size_t buffer_size,
                               const data_set_t *ds, const value_list_t *vl,
                               unsigned int flags) {
  int status;

  status = format_graphite(buffer, buffer_size, ds, vl, NULL, NULL, '_', flags);
  if (status != 0)
    return status;

  return 0;
}

/* -------------------------------------------------------------------------- */
static int wdlt_write_json(char *buffer, size_t buffer_size,
                           const data_set_t *ds, const value_list_t *vl) {
  size_t bfree = buffer_size;
  size_t bfill = 0;

  format_json_initialize(buffer, &bfill, &bfree);
  format_json_value_list(buffer, &bfill, &bfree, ds, vl, 0);
  format_json_finalize(buffer, &bfill, &bfree);

  return 0;
}

/* -------------------------------------------------------------------------- */
static int wdlt_write(const data_set_t *ds, const value_list_t *vl,
                      __attribute__((unused)) user_data_t *user_data) {
  char buffer[WL_BUF_SIZE] = {0};
  int status = 0;

  if (0 != strcmp(ds->type, vl->type)) {
    ERROR("%s: DS type does not match value list type", wdlt_name);
    return -1;
  }

  switch (wdlt_format) {
  case WL_FORMAT_GRAPHITE:
    wdlt_write_graphite(buffer, sizeof(buffer), ds, vl,
                        GRAPHITE_USE_TAGS | GRAPHITE_ALWAYS_APPEND_DS);
    break;
  case WL_FORMAT_GRAPHITE_SHORT:
    wdlt_write_graphite(buffer, sizeof(buffer), ds, vl,
                        GRAPHITE_SEPARATE_INSTANCES);
    break;
  case WL_FORMAT_JSON:
    status = wdlt_write_json(buffer, sizeof(buffer), ds, vl);
    break;
  }

  if (0 != status)
    return status;

#if defined HAVE_DLT_DLT_H
  DltContext *ctx = jsonContext;
  switch (wdlt_format) {
  case WL_FORMAT_GRAPHITE:
    ctx = graphiteTagContext;
    break;
  case WL_FORMAT_GRAPHITE_SHORT:
    ctx = graphiteShortContext;
    break;
  }

  DltLogLevelType dlt_level = wdlt_level_list_get(buffer);
  DltContext *dlt_context = wdlt_context_list_get(buffer, ctx);
  if (dlt_context != NULL) {
    DLT_LOG(*dlt_context, dlt_level, DLT_STRING(buffer));
  }
#endif

#if defined HAVE_ARA_LOG_LOGGING_H
  if (ara_logger != nullptr) {
    ara_logger->LogInfo() << std::string(buffer);
  }
#endif

  return status;
}

/* ************************************************************************** */
/* configuration */
/* ************************************************************************** */

/* -------------------------------------------------------------------------- */
static int wg_config_dlt(oconfig_item_t *ci) {
  for (int i = 0; i < ci->children_num; i++) {
    oconfig_item_t *child = ci->children + i;

    if (strcasecmp("AppID", child->key) == 0) {
      bzero(wdlt_appid, 4);
      cf_util_get_string_buffer(child, wdlt_appid, sizeof(wdlt_appid));
    } else if (strcasecmp("MatchLevel", child->key) == 0) {
      if ((child->values_num != 2) ||
          (OCONFIG_TYPE_STRING != child->values[0].type) ||
          (OCONFIG_TYPE_STRING != child->values[1].type)) {
        ERROR("%s: `'MatchLevel' needs exactly two string arguments (got %i).",
              wdlt_name, child->values_num);
        continue;
      }
#if defined HAVE_DLT_DLT_H
      wdlt_level_list_add(child->values[0].value.string,
                          child->values[1].value.string);
#endif
    } else if (strcasecmp("MatchContext", child->key) == 0) {
      if ((child->values_num != 2) ||
          (OCONFIG_TYPE_STRING != child->values[0].type) ||
          (OCONFIG_TYPE_STRING != child->values[1].type)) {
        ERROR(
            "%s: `'MatchContext' needs exactly two string arguments (got %i).",
            wdlt_name, child->values_num);
        continue;
      }
#if defined HAVE_DLT_DLT_H
      wdlt_context_list_add(child->values[0].value.string,
                            child->values[1].value.string);
#endif
    } else {
      ERROR("%s: Invalid configuration option in <DLT>: `%s'.", wdlt_name,
            child->key);
      return -EINVAL;
    }
  }
  return 0;
}

/* -------------------------------------------------------------------------- */
static int wdlt_config(oconfig_item_t *ci) {
  bool format_seen = false;

  for (int i = 0; i < ci->children_num; i++) {
    oconfig_item_t *child = ci->children + i;

    if (strcasecmp("DLT", child->key) == 0) {
      if (wg_config_dlt(child) == 0)
        continue;
      else
        /* error message bufpos by child function */
        return -EINVAL;
    }

    else if (strcasecmp("Format", child->key) == 0) {
      char str[16];

      if (cf_util_get_string_buffer(child, str, sizeof(str)) != 0)
        continue;

      if (format_seen) {
        WARNING("%s: Redefining option `%s'.", wdlt_name, child->key);
      }
      format_seen = true;

      if (strcasecmp("Graphite", str) == 0)
        wdlt_format = WL_FORMAT_GRAPHITE;
      else if (strcasecmp("GraphiteShort", str) == 0)
        wdlt_format = WL_FORMAT_GRAPHITE_SHORT;
      else if (strcasecmp("JSON", str) == 0)
        wdlt_format = WL_FORMAT_JSON;
      else {
        ERROR("%s: Unknown format `%s' for option `%s'.", wdlt_name, str,
              child->key);
        return -EINVAL;
      }
    } else if (strcasecmp("LogLevel", child->key) == 0) {
      char value[16];

      if (cf_util_get_string_buffer(child, value, sizeof(value)) != 0)
        continue;

      int verbosity = parse_log_severity(value);
      if (verbosity < 0) {
        ERROR("%s: Unknown format `%s' for option `%s'.", wdlt_name, value,
              child->key);
        return -EINVAL;
      }

      log_verbosity = verbosity;
    } else if (strcasecmp("LogTarget", child->key) == 0) {
      char raw_value[16];
      if (cf_util_get_string_buffer(child, raw_value, sizeof(raw_value)) != 0)
        continue;
      const std::string value = raw_value;
      if (value.empty() ||
          std::any_of(value.begin(), value.end(),
                      [](unsigned char c) { return !std::isalnum(c); })) {
        ERROR("%s: Invalid value for option `%s': `%s'.", wdlt_name, child->key,
              value.c_str());
        return -EINVAL;
      }

      log_target = value;
    } else {
      ERROR("%s: Invalid configuration option: `%s'.", wdlt_name, child->key);
      return -EINVAL;
    }
  }

  return 0;
}

/* -------------------------------------------------------------------------- */
static int wdlt_init() {
  INFO(LOG_KEY "register app with '%s'.", wdlt_appid);

#if defined HAVE_DLT_DLT_H
  DLT_REGISTER_APP(wdlt_appid, "Collectd service");

  jsonContext = wdlt_context_get("JSON", "Metrics in JSON format");
  graphiteTagContext =
      wdlt_context_get("GRPH", "Metrics in Tagged Graphite Format");
  graphiteShortContext =
      wdlt_context_get("GRSH", "Metrics in Short Graphite Format");
  notifyContext = wdlt_context_get("NTFY", "Informational messages");
  logContext = wdlt_context_get(log_target.c_str(), "Service log");

  DLT_LOG(
      *notifyContext, DLT_LOG_INFO,
      DLT_STRING("NTFY context is for informational messages. Format is not "
                 "necessarily machine-readable and is subject to change."));
  DLT_LOG(*notifyContext, DLT_LOG_INFO, DLT_STRING("collectd version: daemon"),
          DLT_STRING(plugin_get_daemon_version()));
  DLT_LOG(
      *notifyContext, DLT_LOG_INFO,
      DLT_STRING("collectd version: plugin " PLUGIN_NAME " " PACKAGE_VERSION));
  DLT_LOG(*notifyContext, DLT_LOG_INFO, DLT_STRING("maximal line length: "),
          DLT_INT(MAX_LINE_LEN));
#endif

#if defined HAVE_ARA_LOG_LOGGING_H
  ara::core::Result<void> init_result{ara::core::Initialize()};

  if (!init_result.HasValue()) {
    char const *msg{"ara::core::Initialize() failed."};
    std::cerr << msg << "\nResult contains: " << init_result.Error().Message()
              << ", " << init_result.Error().UserMessage() << "\n";

    ara::core::Abort(msg);
  } else {
    ara_logger = &(ara::log::CreateLogger(
        ara::core::StringView(wdlt_appid),
        ara::core::StringView("Measurement and trace daemon")));
  }
#endif

  return 0;
}

/* -------------------------------------------------------------------------- */
static int wdlt_shutdown() {
  INFO(LOG_KEY "Stopping logging to DLT");
  plugin_unregister_log(PLUGIN_NAME);

#if defined HAVE_DLT_DLT_H
  wdlt_level_list_clear();
  wdlt_context_list_clear();
  wdlt_context_clear();
#endif

  INFO(LOG_KEY "unregister app with '%s'.", wdlt_appid);

#if defined HAVE_DLT_DLT_H
  DLT_UNREGISTER_APP();
#endif

#if defined HAVE_ARA_LOG_LOGGING_H
  ara::core::Result<void> deinit_result{ara::core::Deinitialize()};

  if (!deinit_result.HasValue()) {
    char const *msg{"ara::core::Deinitialize() failed."};
    std::cerr << msg << "\nResult contains: " << deinit_result.Error().Message()
              << ", " << deinit_result.Error().UserMessage() << "\n";
    ara::core::Abort(msg);
  }
#endif

  return 0;
}

/* -------------------------------------------------------------------------- */
static void wdlt_log(int verbosity, const char *msg,
                     user_data_t __attribute__((unused)) * user_data) {

  if (verbosity > log_verbosity) {
    return;
  }

  DltLogLevelType dlt_log_level = DLT_LOG_DEFAULT;

  if (verbosity <= LOG_ERR) {
    dlt_log_level = DLT_LOG_ERROR;
  } else if (verbosity == LOG_WARNING) {
    dlt_log_level = DLT_LOG_WARN;
  } else if (verbosity == LOG_NOTICE || verbosity == LOG_INFO) {
    dlt_log_level = DLT_LOG_INFO;
  } else if (verbosity >= LOG_DEBUG) {
    dlt_log_level = DLT_LOG_DEBUG;
  }

  DLT_LOG(*logContext, dlt_log_level, DLT_STRING(msg));
}

/*--------------------------------------------------------------------------- */
static int wdlt_notify(const notification_t *notif, user_data_t *userdata) {
#if !defined HAVE_ARA_LOG_LOGGING_H
  DltLogLevelType log_level = DLT_LOG_FATAL;
  switch (notif->severity) {
  case NOTIF_FAILURE:
    log_level = DLT_LOG_ERROR;
    break;
  case NOTIF_WARNING:
    log_level = DLT_LOG_WARN;
    break;
  case NOTIF_OKAY:
    log_level = DLT_LOG_INFO;
    break;
  }

  // create full message, take care of maximal message length

  char buf[WL_BUF_SIZE];
  const int bufsize = (int)sizeof(buf);
  const char *sep = ": ";

  int bufpos = snprintf(buf, bufsize, "%s", notif->message);
  if (bufpos < bufsize) {
    for (notification_meta_t *meta = notif->meta; meta != NULL;
         meta = meta->next) {
      char *bufptr = buf + bufpos;
      int buffree = bufsize - bufpos;

      int written;
      if (meta->name[0]) {
        written = snprintf(bufptr, buffree, "%s%s=", sep, meta->name);
      } else {
        written = snprintf(bufptr, buffree, "%s", sep);
      }
      if (written >= buffree) {
        bufpos += written;
        break;
      }
      bufpos += written;
      bufptr = buf + bufpos;
      buffree = bufsize - bufpos;
      switch (meta->type) {
      case NM_TYPE_STRING:
        written = snprintf(bufptr, buffree, "%s", meta->nm_value.nm_string);
        break;
      case NM_TYPE_SIGNED_INT:
        written =
            snprintf(bufptr, buffree, "%" PRIi64, meta->nm_value.nm_signed_int);
        break;
      case NM_TYPE_UNSIGNED_INT:
        written = snprintf(bufptr, buffree, "%" PRIu64,
                           meta->nm_value.nm_unsigned_int);
        break;
      case NM_TYPE_DOUBLE:
        written =
            snprintf(bufptr, buffree, GAUGE_FORMAT, meta->nm_value.nm_double);
        break;
      case NM_TYPE_BOOLEAN:
        written = snprintf(bufptr, buffree, "%s",
                           meta->nm_value.nm_boolean ? "true" : "false");
        break;
      }
      bufpos += written;
      sep = "; ";
    }
  }

  if (bufpos >= bufsize) {
    for (int co = 2; co <= 4; ++co) {
      buf[bufsize - co] = '.';
    }
  }

  // write splitted message

  const char *contbuf = ">>>";
  const size_t contsize = strlen(contbuf);

  char outbuf[MAX_LINE_LEN + 1];
  const int outsize = sizeof(outbuf);
  const int cont_outsize = outsize - contsize;

  int remaining = bufpos;
  const char *inptr = buf;

  while (remaining > 0) {
    if (remaining <= outsize) {
      remaining = 0;
      DLT_LOG(*notifyContext, log_level, DLT_STRING(inptr));
    } else {
      memcpy(outbuf, inptr, cont_outsize);
      memcpy(outbuf + cont_outsize - 1, contbuf, contsize);
      outbuf[outsize - 1] = '\0';
      inptr += cont_outsize - 1;
      remaining -= cont_outsize - 1;
      DLT_LOG(*notifyContext, log_level, DLT_STRING(outbuf));
    }
  }

#endif
  return 0;
}

/* -------------------------------------------------------------------------- */
void module_register(void) {
  plugin_register_complex_config(PLUGIN_NAME, wdlt_config);
  plugin_register_log(PLUGIN_NAME, wdlt_log, NULL);
  plugin_register_write(PLUGIN_NAME, wdlt_write, NULL);
  plugin_register_notification(PLUGIN_NAME, wdlt_notify, NULL);
  plugin_register_init(PLUGIN_NAME, wdlt_init);
  plugin_register_shutdown(PLUGIN_NAME, wdlt_shutdown);
}
