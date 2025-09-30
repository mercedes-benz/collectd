/**
 * The MIT License
 *
 * Copyright (C) 2024 MBition GmbH
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
 *   Ravi Sankar P <ravi_sankar dot ponnurangam at mercedes-benz.com>
 *
 * Rewritten version of `memory` plugin.
 * Features:
 *   - support of logging of richos memory and pressure
 * Credits to the authors of `memory.c` for their inspiration.
 */

#include "collectd.h"

#include "plugin.h"
#include "utils/common/common.h"

#define PLUGIN_NAME "memory2"
#define LOG_KEY PLUGIN_NAME " plugin: "

#define _MB(mem) ((mem) / 1024 / 1024)

static const char *ZRAM_MEM_PATH = "/sys/block/zram0/mm_stat";
static const char *PROC_MEMINFO_PATH = "/proc/meminfo";
static const char *MEMORY_PRESSURE_PATH = "/proc/pressure/memory";

static bool values_absolute = true;
static bool values_percentage;
static bool extra_stats = false;
static bool memory_usage = false;
static bool memory_pressure = false;
static bool memory_zram = false;
static bool detailed_slab_info = false;

struct memory_stats_s {
  gauge_t mem_total;
  gauge_t mem_used;
  gauge_t mem_buffered;
  gauge_t mem_cached;
  gauge_t mem_free;
  gauge_t mem_available;
  gauge_t mem_anon_pages;
  gauge_t mem_mapped;
  gauge_t mem_shmem;
  gauge_t mem_slab_total;
  gauge_t mem_slab_reclaimable;
  gauge_t mem_slab_unreclaimable;
  gauge_t mem_swap_total;
  gauge_t mem_swap_free;
  gauge_t mem_swap_cached;
};

struct zram_mem_s {
  gauge_t orig;
  gauge_t compr;
  gauge_t used;
  gauge_t ratio;
};

static struct memory_stats_s memory_stats_t;
static gauge_t mem_pressure = 0;
static struct zram_mem_s zram_mem_t;

//------------------------------------------------------------------------------
static bool memory2_is_zram_available() {

  FILE *fh;
  char buffer[1024];
  char *fields[10];
  bool availability = false;

  if ((fh = fopen(ZRAM_MEM_PATH, "r")) == NULL) {
    WARNING(LOG_KEY "fopen() failed with '%s'", STRERRNO);
    return false;
  }

  while (fgets(buffer, sizeof(buffer), fh) != NULL) {
    int numfields = strsplit(buffer, fields, STATIC_ARRAY_SIZE(fields));
    if ((numfields >= 9) && (atof(fields[0]) > 0)) {
      availability = true;
      break;
    }
  }

  if (fclose(fh)) {
    WARNING(LOG_KEY "fclose() failed with '%s'", STRERRNO);
    return false;
  }

  INFO(LOG_KEY "zram is %savailable", availability ? "" : "not ");
  return availability;
}

//------------------------------------------------------------------------------
static int memory2_config(oconfig_item_t *ci) /* {{{ */
{
  for (int i = 0; i < ci->children_num; i++) {
    oconfig_item_t *child = ci->children + i;
    if (strcasecmp("ValuesAbsolute", child->key) == 0)
      cf_util_get_boolean(child, &values_absolute);
    else if (strcasecmp("ValuesPercentage", child->key) == 0)
      cf_util_get_boolean(child, &values_percentage);
    else if (strcasecmp("ExtraStats", child->key) == 0)
      cf_util_get_boolean(child, &extra_stats);
    else if (strcasecmp("MemoryUsage", child->key) == 0)
      cf_util_get_boolean(child, &memory_usage);
    else if (strcasecmp("MemoryPressure", child->key) == 0)
      cf_util_get_boolean(child, &memory_pressure);
    else
      ERROR("memory plugin: Invalid configuration option: "
            "\"%s\".",
            child->key);
  }

  return 0;
} /* }}} int memory2_config */

static int memory2_init(void) {

  memory_zram = memory2_is_zram_available();

  return 0;
}

#define MEMORY_SUBMIT(...)                                                     \
  do {                                                                         \
    if (values_absolute)                                                       \
      plugin_dispatch_multivalue(vl, false, DS_TYPE_GAUGE, __VA_ARGS__, NULL); \
    if (values_percentage)                                                     \
      plugin_dispatch_multivalue(vl, true, DS_TYPE_GAUGE, __VA_ARGS__, NULL);  \
  } while (0)

//------------------------------------------------------------------------------
static int memory2_notify(value_list_t *vl) {
#if KERNEL_LINUX
  notification_t n = {NOTIF_OKAY, cdtime(),    "", "",  PLUGIN_NAME,
                      "",         "mem_usage", "", NULL};
  char *msgp = n.message;
  size_t msgn = sizeof(n.message);
  char buf[256] = {0};
  char *bufp = buf;
  size_t bufn = sizeof(buf);
  int r = 0;
  if (memory_usage) {
    snprintf(msgp, msgn,
             "memory total %s - "
             "total/free/available/buffers/cached/shmem/slab/swaptotal/"
             "swapfree/swapcached (MB)",
             hostname_g);
    r = snprintf(
        bufp, bufn, "%.f/%.f/%.f/%.f/%.f/%.f/%.f/%.f/%.f/%.f",
        _MB(memory_stats_t.mem_total), _MB(memory_stats_t.mem_free),
        _MB(memory_stats_t.mem_available), _MB(memory_stats_t.mem_buffered),
        _MB(memory_stats_t.mem_cached), _MB(memory_stats_t.mem_shmem),
        _MB(memory_stats_t.mem_slab_total), _MB(memory_stats_t.mem_swap_total),
        _MB(memory_stats_t.mem_swap_free), _MB(memory_stats_t.mem_swap_cached));
    if (r < 0 || r > bufn) {
      ERROR(LOG_KEY "Error while adding memory usage to buffer");
      return -1;
    }

    if (plugin_notification_meta_add_string(&n, "", buf) != 0) {
      ERROR(LOG_KEY "Error adding meta information to notification.");
      return -1;
    }

    plugin_dispatch_notification(&n);
    if (n.meta != NULL)
      plugin_notification_meta_free(n.meta);
  }

  // special notification for compressed RAM-based block device
  if (memory_zram) {
    char buff[256] = {0};
    bufp = buff;
    bufn = sizeof(buff);
    notification_t z = {NOTIF_OKAY, cdtime(),   "", "",  PLUGIN_NAME,
                        "",         "mem_zram", "", NULL};
    msgp = z.message;
    msgn = sizeof(z.message);
    snprintf(msgp, msgn, "memory zram %s - orig/compr/used (MB)/ratio (%%)",
             hostname_g);
    r = snprintf(bufp, bufn, "%.f/%.f/%.f/%.f", _MB(zram_mem_t.orig),
                 _MB(zram_mem_t.compr), _MB(zram_mem_t.used), zram_mem_t.ratio);
    if (r < 0 || r > bufn) {
      ERROR(LOG_KEY "Error while adding zram memory usage to buffer");
      return -1;
    }

    if (plugin_notification_meta_add_string(&z, "", buff) != 0) {
      ERROR(LOG_KEY "Error adding meta information to notification.");
      return -1;
    }
    plugin_dispatch_notification(&z);
    if (z.meta != NULL)
      plugin_notification_meta_free(z.meta);
  }

  if (memory_pressure) {
    notification_t p = {NOTIF_OKAY, cdtime(),       "", "",  PLUGIN_NAME,
                        "",         "mem_pressure", "", NULL};
    msgp = p.message;
    msgn = sizeof(p.message);
    snprintf(msgp, msgn, "memory pressure %s: %.2f", hostname_g, mem_pressure);

    plugin_dispatch_notification(&p);
    if (p.meta != NULL)
      plugin_notification_meta_free(p.meta);
  }

  if (memory_stats_t.mem_total <
      (memory_stats_t.mem_free + memory_stats_t.mem_buffered +
       memory_stats_t.mem_cached + memory_stats_t.mem_slab_total))
    return -1;

  memory_stats_t.mem_used =
      memory_stats_t.mem_total -
      (memory_stats_t.mem_free + memory_stats_t.mem_buffered +
       memory_stats_t.mem_cached + memory_stats_t.mem_slab_total);

  /* SReclaimable and SUnreclaim were introduced in kernel 2.6.19
   * They sum up to the value of Slab, which is available on older & newer
   * kernels. So SReclaimable/SUnreclaim are submitted if available, and Slab
   * if not. */
  if (detailed_slab_info)
    MEMORY_SUBMIT("used", memory_stats_t.mem_used, "buffered",
                  memory_stats_t.mem_buffered, "cached",
                  memory_stats_t.mem_cached, "free", memory_stats_t.mem_free,
                  "slab_unrecl", memory_stats_t.mem_slab_unreclaimable,
                  "slab_recl", memory_stats_t.mem_slab_reclaimable);
  else
    MEMORY_SUBMIT("used", memory_stats_t.mem_used, "buffered",
                  memory_stats_t.mem_buffered, "cached",
                  memory_stats_t.mem_cached, "free", memory_stats_t.mem_free,
                  "slab", memory_stats_t.mem_slab_total);
  if (extra_stats)
    MEMORY_SUBMIT("available", memory_stats_t.mem_available, "anon_pages",
                  memory_stats_t.mem_anon_pages, "mapped",
                  memory_stats_t.mem_mapped, "shmem", memory_stats_t.mem_shmem);
#endif

  return 0;
}

//------------------------------------------------------------------------------
static int memory2_read(void) {
#if KERNEL_LINUX
  value_t v[1];
  value_list_t vl = VALUE_LIST_INIT;

  vl.values = v;
  vl.values_len = STATIC_ARRAY_SIZE(v);
  sstrncpy(vl.plugin, "memory2", sizeof(vl.plugin));
  sstrncpy(vl.type, "memory", sizeof(vl.type));
  vl.time = cdtime();
  FILE *fh;
  char buffer[1024];
  char *fields[10];
  int numfields;

  if ((fh = fopen(PROC_MEMINFO_PATH, "r")) == NULL) {
    WARNING(LOG_KEY "fopen() failed with '%s'", STRERRNO);
    return -1;
  }

  memset(&memory_stats_t, 0, sizeof(memory_stats_t));
  while (fgets(buffer, sizeof(buffer), fh) != NULL) {
    gauge_t *val = NULL;

    if (strncasecmp(buffer, "MemTotal:", 9) == 0)
      val = &memory_stats_t.mem_total;
    else if (strncasecmp(buffer, "MemFree:", 8) == 0)
      val = &memory_stats_t.mem_free;
    else if (strncasecmp(buffer, "Buffers:", 8) == 0)
      val = &memory_stats_t.mem_buffered;
    else if (strncasecmp(buffer, "Cached:", 7) == 0)
      val = &memory_stats_t.mem_cached;
    else if (strncasecmp(buffer, "MemAvailable:", 13) == 0)
      val = &memory_stats_t.mem_available;
    else if (strncasecmp(buffer, "AnonPages:", 10) == 0)
      val = &memory_stats_t.mem_anon_pages;
    else if (strncasecmp(buffer, "Mapped:", 7) == 0)
      val = &memory_stats_t.mem_mapped;
    else if (strncasecmp(buffer, "Shmem:", 6) == 0)
      val = &memory_stats_t.mem_shmem;
    else if (strncasecmp(buffer, "Slab:", 5) == 0)
      val = &memory_stats_t.mem_slab_total;
    else if (strncasecmp(buffer, "SReclaimable:", 13) == 0) {
      val = &memory_stats_t.mem_slab_reclaimable;
      detailed_slab_info = true;
    } else if (strncasecmp(buffer, "SUnreclaim:", 11) == 0) {
      val = &memory_stats_t.mem_slab_unreclaimable;
      detailed_slab_info = true;
    } else if (strncasecmp(buffer, "SwapTotal:", 10) == 0)
      val = &memory_stats_t.mem_swap_total;
    else if (strncasecmp(buffer, "SwapFree:", 9) == 0)
      val = &memory_stats_t.mem_swap_free;
    else if (strncasecmp(buffer, "SwapCached:", 11) == 0)
      val = &memory_stats_t.mem_swap_cached;
    else
      continue;

    numfields = strsplit(buffer, fields, STATIC_ARRAY_SIZE(fields));
    if (numfields < 2)
      continue;

    *val = 1024.0 * atof(fields[1]);
  }

  if (fclose(fh)) {
    WARNING(LOG_KEY "fclose() failed with '%s'", STRERRNO);
  }

  if (memory_pressure) {
    if ((fh = fopen(MEMORY_PRESSURE_PATH, "r")) == NULL) {
      WARNING(LOG_KEY "fopen() failed with '%s'", STRERRNO);
      return -1;
    }
    memset(&buffer, 0, sizeof(buffer));
    while (fgets(buffer, sizeof(buffer), fh) != NULL) {
      if (strncasecmp(buffer, "some", 4) == 0) {
        numfields = strsplit(buffer, fields, STATIC_ARRAY_SIZE(fields));
        if (numfields < 2)
          continue;
        mem_pressure = atof(fields[1] + 6);
      }
    }
    if (fclose(fh)) {
      WARNING(LOG_KEY "fclose() failed with '%s'", STRERRNO);
    }
  }

  if (memory_zram) {
    memset(&zram_mem_t, 0, sizeof(zram_mem_t));
    if ((fh = fopen(ZRAM_MEM_PATH, "r")) == NULL) {
      WARNING(LOG_KEY "fopen() failed with '%s'", STRERRNO);
    } else {
      while (fgets(buffer, sizeof(buffer), fh) != NULL) {
        numfields = strsplit(buffer, fields, STATIC_ARRAY_SIZE(fields));

        if (numfields < 8)
          continue;

        zram_mem_t.orig = atof(fields[0]);
        zram_mem_t.compr = atof(fields[1]);
        zram_mem_t.used = atof(fields[2]);
        zram_mem_t.ratio = (zram_mem_t.used * 100.0) / zram_mem_t.orig;

        break; // take first fitting entry
      }
      if (fclose(fh)) {
        WARNING(LOG_KEY "fclose() failed with %s", STRERRNO);
      }
    }
  }
  memory2_notify(&vl);
#endif

  return 0;
}

//------------------------------------------------------------------------------
void module_register(void) {
  plugin_register_complex_config("memory2", memory2_config);
  plugin_register_init("memory2", memory2_init);
  plugin_register_read("memory2", memory2_read);
} /* void module_register */
