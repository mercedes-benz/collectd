/**
 * The MIT License
 *
 * Copyright (C) 2023 MBition GmbH
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
 *
 * Rewritten version of `cgroup` plugin.
 * Features:
 *   - support of Cgroups version 2
 * Credits to the authors of `cgroup.c` for their inspiration.
 */

#include "collectd.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <unistd.h>

#include "plugin.h"
#include "utils/common/common.h"
#include "utils/mount/mount.h"
#include "utils_cache.h"

#define PLUGIN_NAME "cgroups2"
#define LOG_KEY PLUGIN_NAME " plugin: "
#define CG2_MAX(a, b) (((a) > (b)) ? (a) : (b))
#define CGROUPS2_TRACE 0

#if CGROUPS2_TRACE
#define TRACE(...) plugin_log(LOG_DEBUG, LOG_KEY __VA_ARGS__)
#else              /* COLLECT_DEBUG */
#define TRACE(...) /* noop */
#endif             /* ! COLLECT_DEBUG */

typedef enum {
  CG2_CGROUP_VERSION_AUTO = 0,
  CG2_CGROUP_VERSION_1 = 1,
  CG2_CGROUP_VERSION_2 = 2
} cg2_cgroup_version_t;
typedef enum {
  CG2_MEMORY_UNIT_AUTO = 0,
  CG2_MEMORY_UNIT_KB = 1,
  CG2_MEMORY_UNIT_MB = 2,
  CG2_MEMORY_UNIT_GB = 3,
} cg2_memory_unit_t;
typedef enum {
  USAGE,
  PEAK,
  LIMIT,
  EVENTS_HIGH,
  EVENTS_MAX,
  EVENTS_FAIL,
} cg2_swap_init_t;

static int HERTZ = 0;
static const char *SLICE_SUFFIX = ".slice";
static cu_mount_t *mnt_list = NULL;
static const data_set_t *cg2_ds_cputime = NULL;
static const data_set_t *cg2_ds_cpu_pressure = NULL;
static const data_set_t *cg2_ds_mem_pressure = NULL;
static const data_set_t *cg2_ds_io_pressure = NULL;

static const char *cg2_keys[] = {
    "CGroupVersion", "CpuPressure", "MemPressure",      "IOPressure",
    "MemoryUnit",    "MaxLevel",    "SortAlphabetical", "ProcessCount",
    "ThreadCount",   "MountCache",  "SwapMemory",
};
static int cg2_keys_num = STATIC_ARRAY_SIZE(cg2_keys);
static cg2_cgroup_version_t cg2_cgroup_version = CG2_CGROUP_VERSION_AUTO;
static bool cg2_cpu_pressure_enabled = false;
static bool cg2_mem_pressure_enabled = false;
static bool cg2_io_pressure_enabled = false;
static bool cg2_swap_enabled = false;
static bool cg2_swap_available = false;
static const char *cg2_memory_unit_str[] = {"", "kB", "MB", "GB"};
static gauge_t memory_swap[6];
static cg2_memory_unit_t cg2_memory_unit = CG2_MEMORY_UNIT_AUTO;
static const unsigned int CG2_FIRST_LEVEL = 0;
static unsigned int cg2_max_level = UINT_MAX;
static bool cg2_sort_alphabetical = false;
const gauge_t NO_LIMIT = 0x7FFFFFFFFFFFF000;
const gauge_t UNKNOWN_SIZE = -42;
const gauge_t MAX = 0x7FFFFFFFFFFFFF00;
static bool cg2_proc_count = false;
static bool cg2_thread_count = false;
static bool cg2_mount_cache = true;

//==============================================================================
// Format
//==============================================================================

//------------------------------------------------------------------------------
static void cg2_pretty_size(char *buf, size_t sz, gauge_t bytes) {
  if (bytes == NO_LIMIT) {
    sstrncpy(buf, "NO_LIMIT", sz);
    return;
  }
  if (bytes == UNKNOWN_SIZE) {
    sstrncpy(buf, "UNKNOWN", sz);
    return;
  }

  if (bytes == MAX) {
    sstrncpy(buf, "max", sz);
    return;
  }

  switch (cg2_memory_unit) {
  case CG2_MEMORY_UNIT_KB:
  case CG2_MEMORY_UNIT_MB:
  case CG2_MEMORY_UNIT_GB:
    ssnprintf(buf, sz, "%.0f",
              (float)roundf(bytes / (1lu << (10 * cg2_memory_unit))));
    break;
  case CG2_MEMORY_UNIT_AUTO:
    if (bytes < 0) {
      sstrncpy(buf, "?", sz);
    } else if (bytes > 1024.0 * 1024 * 1024 * 1024) {
      ssnprintf(buf, sz, ">1TB");
    } else if (bytes >= 1024 * 1024 * 1024) {
      ssnprintf(buf, sz, "%.1fGB", (float)bytes / 1024 / 1024 / 1024);
    } else if (bytes >= 1024 * 1024) {
      ssnprintf(buf, sz, "%.1fMB", (float)bytes / 1024 / 1024);
    } else if (bytes >= 1024) {
      ssnprintf(buf, sz, "%.1fKB", (float)bytes / 1024);
    } else {
      ssnprintf(buf, sz, "%.0f", bytes);
    }
  }
}

//------------------------------------------------------------------------------
// Assumption: given name contains a file name. It will be removed.

static char *cg2_format_cgroup(const char *cgroup, char *res, size_t res_len) {
  size_t res_co = 0;
  char *last_sep = NULL;

  if (!cgroup || !*cgroup || (cgroup[1] == '\0') || (cgroup[1] == '\n')) {
    sstrncpy(res, "__", res_len);
  } else {
    const char *start = cgroup;
    if (*start == '/')
      ++start;
    const char *point = NULL;
    for (const char *ch = start; *ch; ++ch) {

      switch (*ch) {
      case '/':
        last_sep = res + res_co;
        res[res_co++] = '_';
        res[res_co++] = '_';
        if (point == NULL)
          point = ch;
        size_t size = point - start;
        if (strncmp(start, ch + 1, point - start) == 0) {
          if (ch[size + 1] == '-')
            ch += size + 1;
        }
        start = ch + 1;
        point = NULL;
        break;
      case '.':
        point = ch;
        if (strncmp(ch + 1, "slice", 5) == 0)
          ch += 5;
        else if (strncmp(ch + 1, "scope", 5) == 0)
          ch += 5;
        else if (strncmp(ch + 1, "service", 7) == 0) {
          ch += 7;
          // go back until upper entry
          for (; res_co > 1; --res_co) {
            if ((res[res_co - 1] == '_') && (res[res_co - 2] == '_')) {
              res_co -= 2;
              break;
            }
          }
        } else
          res[res_co++] = '.';
        break;
      case '\\':
        if (strncmp(ch + 1, "x2d", 3) == 0) {
          ch += 3;
          res[res_co++] = '-';
        }
        break;
      case ':':
        res[res_co++] = '_';
        break;
      case '\n':
        break;
      default:
        res[res_co++] = *ch;
        point = NULL;
        break;
      }
      if (res_co >= res_len - 1)
        break;
    }
    res[res_co] = '\0';
  }

  if (last_sep == NULL) {
    sstrncpy(res, "__", res_len);
  } else {
    *last_sep = '\0';
  }

  DEBUG("cgroup format (int): '%s' --> '%s'", cgroup, res);
  return res;
}
//------------------------------------------------------------------------------
static char *cg2_format_cgroup_for_notification(const char *cgroup, char *res,
                                                size_t res_len) {
  size_t res_co = 0;

  const char *start = cgroup;
  for (const char *ch = start; *ch; ++ch) {
    switch (*ch) {
    case '_':
      if (*(ch + 1) == '_') {
        res[res_co++] = '/';
        ++ch;
        break;
      }
    default:
      res[res_co++] = *ch;
      break;
    }
    if (res_co >= res_len - 1)
      break;
  }
  res[res_co] = '\0';

  DEBUG("cgroup format (ext): '%s' --> '%s'", cgroup, res);
  return res;
}

//==============================================================================
// CPU usage info
//==============================================================================

//------------------------------------------------------------------------------
typedef struct cg2_cpu_usage_entry_s {
  char cgroup[256];
  double perc_now;
  double perc_avg;
  long proc_count;
  long thread_count;
} cg2_cpu_usage_entry_t;

typedef struct cg2_cpu_usage_info_s {
  int version;
  int entries_num;
  cg2_cpu_usage_entry_t entries[1024];
} cg2_cpu_usage_info_t;

//------------------------------------------------------------------------------
bool cg2_is_part_of(cg2_cpu_usage_entry_t *entry, const char *cgroup) {
  const char *ecn = entry->cgroup;
  size_t ecl = strlen(ecn);

  // root includes all cgroups
  if ((ecl == 2) && (ecn[0] == '_') && (ecn[1] == '_'))
    return true;

  if (strncmp(ecn, cgroup, ecl) != 0)
    return false;

  if (cgroup[ecl] == '\0')
    return true;

  if ((cgroup[ecl] == '_') && (cgroup[++ecl] == '_'))
    return true;

  return false;
}

//------------------------------------------------------------------------------
int cg2_cpu_usage_info_init(cg2_cpu_usage_info_t *info) {
  info->version = 0;
  info->entries_num = 0;
  return 0;
}

//------------------------------------------------------------------------------
int cg2_cpu_usage_info_add(cg2_cpu_usage_info_t *info, const char *cgroup,
                           double perc_now, double perc_avg) {
  if (info->entries_num >= STATIC_ARRAY_SIZE(info->entries)) {
    WARNING(LOG_KEY "cpu usage info: reached maximum entries %lu",
            STATIC_ARRAY_SIZE(info->entries));
    return -1;
  }

  cg2_cpu_usage_entry_t *entry = info->entries + info->entries_num;
  sstrncpy(entry->cgroup, cgroup, sizeof(entry->cgroup));
  entry->perc_now = perc_now;
  entry->perc_avg = perc_avg;
  entry->proc_count = -1;
  entry->thread_count = -1;
  ++info->entries_num;

  return 0;
}

//------------------------------------------------------------------------------
int cg2_sort_cpu_info_sort(const void *a, const void *b) {
  cg2_cpu_usage_entry_t *e1 = (cg2_cpu_usage_entry_t *)a;
  cg2_cpu_usage_entry_t *e2 = (cg2_cpu_usage_entry_t *)b;
  if (cg2_sort_alphabetical) {
    return strcmp(e1->cgroup, e2->cgroup);
  }
  return (int)strlen(e1->cgroup) - (int)strlen(e2->cgroup);
}

//------------------------------------------------------------------------------
int cg2_cpu_usage_info_notify(cg2_cpu_usage_info_t *info) {
  char buf[256];

  if (info->entries_num <= 0)
    return 0;

  qsort(info->entries, info->entries_num, sizeof(cg2_cpu_usage_entry_t),
        cg2_sort_cpu_info_sort);

  cdtime_t now = cdtime();
  notification_t n = {NOTIF_OKAY, now,         "", "",  PLUGIN_NAME,
                      "",         "cpu-usage", "", NULL};
  ssnprintf(n.message, sizeof(n.message),
            "cgroup cpu usage (v%d) - now%%(avg%%)", info->version);

  if (cg2_proc_count) {
    char suffix[] = "/prc";
    strncat(n.message, suffix, sizeof(n.message) - 1 - strlen(suffix));
  }

  if (cg2_thread_count) {
    char suffix[] = "/thr";
    strncat(n.message, suffix, sizeof(n.message) - 1 - strlen(suffix));
  }

  for (int co = 0; co < info->entries_num; ++co) {
    cg2_cpu_usage_entry_t *e = &info->entries[co];
    DEBUG(LOG_KEY "cg2_cpu_usage_info_notify():   co=%d, cgroup=%s", co,
          e->cgroup);
    char cgroup[256];
    cg2_format_cgroup_for_notification(e->cgroup, cgroup, sizeof(cgroup));
    ssnprintf(buf, sizeof(buf), "%.1f(%.1f)", e->perc_now, e->perc_avg);

    if (cg2_proc_count) {
      size_t n = strlen(buf);
      ssnprintf(buf + n, sizeof(buf) - n, "/%ld", e->proc_count);
    }

    if (cg2_thread_count) {
      size_t n = strlen(buf);
      ssnprintf(buf + n, sizeof(buf) - n, "/%ld", e->thread_count);
    }

    if (plugin_notification_meta_add_string(&n, cgroup, buf) != 0) {
      ERROR(LOG_KEY "adding meta data to notification failed");
      return -1;
    }
  }

  plugin_dispatch_notification(&n);
  if (n.meta != NULL)
    plugin_notification_meta_free(n.meta);

  return 0;
}

//==============================================================================
// CPU pressure info
//==============================================================================

//------------------------------------------------------------------------------
typedef struct cg2_cpu_pressure_entry_s {
  char cgroup[256];
  double some_pressure;
  double some_pressure_avg;
} cg2_cpu_pressure_entry_t;

typedef struct cg2_pressure_info_s {
  int version;
  int entries_num;
  cg2_cpu_pressure_entry_t entries[256];
} cg2_pressure_info_t;

int cg2_pressure_info_init(cg2_pressure_info_t *info) {
  info->version = 0;
  info->entries_num = 0;
  return 0;
}

//------------------------------------------------------------------------------
int cg2_pressure_info_add(cg2_pressure_info_t *info, const char *cgroup,
                          double some_pressure, double some_pressure_avg) {

  if (info->entries_num >= STATIC_ARRAY_SIZE(info->entries)) {
    WARNING(LOG_KEY "cpu pressure info: reached maximum entries %lu",
            STATIC_ARRAY_SIZE(info->entries));
    return -1;
  }

  cg2_cpu_pressure_entry_t *entry = info->entries + info->entries_num;
  sstrncpy(entry->cgroup, cgroup, sizeof(entry->cgroup));
  entry->some_pressure = some_pressure;
  entry->some_pressure_avg = some_pressure_avg;
  ++info->entries_num;

  return 0;
}

//------------------------------------------------------------------------------
int cg2_sort_cpu_pressure_info_sort(const void *a, const void *b) {
  cg2_cpu_pressure_entry_t *e1 = (cg2_cpu_pressure_entry_t *)a;
  cg2_cpu_pressure_entry_t *e2 = (cg2_cpu_pressure_entry_t *)b;
  if (cg2_sort_alphabetical) {
    return strcmp(e1->cgroup, e2->cgroup);
  }
  return (int)strlen(e1->cgroup) - (int)strlen(e2->cgroup);
}

//------------------------------------------------------------------------------
int cg2_pressure_info_notify(cg2_pressure_info_t *info, const char *file_name) {

  if (info->entries_num <= 0)
    return 0;

  qsort(info->entries, info->entries_num, sizeof(cg2_cpu_pressure_entry_t),
        cg2_sort_cpu_pressure_info_sort);

  cdtime_t now = cdtime();
  notification_t n = {NOTIF_OKAY, now,        "", "",  PLUGIN_NAME,
                      "",         "pressure", "", NULL};

  ssnprintf(n.message, sizeof(n.message),
            "cgroup %s pressure (v%d) - now(avg)%%", file_name, info->version);

  for (int co = 0; co < info->entries_num; ++co) {
    cg2_cpu_pressure_entry_t *e = info->entries + co;

    char pressure[256] = {0};
    size_t pressure_n = 0;
    pressure_n +=
        ssnprintf(pressure, sizeof(pressure), "%.2f", e->some_pressure);
    ssnprintf(pressure + pressure_n, sizeof(pressure) - pressure_n, "(%.2f)",
              e->some_pressure_avg);

    char cgroup[256];
    cg2_format_cgroup_for_notification(e->cgroup, cgroup, sizeof(e->cgroup));

    if (plugin_notification_meta_add_string(&n, cgroup, pressure) != 0) {
      ERROR(LOG_KEY "adding meta data to notification failed");
      return -1;
    }
  }

  plugin_dispatch_notification(&n);
  if (n.meta != NULL)
    plugin_notification_meta_free(n.meta);

  return 0;
}

//==============================================================================
// memory usage info
//==============================================================================

//------------------------------------------------------------------------------
gauge_t cg2_parse_size(int dirfd, char const *file_name) {
  int filefd = openat(dirfd, file_name, O_RDONLY);
  TRACE("cg2_parse_size(%d, '%s'): openat() --> %d", dirfd, file_name, filefd);
  if (filefd == -1) {
    return UNKNOWN_SIZE;
  }

  char buffer[256];
  FILE *fh = fdopen(filefd, "r");
  TRACE("cg2_parse_size(%d, '%s'): fdopen(%d) --> %p", dirfd, file_name, filefd,
        fh);
  if (fh == NULL)
    return UNKNOWN_SIZE;

  if (fgets(buffer, sizeof(buffer), fh) == NULL) {
    TRACE("cg2_parse_size(%d, '%s'): fclose(%p), no content", dirfd, file_name,
          fh);
    if (fclose(fh)) {
      ERROR(LOG_KEY "fclose('%s') failed: %s", file_name, STRERRNO);
    };
    return UNKNOWN_SIZE;
  }

  TRACE("cg2_parse_size(%d, '%s'): fclose(%p)", dirfd, file_name, fh);
  if (fclose(fh)) {
    ERROR(LOG_KEY "fclose('%s') failed: %s", file_name, STRERRNO);
  };

  if (strncmp(buffer, "max", 3) == 0) {
    if (cg2_swap_enabled)
      return MAX;
    else
      return NO_LIMIT;
  }

  value_t v;
  if (parse_value(buffer, &v, DS_TYPE_GAUGE) != 0) {
    return UNKNOWN_SIZE;
  }

  return v.gauge;
}

//------------------------------------------------------------------------------
typedef struct cg2_mem_usage_entry_s {
  char cgroup[256];
  gauge_t usage;
  gauge_t peak;
  gauge_t limit;
} cg2_mem_usage_entry_t;

typedef struct cg2_mem_usage_info_s {
  int version;
  int entries_num;
  cg2_mem_usage_entry_t entries[256];
} cg2_mem_usage_info_t;

//------------------------------------------------------------------------------
int cg2_mem_usage_info_init(cg2_mem_usage_info_t *info) {
  info->version = 0;
  info->entries_num = 0;
  return 0;
}

//------------------------------------------------------------------------------
int cg2_mem_usage_info_add(cg2_mem_usage_info_t *info, const char *cgroup,
                           gauge_t usage, gauge_t peak, gauge_t limit) {
  if (info->entries_num >= STATIC_ARRAY_SIZE(info->entries)) {
    WARNING(LOG_KEY "mem usage info: reached maximum entries %lu",
            STATIC_ARRAY_SIZE(info->entries));
    return -1;
  }
  cg2_mem_usage_entry_t *entry = info->entries + info->entries_num;
  sstrncpy(entry->cgroup, cgroup, sizeof(entry->cgroup));
  entry->usage = usage;
  entry->peak = peak;
  entry->limit = limit;
  ++info->entries_num;

  return 0;
}

//------------------------------------------------------------------------------
int cg2_sort_mem_info_sort(const void *a, const void *b) {
  cg2_mem_usage_entry_t *e1 = (cg2_mem_usage_entry_t *)a;
  cg2_mem_usage_entry_t *e2 = (cg2_mem_usage_entry_t *)b;
  if (cg2_sort_alphabetical) {
    return strcmp(e1->cgroup, e2->cgroup);
  }
  return (int)strlen(e1->cgroup) - (int)strlen(e2->cgroup);
}

//------------------------------------------------------------------------------
int cg2_mem_usage_info_notify(cg2_mem_usage_info_t *info) {
  char buf[256];

  if (info->entries_num <= 0)
    return 0;

  qsort(info->entries, info->entries_num, sizeof(cg2_mem_usage_entry_t),
        cg2_sort_mem_info_sort);

  cdtime_t now = cdtime();
  notification_t n = {NOTIF_OKAY, now,         "", "",  PLUGIN_NAME,
                      "",         "cpu-usage", "", NULL};
  ssnprintf(n.message, sizeof(n.message),
            "cgroup memory usage (v%d) - usage/peak/limit", info->version);

  if (cg2_memory_unit != CG2_MEMORY_UNIT_AUTO) {
    const size_t n_msg_len = strlen(n.message);
    ssnprintf(n.message + n_msg_len, sizeof n.message - n_msg_len - 1, " (%s)",
              cg2_memory_unit_str[cg2_memory_unit]);
  }

  for (int co = 0; co < info->entries_num; ++co) {
    cg2_mem_usage_entry_t *e = info->entries + co;
    char cgroup[256];
    char usage_buf[64];
    char peak_buf[64];
    char limit_buf[64];

    cg2_format_cgroup_for_notification(e->cgroup, cgroup, sizeof(e->cgroup));
    cg2_pretty_size(usage_buf, sizeof(usage_buf), e->usage);
    cg2_pretty_size(peak_buf, sizeof(peak_buf), e->peak);
    cg2_pretty_size(limit_buf, sizeof(limit_buf), e->limit);

    ssnprintf(buf, sizeof(buf), "%s/%s/%s", usage_buf, peak_buf, limit_buf);

    if (plugin_notification_meta_add_string(&n, cgroup, buf) != 0) {
      ERROR(LOG_KEY "adding meta data to notification failed");
      return -1;
    }
  }

  plugin_dispatch_notification(&n);
  if (n.meta != NULL)
    plugin_notification_meta_free(n.meta);

  return 0;
}

// -----------------------------------------------------------------------------
typedef struct cg2_swap_usage_entry_s {
  char cgroup[256];
  gauge_t usage;
  gauge_t peak;
  gauge_t limit;
  gauge_t events_high;
  gauge_t events_max;
  gauge_t events_fail;
} cg2_swap_usage_entry_t;

typedef struct cg2_swap_usage_info_s {
  int version;
  int entries_num;
  cg2_swap_usage_entry_t entries[256];
} cg2_swap_usage_info_t;

// -----------------------------------------------------------------------------
int cg2_swap_usage_info_init(cg2_swap_usage_info_t *info) {
  info->version = 0;
  info->entries_num = 0;
  return 0;
}

//------------------------------------------------------------------------------
int cg2_swap_usage_info_add(cg2_swap_usage_info_t *info, const char *cgroup,
                            gauge_t memory_swap[]) {
  if (info->entries_num >= STATIC_ARRAY_SIZE(info->entries)) {
    WARNING(LOG_KEY "swap usage info: reached maximum entries %lu",
            STATIC_ARRAY_SIZE(info->entries));
    return -1;
  }
  cg2_swap_usage_entry_t *entry = info->entries + info->entries_num;
  sstrncpy(entry->cgroup, cgroup, sizeof(entry->cgroup));
  entry->usage = memory_swap[USAGE];
  entry->peak = memory_swap[PEAK];
  entry->limit = memory_swap[LIMIT];
  entry->events_high = memory_swap[EVENTS_HIGH];
  entry->events_max = memory_swap[EVENTS_MAX];
  entry->events_fail = memory_swap[EVENTS_FAIL];
  ++info->entries_num;

  return 0;
}

//------------------------------------------------------------------------------
int cg2_sort_swap_info_sort(const void *a, const void *b) {
  cg2_swap_usage_entry_t *e1 = (cg2_swap_usage_entry_t *)a;
  cg2_swap_usage_entry_t *e2 = (cg2_swap_usage_entry_t *)b;
  if (cg2_sort_alphabetical) {
    return strcmp(e1->cgroup, e2->cgroup);
  }
  return (int)strlen(e1->cgroup) - (int)strlen(e2->cgroup);
}

//------------------------------------------------------------------------------
int cg2_swap_usage_info_notify(cg2_swap_usage_info_t *info) {
  char buf[256];

  if (info->entries_num <= 0)
    return 0;

  qsort(info->entries, info->entries_num, sizeof(cg2_swap_usage_entry_t),
        cg2_sort_swap_info_sort);

  cdtime_t now = cdtime();
  notification_t n = {NOTIF_OKAY, now,          "", "",  PLUGIN_NAME,
                      "",         "swap-usage", "", NULL};
  ssnprintf(n.message, sizeof(n.message),
            "cgroup memory swap usage (v%d) - "
            "usage/high/limit/eventshigh/eventsmax/eventsfail",
            info->version);

  if (cg2_memory_unit != CG2_MEMORY_UNIT_AUTO) {
    const size_t n_msg_len = strlen(n.message);
    ssnprintf(n.message + n_msg_len, sizeof n.message - n_msg_len - 1, " (%s)",
              cg2_memory_unit_str[cg2_memory_unit]);
  }

  for (int co = 0; co < info->entries_num; ++co) {
    cg2_swap_usage_entry_t *e = info->entries + co;
    char cgroup[256];
    char usage_buf[64];
    char peak_buf[64];
    char limit_buf[64];

    cg2_format_cgroup_for_notification(e->cgroup, cgroup, sizeof(e->cgroup));

    cg2_pretty_size(usage_buf, sizeof(usage_buf), e->usage);
    cg2_pretty_size(peak_buf, sizeof(peak_buf), e->peak);
    cg2_pretty_size(limit_buf, sizeof(limit_buf), e->limit);

    ssnprintf(buf, sizeof(buf), "%s/%s/%s/%0.lf/%0.lf/%0.lf", usage_buf,
              peak_buf, limit_buf, e->events_high, e->events_max,
              e->events_fail);

    if (plugin_notification_meta_add_string(&n, cgroup, buf) != 0) {
      ERROR(LOG_KEY "adding meta data to notification failed");
      return -1;
    }
  }

  plugin_dispatch_notification(&n);
  if (n.meta != NULL)
    plugin_notification_meta_free(n.meta);

  return 0;
}
//==============================================================================
// System utils
//==============================================================================

//------------------------------------------------------------------------------
static cdtime_t cg2_get_uptime() {
  struct sysinfo info;

  if (sysinfo(&info) != 0) {
    ERROR("uptime plugin: Error calling sysinfo: %s", STRERRNO);
    return -1;
  }

  return TIME_T_TO_CDTIME_T(info.uptime);
}

//==============================================================================
// I/O utils
//==============================================================================

//------------------------------------------------------------------------------
int cg2_ends_with(const char *input, const char *expected_suffix) {
  // no pattern, never a fit
  if (expected_suffix == NULL)
    return 0;
  size_t slen = strlen(input);
  size_t tlen = strlen(expected_suffix);
  if (tlen > slen)
    return 1;
  const char *input_suffix = input + slen - tlen;
  return strcmp(input_suffix, expected_suffix);
}

//------------------------------------------------------------------------------
typedef int (*cg2_file_callback)(int dirfd, const char *dir_name,
                                 const char *file_name, void *user_data);

//------------------------------------------------------------------------------
static int cg2_handle_files_recurse(DIR *parent, const char *dir_pattern,
                                    const char *dir_name, const char *file_name,
                                    cg2_file_callback callback, void *user_data,
                                    int level) {
  struct dirent *ent;
  struct stat sb;
  int success = 0;
  int failure = 0;
  int status = 0;
  bool max_level_reached = (level >= 0) && (level >= cg2_max_level);
  int next_level = (level >= 0) ? ++level : level;
  char subdirname[PATH_MAX];

  DEBUG(LOG_KEY
        "cg2_handle_files_recurse(dir='%s', pattern='%s', file='%s', level=%d)",
        dir_name, dir_pattern, file_name, level);

  if (!parent) {
    return 0;
  }

  int parent_fd = dirfd(parent);
  if (parent_fd < 0) {
    WARNING(LOG_KEY "dirfd()in handle_files_recursive(%s) "
                    "failed with '%s'",
            dir_name, STRERRNO);
    return -1;
  }

  // don't go too far down in directory tree
  if (!max_level_reached) {
    errno = 0;
    while ((ent = readdir(parent)) != NULL) {
      // check for directories only
      if (ent->d_type != DT_DIR) {
        continue;
      }
      // skip special directories
      const char *dname = ent->d_name;
      if (dname[0] == '\0')
        continue;
      if (dname[0] == '.') {
        if (dname[1] == '\0')
          continue;
        if ((dname[1] == '.') && (dname[2] == '\0'))
          continue;
      }

      // allow only directories with given suffix for recursion
      if (cg2_ends_with(dname, dir_pattern) != 0)
        continue;

      int fd = openat(parent_fd, dname, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
      TRACE("cg2_handle_files_recurse('%s', '%s', '%s', %d): openat(%d, '%s) "
            "--> %d",
            dir_name, dir_pattern, file_name, level, parent_fd, dname, fd);

      if (fd != -1) { /* Directory */
        DIR *child = fdopendir(fd);
        TRACE("cg2_handle_files_recurse('%s', '%s', '%s', %d): fdopendir(%d) "
              "--> %p",
              dir_name, dir_pattern, file_name, level, fd, child);
        if (child != NULL) {
          if (dir_name == NULL) {
            status =
                cg2_handle_files_recurse(child, dir_pattern, ".", file_name,
                                         callback, user_data, next_level);
          } else {
            snprintf(subdirname, sizeof(subdirname), "%s%s/", dir_name,
                     ent->d_name);
            status = cg2_handle_files_recurse(child, dir_pattern, subdirname,
                                              file_name, callback, user_data,
                                              next_level);
          }

          TRACE("cg2_handle_files_recurse('%s', '%s', '%s', %d): closedir(%p) ",
                dir_name, dir_pattern, file_name, level, child);
          if (closedir(child)) {
            ERROR(LOG_KEY "closedir(%d) failed: %s", fd, STRERRNO);
          };
        } else {
          WARNING(LOG_KEY "fdopendir(%s) in handle_files_recursive(%s) "
                          "failed with '%s'",
                  dname, dir_name, STRERRNO);
        }
        TRACE("cg2_handle_files_recurse('%s', '%s', '%s', %d): close(%d) ",
              dir_name, dir_pattern, file_name, level, fd);
      } else {
        WARNING(LOG_KEY "handle files recursive(), openat failed with '%s', "
                        "dir=%s, file=%s, file descriptor=%d",
                STRERRNO, dir_name, file_name, parent_fd);
      }
      if (status >= 0)
        success += status;
      else
        failure++;
    }
  }

  // try handle the requested file
  if (fstatat(parent_fd, file_name, &sb, AT_SYMLINK_NOFOLLOW) == 0) {
    if ((sb.st_mode & S_IFMT) == S_IFREG) {
      status = (*callback)(parent_fd, dir_name, file_name, user_data);
      if (status >= 0) {
        success++;
      } else {
        WARNING(LOG_KEY "handling '%s' in '%s' failed", file_name, dir_name);
        failure++;
      }
    } else {
      WARNING(LOG_KEY "%s/%s is not a regular file", dir_name, file_name);
      failure++;
    }
  }

  if ((success == 0) && (failure > 0))
    return -1;

  return success;
}

//------------------------------------------------------------------------------
static int cg2_handle_files(const char *dir_name, const char *dir_pattern,
                            const char *file_name, cg2_file_callback callback,
                            void *user_data, int level) {
  DIR *dir = opendir(dir_name);
  if (dir == NULL) {
    ERROR(LOG_KEY "handle files(), cannot open '%s': %s", dir_name, STRERRNO);
    return -1;
  }

  DEBUG(LOG_KEY "cg2_handle_files(dir='%s', pattern='%s', file='%s', level=%d)",
        dir_name, dir_pattern, file_name, level);
  int status = cg2_handle_files_recurse(dir, dir_pattern, "", file_name,
                                        callback, user_data, level);
  if (closedir(dir)) {
    ERROR(LOG_KEY "closedir(\"%s\") failed: %s", dir_name, STRERRNO);
  }

  return status;
}

//==============================================================================
// Value submission
//==============================================================================

//------------------------------------------------------------------------------
static void cg2_submit_cpu_usage(char const *cgroup, derive_t user_counter,
                                 derive_t system_counter,
                                 cg2_cpu_usage_info_t *cpu_usage) {
  // do not submit invalid values
  if ((user_counter < 0) || (system_counter < 0))
    return;

  uint64_t interval_all = CDTIME_T_TO_US(cg2_get_uptime());
  uint64_t interval_now = CDTIME_T_TO_US(plugin_get_interval());

  value_t values[3];
  value_list_t vl = {.values = values, .meta = NULL};

  sstrncpy(vl.host, hostname_g, sizeof(vl.host));
  sstrncpy(vl.plugin, PLUGIN_NAME, sizeof(vl.plugin));
  sstrncpy(vl.plugin_instance, cgroup, sizeof(vl.plugin_instance));

  // first calculate the consumed cpu before updating
  sstrncpy(vl.type, "cg_cputime", sizeof(vl.type));
  sstrncpy(vl.type_instance, "", sizeof(vl.type_instance));

  double perc_user_now = 0.0;
  double perc_system_now = 0.0;
  double perc_total_now = 0.0;

  value_t *last = uc_get_value(cg2_ds_cputime, &vl);
  if (last != NULL) {
    DEBUG(LOG_KEY "submit values for %s: %ld(%ld), %ld(%ld)", cgroup,
          user_counter, last[0].derive, system_counter, last[1].derive);
    if ((last[0].derive > 0.0) || (last[1].derive > 0.0)) {
      derive_t user_now = user_counter - last[0].derive;
      derive_t system_now = system_counter - last[1].derive;

      perc_user_now = 100.0 * user_now / interval_now;
      perc_system_now = 100.0 * system_now / interval_now;
      perc_total_now = perc_user_now + perc_system_now;
    }
    free(last);
  }

  // update raw counter values
  vl.values_len = 2;
  values[0].derive = user_counter;
  values[1].derive = system_counter;
  plugin_dispatch_values(&vl);

  // write average cpu usage in percent
  sstrncpy(vl.type, "cg_cpu_percent", sizeof(vl.type));
  sstrncpy(vl.type_instance, "avg", sizeof(vl.type_instance));

  double perc_user_avg = 100.0 * user_counter / interval_all;
  double perc_system_avg = 100.0 * system_counter / interval_all;
  double perc_total_avg = perc_user_avg + perc_system_avg;

  vl.values_len = 3;
  values[0].gauge = perc_user_avg;
  values[1].gauge = perc_system_avg;
  values[2].gauge = perc_total_avg;

  plugin_dispatch_values(&vl);

  // write current cpu usage in percent if available
  sstrncpy(vl.type_instance, "now", sizeof(vl.type_instance));

  vl.values_len = 3;
  values[0].gauge = perc_user_now;
  values[1].gauge = perc_system_now;
  values[2].gauge = perc_total_now;

  plugin_dispatch_values(&vl);

  cg2_cpu_usage_info_add(cpu_usage, cgroup, perc_total_now, perc_total_avg);

  DEBUG(LOG_KEY "%s: cpu=%.1f(%.1f), usr=%.1f(%.1f), "
                "sys=%.1f(%.1f)",
        cgroup, perc_user_now + perc_system_now,
        perc_user_avg + perc_system_avg, perc_user_now, perc_user_avg,
        perc_system_now, perc_system_avg);
}

//------------------------------------------------------------------------------
static void cg2_submit_mem_usage(char const *cgroup, gauge_t usage,
                                 gauge_t peak, gauge_t limit,
                                 cg2_mem_usage_info_t *mem_usage) {
  // do not submit invalid values
  if (usage < 0)
    return;

  value_t values[3];
  value_list_t vl = {.values = values, .meta = NULL};

  sstrncpy(vl.host, hostname_g, sizeof(vl.host));
  sstrncpy(vl.plugin, PLUGIN_NAME, sizeof(vl.plugin));
  sstrncpy(vl.plugin_instance, cgroup, sizeof(vl.plugin_instance));
  sstrncpy(vl.type, "cg_memory", sizeof(vl.type));
  sstrncpy(vl.type_instance, "", sizeof(vl.type_instance));

  // write raw memoy values
  vl.values_len = 3;
  values[0].gauge = usage;
  values[1].gauge = peak;
  values[2].gauge = limit;
  plugin_dispatch_values(&vl);

  cg2_mem_usage_info_add(mem_usage, cgroup, usage, peak, limit);
}

//------------------------------------------------------------------------------
static void cg2_submit_pressure(char const *cgroup, gauge_t pressure_avg10,
                                cg2_pressure_info_t *user_data,
                                const char *file_name) {
  cg2_pressure_info_t *pressure = user_data;
  if (pressure_avg10 < 0)
    return;

  value_t values[2];
  value_list_t vl = {.values = values, .meta = NULL};
  value_t *last = NULL;

  sstrncpy(vl.host, hostname_g, sizeof(vl.host));
  sstrncpy(vl.plugin, PLUGIN_NAME, sizeof(vl.plugin));
  sstrncpy(vl.plugin_instance, cgroup, sizeof(vl.plugin_instance));
  strncpy(vl.type, "cg_pressure", sizeof(vl.type));

  if (!strncmp(file_name, "cpu", 3)) {
    sstrncpy(vl.type_instance, "cpu", sizeof(vl.type_instance));
    last = uc_get_value(cg2_ds_cpu_pressure, &vl);
  } else if (!strncmp(file_name, "memory", 6)) {
    sstrncpy(vl.type_instance, "memory", sizeof(vl.type_instance));
    last = uc_get_value(cg2_ds_mem_pressure, &vl);
  } else {
    sstrncpy(vl.type_instance, "io", sizeof(vl.type_instance));
    last = uc_get_value(cg2_ds_io_pressure, &vl);
  }

  double some_pressure = 0;
  double some_pressure_avg = 0;
  some_pressure = pressure_avg10;
  if (last) {
    some_pressure_avg = (pressure_avg10 + last[1].gauge) / 2;
    DEBUG(LOG_KEY "pressure: calculated %s. "
                  "avg10: (old:%.lf new:%.lf). "
                  "running average:(old:%lf  new:%lf)",
          cgroup, last[0].gauge, pressure_avg10, last[1].gauge,
          some_pressure_avg);
    free(last);
  } else {
    some_pressure_avg = some_pressure;
  }

  vl.values_len = 2;
  values[0].gauge = some_pressure;
  values[1].gauge = some_pressure_avg;
  plugin_dispatch_values(&vl);

  cg2_pressure_info_add(pressure, cgroup, some_pressure, some_pressure_avg);
}

//==============================================================================
// data handler
//==============================================================================

//------------------------------------------------------------------------------
int cg2_handle_files_log(int dirfd, const char *dir_name, const char *file_name,
                         void *user_data) {
  DEBUG(LOG_KEY "found %s in %s", file_name, dir_name);
  return 0;
}

//------------------------------------------------------------------------------
static int cg2_handle_cgroup_procs(int dirfd, const char *dir_name,
                                   const char *file_name, void *user_data) {

  DEBUG(LOG_KEY "cg2_handle_cgroup_procs(%s) in %s", file_name, dir_name);

  int filefd = openat(dirfd, file_name, O_RDONLY);
  TRACE("cg2_handle_cgroup_procs(%d, '%s', '%s', %p): openat() --> %d", dirfd,
        dir_name, file_name, user_data, filefd);
  if (filefd == -1) {
    WARNING(LOG_KEY "could not open file '%s' in '%s'", file_name, dir_name);
    return -1;
  }

  FILE *fh = fdopen(filefd, "r");
  DEBUG(LOG_KEY
        "cg2_handle_cgroup_procs(%d, '%s', '%s', %p): fdopen(%d) --> %p ",
        dirfd, dir_name, file_name, user_data, filefd, fh);
  if (fh == NULL) {
    ERROR(LOG_KEY "fopen (\"%s\") failed: %s", file_name, STRERRNO);
    return -1;
  }

  long proc_count = 0;
  while (!feof(fh)) {
    if (fgetc(fh) == '\n') {
      proc_count++;
    }
  }

  TRACE("cg2_handle_cgroup_procs(%d, '%s', '%s', %p): fclose(%p)", dirfd,
        dir_name, file_name, user_data, fh);
  if (fclose(fh)) {
    ERROR(LOG_KEY "fclose('%s') failed: %s", file_name, STRERRNO);
  };

  char cgroup[PATH_MAX];
  cg2_format_cgroup(dir_name, cgroup, sizeof(cgroup));

  DEBUG(LOG_KEY
        "cg2_handle_cgroup_procs(%s): found %ld processes in %s, cgroup %s",
        file_name, proc_count, dir_name, cgroup);

  cg2_cpu_usage_info_t *cpu_usage = (cg2_cpu_usage_info_t *)user_data;
  for (size_t i = 0; i < cpu_usage->entries_num; i++) {
    cg2_cpu_usage_entry_t *e = &cpu_usage->entries[i];
    if (cg2_is_part_of(e, cgroup)) {
      DEBUG(LOG_KEY "proc_count added to %s:", e->cgroup);
      e->proc_count = CG2_MAX(e->proc_count, 0) + proc_count;
    }
  }

  return 0;
}

//------------------------------------------------------------------------------
static int cg2_handle_cgroup_threads(int dirfd, const char *dir_name,
                                     const char *file_name, void *user_data) {
  int filefd = openat(dirfd, file_name, O_RDONLY);
  TRACE("cg2_handle_cgroup_threads(%d, '%s', '%s', %p): openat() --> %d", dirfd,
        dir_name, file_name, user_data, filefd);
  if (filefd == -1) {
    WARNING(LOG_KEY "could not open file '%s' in '%s'", file_name, dir_name);
    return -1;
  }

  FILE *fh = fdopen(filefd, "r");
  TRACE("cg2_handle_cgroup_threads(%d, '%s', '%s', %p): fdopen(%d) --> %p ",
        dirfd, dir_name, file_name, user_data, filefd, fh);
  if (fh == NULL) {
    ERROR(LOG_KEY "fopen ('%s') failed: %s", file_name, STRERRNO);
    return -1;
  }

  long thread_count = 0;
  while (!feof(fh)) {
    if (fgetc(fh) == '\n') {
      thread_count++;
    }
  }
  TRACE("cg2_handle_cgroup_threads(%d, '%s', '%s', %p): fclose(%p)", dirfd,
        dir_name, file_name, user_data, fh);
  if (fclose(fh)) {
    ERROR(LOG_KEY "fclose('%s') failed: %s", file_name, STRERRNO);
  };

  char cgroup[PATH_MAX];
  cg2_format_cgroup(dir_name, cgroup, sizeof(cgroup));

  DEBUG(LOG_KEY "thread_count '%s':  %ld:", cgroup, thread_count);

  cg2_cpu_usage_info_t *cpu_usage = (cg2_cpu_usage_info_t *)user_data;
  for (size_t i = 0; i < cpu_usage->entries_num; i++) {
    cg2_cpu_usage_entry_t *e = &cpu_usage->entries[i];
    if (cg2_is_part_of(e, cgroup)) {
      DEBUG(LOG_KEY "thread_count added to %s:", e->cgroup);
      e->thread_count = CG2_MAX(e->thread_count, 0) + thread_count;
    }
  }

  return 0;
}

//------------------------------------------------------------------------------
static int cg2_handle_v1_cpu_stat(int dirfd, const char *dir_name,
                                  const char *file_name, void *user_data) {
  int filefd = openat(dirfd, file_name, O_RDONLY);
  TRACE("cg2_handle_v1_cpu_stat(%d, '%s', '%s', %p): openat() --> %d", dirfd,
        dir_name, file_name, user_data, filefd);
  if (filefd == -1) {
    WARNING(LOG_KEY "could not open file '%s' in '%s'", file_name, dir_name);
    return -1;
  }

  FILE *fh = fdopen(filefd, "r");
  TRACE("cg2_handle_v1_cpu_stat(%d, '%s', '%s', %p): fdopen(%d) --> %p ", dirfd,
        dir_name, file_name, user_data, filefd, fh);
  if (fh == NULL) {
    ERROR(LOG_KEY "fdopen (\"%s\") failed: %s", file_name, STRERRNO);
    return -1;
  }

  derive_t user_counter = -1;
  derive_t system_counter = -1;
  char buf[1024];

  while (fgets(buf, sizeof(buf), fh) != NULL) {
    char *fields[3];

    int field_num = strsplit(buf, fields, STATIC_ARRAY_SIZE(fields));
    if (field_num != 2) {
      ERROR(LOG_KEY "wrong number of fields in %s: %s, %d != %d", file_name,
            buf, field_num, 2);
      continue;
    }

    DEBUG(LOG_KEY "cg2_handle_v1_cpu_stat(): %d fields, '%s', %s", field_num,
          fields[0], fields[1]);

    if (strcmp(fields[0], "user") == 0) {
      user_counter = atoll(fields[1]) * 1000000 / HERTZ;
      if (system_counter >= 0)
        break;
    } else if (strcmp(fields[0], "system") == 0) {
      system_counter = atoll(fields[1]) * 1000000 / HERTZ;
      if (user_counter >= 0)
        break;
    }
  }
  TRACE("cg2_handle_v1_cpu_stat(%d, '%s', '%s', %p): fclose(%p)", dirfd,
        dir_name, file_name, user_data, fh);
  if (fclose(fh)) {
    ERROR(LOG_KEY "fclose('%s') failed: %s", file_name, STRERRNO);
  };

  DEBUG(LOG_KEY "cg2_handle_v1_cpu_stat(): %ld, %ld", user_counter,
        system_counter);

  cg2_cpu_usage_info_t *cpu_usage = (cg2_cpu_usage_info_t *)user_data;
  cg2_format_cgroup(dir_name, buf, sizeof(buf));
  cg2_submit_cpu_usage(buf, user_counter, system_counter, cpu_usage);

  return 0;
}

//------------------------------------------------------------------------------
static int cg2_handle_v2_cpu_stat(int dirfd, const char *dir_name,
                                  const char *file_name, void *user_data) {
  int filefd = openat(dirfd, file_name, O_RDONLY);
  TRACE("cg2_handle_v2_cpu_stat(%d, '%s', '%s', %p): openat() --> %d", dirfd,
        dir_name, file_name, user_data, filefd);
  if (filefd == -1) {
    WARNING(LOG_KEY "could not open file '%s' in '%s'", file_name, dir_name);
    return -1;
  }

  FILE *fh = fdopen(filefd, "r");
  TRACE("cg2_handle_v2_cpu_stat(%d, '%s', '%s', %p): fdopen(%d) --> %p ", dirfd,
        dir_name, file_name, user_data, filefd, fh);
  if (fh == NULL) {
    ERROR(LOG_KEY "fdopen (\"%s\") failed: %s", file_name, STRERRNO);
    return -1;
  }

  derive_t user_counter = -1;
  derive_t system_counter = -1;
  char buf[1024];
  while (fgets(buf, sizeof(buf), fh) != NULL) {
    char *fields[3];

    if (strsplit(buf, fields, STATIC_ARRAY_SIZE(fields)) != 2)
      continue;

    if (strcmp(fields[0], "user_usec") == 0) {
      user_counter = atoll(fields[1]);
      if (system_counter >= 0)
        break;
    } else if (strcmp(fields[0], "system_usec") == 0) {
      system_counter = atoll(fields[1]);
      if (user_counter >= 0)
        break;
    }
  }
  TRACE("cg2_handle_v2_cpu_stat(%d, '%s', '%s', %p): fclose(%p)", dirfd,
        dir_name, file_name, user_data, fh);
  if (fclose(fh)) {
    ERROR(LOG_KEY "fclose('%s') failed: %s", file_name, STRERRNO);
  };

  cg2_format_cgroup(dir_name, buf, sizeof(buf));
  cg2_submit_cpu_usage(buf, user_counter, system_counter, user_data);

  return 0;
}

//------------------------------------------------------------------------------
static int cg2_handle_v1_mem_stat(int dirfd, const char *dir_name,
                                  const char *file_name, void *user_data) {
  gauge_t usage = cg2_parse_size(dirfd, file_name);
  gauge_t peak = cg2_parse_size(dirfd, "memory.max_usage_in_bytes");
  gauge_t limit = cg2_parse_size(dirfd, "memory.limit_in_bytes");

  char buf[1024];
  cg2_format_cgroup(dir_name, buf, sizeof(buf));
  cg2_submit_mem_usage(buf, usage, peak, limit, user_data);

  return 0;
}

//------------------------------------------------------------------------------
static int cg2_handle_v2_mem_stat(int dirfd, const char *dir_name,
                                  const char *file_name, void *user_data) {

  gauge_t usage = cg2_parse_size(dirfd, file_name);
  gauge_t peak = cg2_parse_size(dirfd, "memory.peak");
  gauge_t limit = cg2_parse_size(dirfd, "memory.max");

  char buf[1024];
  cg2_format_cgroup(dir_name, buf, sizeof(buf));
  cg2_submit_mem_usage(buf, usage, peak, limit, user_data);

  return 0;
}

//------------------------------------------------------------------------------
static int cg2_handle_pressure(int dirfd, const char *dir_name,
                               const char *file_name, void *user_data) {

  int filefd = openat(dirfd, file_name, O_RDONLY);
  TRACE("cg2_handle_pressure(%d, '%s', '%s', %p): openat() --> %d", dirfd,
        dir_name, file_name, user_data, filefd);
  if (filefd == -1) {
    WARNING(LOG_KEY "could not open file '%s' in '%s'", file_name, dir_name);
    return -1;
  }

  FILE *fh = fdopen(filefd, "r");
  TRACE("cg2_handle_pressure(%d, '%s', '%s', %p): fdopen(%d) --> %p ", dirfd,
        dir_name, file_name, user_data, filefd, fh);
  if (fh == NULL) {
    ERROR(LOG_KEY "pressure: fdopen (\"%s\") failed: %s", file_name, STRERRNO);
    return -1;
  }

  gauge_t pressure_avg10 = -1;
  char buf[1024];
  if (fgets(buf, sizeof(buf), fh) != NULL) {
    const char *LINEBEGIN = "some", *VALUEAFTER = "avg10=";
    if (strncmp(buf, LINEBEGIN, strlen(LINEBEGIN)) == 0) {
      char *pos = strstr(buf, VALUEAFTER);
      if (pos) {
        pos += strlen(VALUEAFTER);
        pressure_avg10 = atof(pos);
#if COLLECT_DEBUG
        int posn = strpbrk(pos, " ") - pos;
        DEBUG(LOG_KEY "pressure: read '%s': %d %s -> %lf", file_name, posn, pos,
              pressure_avg10);
#endif
      }
    }
  }
  TRACE("cg2_handle_pressure(%d, '%s', '%s', %p): fclose(%p)", dirfd, dir_name,
        file_name, user_data, fh);
  if (fclose(fh)) {
    ERROR(LOG_KEY "fclose('%s') failed: %s", file_name, STRERRNO);
  };

  cg2_format_cgroup(dir_name, buf, sizeof(buf));
  cg2_submit_pressure(buf, pressure_avg10, user_data, file_name);

  return 0;
}

// -----------------------------------------------------------------------------
// set flag if swap is available
static bool is_swap_available() {
  FILE *fh;
  char buffer[1024];
  char *fields[10];
  int numfields;
  bool availability = false;

  if ((fh = fopen("/proc/meminfo", "r")) == NULL) {
    WARNING(LOG_KEY "  fopen: %s", STRERRNO);
    return false;
  }
  while (fgets(buffer, sizeof(buffer), fh) != NULL) {
    if (strncasecmp(buffer, "SwapTotal:", 10) == 0) {
      numfields = strsplit(buffer, fields, STATIC_ARRAY_SIZE(fields));
      if ((numfields >= 2) && (atof(fields[1]) > 0)) {
        availability = true;
        break;
      }
    }
  }
  if (fclose(fh)) {
    WARNING(LOG_KEY " fclose: %s", STRERRNO);
    return false;
  }

  INFO(LOG_KEY " swap is %savailable", availability ? "" : "not ");
  return availability;
}

// -----------------------------------------------------------------------------
static int cg2_handle_v2_swap_stat(int dirfd, const char *dir_name,
                                   const char *file_name, void *user_data) {

  memset(memory_swap, 0, sizeof(memory_swap));
  memory_swap[USAGE] = cg2_parse_size(dirfd, file_name);
  memory_swap[PEAK] = cg2_parse_size(dirfd, "memory.swap.high");
  memory_swap[LIMIT] = cg2_parse_size(dirfd, "memory.swap.max");

  const char *file_name_events = "memory.swap.events";
  int filefd = openat(dirfd, file_name_events, O_RDONLY);
  if (filefd == -1) {
    ERROR("cg2_handle_v2_swap_stat, failed to open a file(%d, '%s')", dirfd,
          file_name_events);
    return -1;
  }

  char buffer[256];
  FILE *fh = fdopen(filefd, "r");
  if (fh == NULL) {
    ERROR("cg2_handle_v2_swap_stat, failed to open a filestream(%s)",
          file_name_events);
    return -1;
  }

  while (fgets(buffer, sizeof(buffer), fh) != NULL) {
    char *fields[3];

    int field_num = strsplit(buffer, fields, STATIC_ARRAY_SIZE(fields));
    if (field_num != 2) {
      ERROR(LOG_KEY "wrong number of fields in %s: %s, %d != %d", file_name,
            buffer, field_num, 2);
      continue;
    }
    if (strncmp(fields[0], "high", 4) == 0) {
      memory_swap[EVENTS_HIGH] = (gauge_t)strtod(fields[1], NULL);
    } else if (strncmp(fields[0], "max", 3) == 0) {
      memory_swap[EVENTS_MAX] = (gauge_t)strtod(fields[1], NULL);
    } else if (strncmp(fields[0], "fail", 4) == 0) {
      memory_swap[EVENTS_FAIL] = (gauge_t)strtod(fields[1], NULL);
    }
  }

  if (fclose(fh)) {
    ERROR(LOG_KEY "fclose('%s') failed: %s", file_name, STRERRNO);
  };

  char buf[1024];
  cg2_format_cgroup(dir_name, buf, sizeof(buf));
  cg2_swap_usage_info_add(user_data, buf, memory_swap);

  return 0;
}

//==============================================================================
// collectd hooks
//==============================================================================

static int cg2_config(const char *key, const char *value) {
  if (strcasecmp(key, "CGroupVersion") == 0) {
    if (strchr("012", *value) == NULL) {
      ERROR("The `%s' option only allows 0, 1 or 2, given: '%s'", key, value);
      return -1;
    }
    cg2_cgroup_version = *value - '0';
    return 0;
  } else if (strcasecmp(key, "CpuPressure") == 0) {
    cg2_cpu_pressure_enabled = IS_TRUE(value) ? true : false;
    return 0;
  } else if (strcasecmp(key, "MemPressure") == 0) {
    cg2_mem_pressure_enabled = IS_TRUE(value) ? true : false;
    return 0;
  } else if (strcasecmp(key, "IOPressure") == 0) {
    cg2_io_pressure_enabled = IS_TRUE(value) ? true : false;
    return 0;
  } else if (strcasecmp(key, "SwapMemory") == 0) {
    cg2_swap_enabled = IS_TRUE(value) ? true : false;
    return 0;
  } else if (strcasecmp(key, "MemoryUnit") == 0) {
    for (size_t i = 0; i < sizeof(cg2_memory_unit_str); ++i) {
      if (strcasecmp(value, cg2_memory_unit_str[i]) == 0) {
        cg2_memory_unit = i;
        return 0;
      }
    }
    ERROR(LOG_KEY "invalid configuration value for '%s': '%s'", key, value);
    return -1;
  } else if (strcasecmp(key, "MaxLevel") == 0) {
    char *end;
    double valuef = strtod(value, &end);
    if (*end != '\0' || valuef > UINT_MAX || trunc(valuef) != valuef) {
      ERROR(LOG_KEY "invalid configuration value for '%s': '%s'", key, value);
      return -1;
    }
    cg2_max_level = valuef;
    return 0;
  } else if (strcasecmp(key, "SortAlphabetical") == 0) {
    cg2_sort_alphabetical = IS_TRUE(value) ? true : false;
    return 0;
  } else if (strcasecmp(key, "ProcessCount") == 0) {
    cg2_proc_count = IS_TRUE(value) ? true : false;
    return 0;
  } else if (strcasecmp(key, "ThreadCount") == 0) {
    cg2_thread_count = IS_TRUE(value) ? true : false;
    return 0;
  } else if (strcasecmp(key, "MountCache") == 0) {
    cg2_mount_cache = IS_TRUE(value) ? true : false;
    return 0;
  } else {
    return -1;
  }
}

//------------------------------------------------------------------------------
static int cg2_init(void) {

  char buf[2048];

  notification_t n = {NOTIF_OKAY, cdtime(), "", "",  PLUGIN_NAME,
                      "",         "",       "", NULL};
  ssnprintf(n.message, NOTIF_MAX_MSG_LEN, "collectd version: plugin %s %s",
            PLUGIN_NAME, PACKAGE_VERSION);
  plugin_dispatch_notification(&n);
  if (n.meta != NULL)
    plugin_notification_meta_free(n.meta);

  HERTZ = sysconf(_SC_CLK_TCK);
  if (HERTZ <= 0)
    HERTZ = HZ;

  cg2_ds_cputime = plugin_get_ds("cg_cputime");
  if (cg2_ds_cputime == NULL) {
    ERROR(LOG_KEY "couldn't find type 'cg_cputime' in database");
    return -1;
  } else {
    DEBUG(LOG_KEY "found 'cg_cputime' with %u values",
          (unsigned)cg2_ds_cputime->ds_num);
  }

  cg2_ds_cpu_pressure = plugin_get_ds("cg_pressure");
  if (cg2_ds_cpu_pressure == NULL) {
    ERROR(LOG_KEY "couldn't find type 'cg_pressure' in database");
    return -1;
  } else {
    cg2_ds_mem_pressure = cg2_ds_cpu_pressure;
    cg2_ds_io_pressure = cg2_ds_cpu_pressure;
    DEBUG(LOG_KEY "found 'cg_pressure' with %u values",
          (unsigned)cg2_ds_cpu_pressure->ds_num);
  }

  // log cgroup versions in use, possible controllers:
  //   cpuset, cpu, cpuacct, blkio, memory, devices, freezer, net_cls
  //   perf_event, net_prio, hugetlb, pids, rdma, misc

  FILE *fh = fopen("/proc/cgroups", "r");
  if (fh == NULL) {
    ERROR(LOG_KEY "fopen (\"/proc/cgroups\") failed: %s", STRERRNO);
    return -1;
  }

  while (fgets(buf, sizeof(buf), fh) != NULL) {
    char *fields[2];

    if (*buf == '#')
      continue;
    int fields_num = strsplit(buf, fields, STATIC_ARRAY_SIZE(fields));
    if (fields_num != STATIC_ARRAY_SIZE(fields)) {
      ERROR(LOG_KEY "found only %d of %lu in /proc/cgroups in '%s'", fields_num,
            STATIC_ARRAY_SIZE(fields), buf);
      return -1;
    }
    if (*fields[1] == '0') {
      DEBUG(LOG_KEY "found v2 controller >%s<", fields[0]);
    } else {
      DEBUG(LOG_KEY "found v1 controller >%s<", fields[0]);
    }
  }
  if (fclose(fh)) {
    ERROR(LOG_KEY "fclose('/proc/cgroups') failed: %s", STRERRNO);
  };

  if (cg2_swap_enabled)
    cg2_swap_available = is_swap_available();

  return 0;
}

//------------------------------------------------------------------------------
static int cg2_read(void) {
  bool cgroup_cpu_found = false;
  bool cgroup_mem_found = false;
  bool cgroup_cpu_pressure_found = false;
  bool cgroup_mem_pressure_found = false;
  bool cgroup_io_pressure_found = false;
  bool cgroup_swap_found = false;

  if (mnt_list == NULL) {
    if (cu_mount_getlist(&mnt_list) == NULL) {
      ERROR(LOG_KEY "cu_mount_getlist failed.");
      return -1;
    }
  }

  cg2_cpu_usage_info_t cpu_usage;
  cg2_cpu_usage_info_init(&cpu_usage);

  cg2_mem_usage_info_t mem_usage;
  cg2_mem_usage_info_init(&mem_usage);

  cg2_pressure_info_t cpu_pressure;
  cg2_pressure_info_init(&cpu_pressure);

  cg2_pressure_info_t memory_pressure;
  cg2_pressure_info_init(&memory_pressure);

  cg2_pressure_info_t io_pressure;
  cg2_pressure_info_init(&io_pressure);

  cg2_swap_usage_info_t swap_usage;
  cg2_swap_usage_info_init(&swap_usage);

  // check version 2 first
  if (cg2_cgroup_version != CG2_CGROUP_VERSION_1) {
    for (cu_mount_t *mnt_ptr = mnt_list; mnt_ptr != NULL;
         mnt_ptr = mnt_ptr->next) {

      if (strcmp(mnt_ptr->type, "cgroup2") != 0)
        continue;

      DEBUG(LOG_KEY "v2 mount point: '%s'", mnt_ptr->dir);

      if (!cgroup_cpu_found) {
        if (cg2_handle_files(mnt_ptr->dir, SLICE_SUFFIX, "cpu.stat",
                             cg2_handle_v2_cpu_stat, &cpu_usage,
                             CG2_FIRST_LEVEL) > 0) {
          cpu_usage.version = 2;
          cgroup_cpu_found = true;
        } else {
          DEBUG(LOG_KEY "v2 cpu statistic disabled");
        }
      }

      if (cg2_proc_count) {
        cg2_handle_files(mnt_ptr->dir, NULL, "cgroup.procs",
                         cg2_handle_cgroup_procs, &cpu_usage, -1);
      }

      if (cg2_thread_count) {
        cg2_handle_files(mnt_ptr->dir, NULL, "cgroup.threads",
                         cg2_handle_cgroup_threads, &cpu_usage, -1);
      }

      if (!cgroup_mem_found) {
        if (cg2_handle_files(mnt_ptr->dir, SLICE_SUFFIX, "memory.current",
                             cg2_handle_v2_mem_stat, &mem_usage,
                             CG2_FIRST_LEVEL) > 0) {
          mem_usage.version = 2;
          cgroup_mem_found = true;
        } else {
          DEBUG(LOG_KEY "v2 memory statistic disabled");
        }
      }

      if (cg2_cpu_pressure_enabled && !cgroup_cpu_pressure_found) {
        if (cg2_handle_files(mnt_ptr->dir, SLICE_SUFFIX, "cpu.pressure",
                             cg2_handle_pressure, &cpu_pressure,
                             CG2_FIRST_LEVEL) > 0) {
          cpu_pressure.version = 2;
          cgroup_cpu_pressure_found = true;
        } else {
          DEBUG(LOG_KEY "v2 cpu pressure statistic disabled");
        }
      }
      if (cg2_mem_pressure_enabled && !cgroup_mem_pressure_found) {
        if (cg2_handle_files(mnt_ptr->dir, SLICE_SUFFIX, "memory.pressure",
                             cg2_handle_pressure, &memory_pressure,
                             CG2_FIRST_LEVEL) > 0) {
          memory_pressure.version = 2;
          cgroup_mem_pressure_found = true;
        } else {
          DEBUG(LOG_KEY "v2 memory pressure statistic disabled");
        }
      }

      if (cg2_io_pressure_enabled && !cgroup_io_pressure_found) {
        if (cg2_handle_files(mnt_ptr->dir, SLICE_SUFFIX, "io.pressure",
                             cg2_handle_pressure, &io_pressure,
                             CG2_FIRST_LEVEL) > 0) {
          io_pressure.version = 2;
          cgroup_io_pressure_found = true;
        } else {
          DEBUG(LOG_KEY "v2 io pressure statistic disabled");
        }
      }

      if (cg2_swap_available) {
        if (cg2_handle_files(mnt_ptr->dir, SLICE_SUFFIX, "memory.swap.current",
                             cg2_handle_v2_swap_stat, &swap_usage,
                             CG2_FIRST_LEVEL) > 0) {
          swap_usage.version = 2;
          cgroup_swap_found = true;
        } else {
          DEBUG(LOG_KEY "v2 swap usage statistic disabled");
        }
      }
    }
  }

  // check v1 as fallback
  if (cg2_cgroup_version != CG2_CGROUP_VERSION_2) {
    for (cu_mount_t *mnt_ptr = mnt_list; mnt_ptr != NULL;
         mnt_ptr = mnt_ptr->next) {

      if (strcmp(mnt_ptr->type, "cgroup") != 0)
        continue;

      DEBUG(LOG_KEY "v1 mount point: '%s'", mnt_ptr->dir);

      if (cu_mount_checkoption(mnt_ptr->options, "cpuacct", 1)) {
        // avoid reading same data multiple times
        if (!cgroup_cpu_found) {
          if (cg2_handle_files(mnt_ptr->dir, SLICE_SUFFIX, "cpuacct.stat",
                               cg2_handle_v1_cpu_stat, &cpu_usage,
                               CG2_FIRST_LEVEL) > 0) {
            cpu_usage.version = 1;
            cgroup_cpu_found = true;
          } else {
            DEBUG(LOG_KEY "v1 cpu statistic disabled");
          }
        }
      }

      if (cu_mount_checkoption(mnt_ptr->options, "memory", 1)) {
        if (!cgroup_mem_found) {
          if (cg2_handle_files(mnt_ptr->dir, SLICE_SUFFIX,
                               "memory.usage_in_bytes", cg2_handle_v1_mem_stat,
                               &mem_usage, CG2_FIRST_LEVEL) > 0) {
            mem_usage.version = 1;
            cgroup_mem_found = true;
          } else {
            DEBUG(LOG_KEY "v1 memory statistic disabled");
          }
        }
      }
    }
  }

  if (!cg2_mount_cache) {
    cu_mount_freelist(mnt_list);
    mnt_list = NULL;
  }

  if (cgroup_cpu_found) {
    DEBUG(LOG_KEY "found %d cpu usage entries", cpu_usage.entries_num);
    cg2_cpu_usage_info_notify(&cpu_usage);
  } else {
    WARNING(LOG_KEY "Unable to find cpu usage information.");
  }

  if (cgroup_mem_found) {
    DEBUG(LOG_KEY "found %d mem usage entries", mem_usage.entries_num);
    cg2_mem_usage_info_notify(&mem_usage);
  } else {
    WARNING(LOG_KEY "Unable to find memory information.");
  }

  if (cg2_cpu_pressure_enabled) {
    if (cgroup_cpu_pressure_found) {
      cg2_pressure_info_notify(&cpu_pressure, "cpu");
    } else {
      WARNING(LOG_KEY "Unable to find cpu pressure information.");
    }
  }

  if (cg2_mem_pressure_enabled) {
    if (cgroup_mem_pressure_found) {
      cg2_pressure_info_notify(&memory_pressure, "memory");
    } else {
      WARNING(LOG_KEY "Unable to find memory pressure information.");
    }
  }
  if (cg2_io_pressure_enabled) {
    if (cgroup_io_pressure_found) {
      cg2_pressure_info_notify(&io_pressure, "io");
    } else {
      WARNING(LOG_KEY "Unable to find io pressure information.");
    }
  }
  if (cg2_swap_available) {
    if (cgroup_swap_found) {
      cg2_swap_usage_info_notify(&swap_usage);
    } else {
      WARNING(LOG_KEY "Unable to find memory swap information.");
    }
  }

  return 0;
}

//------------------------------------------------------------------------------
static int cg2_shutdown(void) {
  INFO(LOG_KEY "shutdown");
  if (mnt_list == NULL) {
    cu_mount_freelist(mnt_list);
    mnt_list = NULL;
  }
  return 0;
}

//------------------------------------------------------------------------------
void module_register(void) {
  plugin_register_config(PLUGIN_NAME, cg2_config, cg2_keys, cg2_keys_num);
  plugin_register_init(PLUGIN_NAME, cg2_init);
  plugin_register_read(PLUGIN_NAME, cg2_read);
  plugin_register_shutdown(PLUGIN_NAME, cg2_shutdown);
}
