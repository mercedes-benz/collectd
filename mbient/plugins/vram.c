/**
 * collectd - mbient/plugins/vram.c
 * Copyright (C) 2024       Harsha R
 *
 * Features:
 *   - support of logging of richos vram usage and top vram users
 *
 * Authors:
 *   Harsha R <harsha dot r at mercedes-benz.com>
 **/

#include <dirent.h>
#include <stdio.h>
#include "plugin.h"
#include "utils/common/common.h"
#include "collectd.h"

#define PLUGIN_NAME "vram"
#define LOG_KEY PLUGIN_NAME " plugin: "

#define HGSL_BASE_DIR "/sys/devices/virtual/hgsl/hgsl"
#define HGSL_TOTAL_MEM HGSL_BASE_DIR "/total_mem"
#define HGSL_CLIENTS HGSL_BASE_DIR "/clients"

#define BYTES2MB(mem) (mem / 1024.0 / 1024.0 + 0.5)

const gauge_t INVALID_SIZE = -42;

static int vram_top_processes = 0;
static bool vram_usage = false;

//==============================================================================
// Helper functions
//==============================================================================

//------------------------------------------------------------------------------
static ssize_t vram_read_file_content_fd(int fd, void *buf, size_t bufsize) {
  char *buf_ptr;
  size_t len;
  errno = 0;

  buf_ptr = buf;
  len = bufsize;

  ssize_t n = 0;
  while (42) {
    ssize_t status = read(fd, (void *)buf_ptr, len);

    if (status < 0) {
      if ((EAGAIN == errno) || (EINTR == errno))
        continue;

      WARNING(LOG_KEY "Failed to read from fd '%d': %s.", fd, STRERRNO);
      close(fd);
      return -1;
    }

    n += status;

    if (status == 0)
      break;

    buf_ptr += status;
    len -= status;

    if (len == 0)
      break;
  }

  return n;
}

//------------------------------------------------------------------------------
static ssize_t vram_read_file_content(const char *filename, void *buf,
                                      size_t bufsize) {
  int fd = open(filename, O_RDONLY);
  if (fd < 0) {
    if (errno != ENOENT)
      WARNING(LOG_KEY "Failed to open '%s': %s.", filename, STRERRNO);
    return -1;
  }

  ssize_t n = vram_read_file_content_fd(fd, buf, bufsize);

  close(fd);

  return n;
}

//------------------------------------------------------------------------------
static ssize_t vram_read_text_file_content_fd(int fd, char *buf,
                                              size_t bufsize) {
  ssize_t ret = vram_read_file_content_fd(fd, buf, bufsize - 1);
  if (ret < 0)
    return ret;

  buf[ret] = '\0';
  return ret + 1;
}

//------------------------------------------------------------------------------
static ssize_t vram_read_text_file_content(const char *filename, char *buf,
                                           size_t bufsize) {
  ssize_t ret = vram_read_file_content(filename, buf, bufsize - 1);
  if (ret < 0)
    return ret;

  buf[ret] = '\0';
  return ret + 1;
}

//------------------------------------------------------------------------------
gauge_t vram_read_size_fd(int fd) {

  char buffer[256];
  vram_read_text_file_content_fd(fd, buffer, sizeof(buffer));

  value_t v;
  if (parse_value(buffer, &v, DS_TYPE_GAUGE) != 0) {
    return INVALID_SIZE;
  }

  return v.gauge;
}
//------------------------------------------------------------------------------
gauge_t vram_read_size(char const *file_name) {

  char buffer[256];
  vram_read_text_file_content(file_name, buffer, sizeof(buffer));

  value_t v;
  if (parse_value(buffer, &v, DS_TYPE_GAUGE) != 0) {
    return INVALID_SIZE;
  }

  return v.gauge;
}

//------------------------------------------------------------------------------
static int vram_get_process_name(pid_t pid, char *buffer, size_t bufsize) {
  char filename[64];
  ssize_t status;

  // fallback if program name not available
  ssnprintf(buffer, bufsize, "pid-%d", pid);

  ssnprintf(filename, sizeof(filename), "/proc/%d/stat", pid);

  status = vram_read_text_file_content(filename, buffer, bufsize);
  if (status <= 0)
    return -1;

  // parse name which is given in parentheses
  // example: 467 (systemd-journal) S 1 467 467 0 -1 4194560 179870 ...
  const char *name_begin = buffer + strcspn(buffer, "(");
  if (*name_begin == '\0') {
    WARNING(LOG_KEY "no name found in '%s'", buffer);
    return -1;
  }
  ++name_begin;
  const char *name_end = name_begin;
  size_t name_len = 0;
  int bracket_co = 1;
  while (*name_end) {
    size_t forward = strcspn(name_end, "()");
    name_len += forward;
    name_end += forward;
    if (*name_end) {
      bracket_co += *name_end == '(' ? 1 : -1;
      ++name_end;
    }
    if (bracket_co == 0)
      break;
    ++name_len;
  }
  if ((bracket_co != 0) || (*name_end != ' ')) {
    WARNING(LOG_KEY "no name found, wrong brackets in '%s'", buffer);
    return -1;
  }
  // make sure the name fits the given buffer
  if (name_len + 1 >= bufsize)
    name_len = bufsize - 1;

  memmove(buffer, name_begin, name_len);
  buffer[name_len] = '\0';

  return 0;
}

//==============================================================================
// client info
//==============================================================================

//------------------------------------------------------------------------------
typedef struct vram_client_entry_s {
  pid_t pid;
  char name[256];
  gauge_t vram_used;
} vram_client_entry_t;

typedef struct vram_client_info_s {
  int entries_num;
  vram_client_entry_t entries[256];
} vram_client_info_t;

//------------------------------------------------------------------------------
int vram_client_info_init(vram_client_info_t *info) {
  info->entries_num = 0;
  return 0;
}

//------------------------------------------------------------------------------
int vram_client_info_add(vram_client_info_t *info, pid_t pid, gauge_t mem) {
  if (info->entries_num >= STATIC_ARRAY_SIZE(info->entries)) {
    WARNING(LOG_KEY "cpu usage info: reached maximum entries %lu",
            STATIC_ARRAY_SIZE(info->entries));
    return -1;
  }

  vram_client_entry_t *entry = info->entries + info->entries_num;
  entry->pid = pid;
  vram_get_process_name(pid, entry->name, sizeof(entry->name));
  entry->vram_used = mem;

  // DEBUG(LOG_KEY "add client %u:  %s (%d)", info->entries_num, entry->name,
  // pid);
  ++info->entries_num;

  return 0;
}

//------------------------------------------------------------------------------
int vram_client_info_add_unique(vram_client_info_t *info, const char *name,
                                gauge_t mem) {
  vram_client_entry_t *entry;

  // check for existing entry, if found then add memory usage
  for (int co = 0; co < info->entries_num; ++co) {
    entry = info->entries + co;
    if (strcmp(entry->name, name) == 0) {
      entry->vram_used += mem;
      return 0;
    }
  }

  if (info->entries_num >= STATIC_ARRAY_SIZE(info->entries)) {
    WARNING(LOG_KEY "cpu usage info: reached maximum entries %lu",
            STATIC_ARRAY_SIZE(info->entries));
    return -1;
  }

  entry = info->entries + info->entries_num;
  entry->pid = 0;
  sstrncpy(entry->name, name, sizeof(entry->name));
  entry->vram_used = mem;

  ++info->entries_num;

  return 0;
}

//------------------------------------------------------------------------------
int vram_clients_info_create_unique(vram_client_info_t *target,
                                    const vram_client_info_t *source) {
  vram_client_info_init(target);
  for (int co = 0; co < source->entries_num; ++co) {
    const vram_client_entry_t *entry = source->entries + co;
    vram_client_info_add_unique(target, entry->name, entry->vram_used);
  }
  return 0;
}

//------------------------------------------------------------------------------
int vram_clients_info_sort_by_mem(const void *a, const void *b) {
  vram_client_entry_t *e1 = (vram_client_entry_t *)a;
  vram_client_entry_t *e2 = (vram_client_entry_t *)b;
  return e2->vram_used - e1->vram_used;
}

//==============================================================================
// Publishing
//==============================================================================

//------------------------------------------------------------------------------
static void vram_submit_usage(const char *plugin_inst, gauge_t usage) {
  // do not submit invalid values
  if (usage < 0)
    return;

  DEBUG(LOG_KEY "submit vram memory=%f", usage);

  value_t value = {.gauge = usage};

  value_list_t vl = VALUE_LIST_INIT;
  vl.values = &value;
  vl.values_len = 1;

  sstrncpy(vl.host, hostname_g, sizeof(vl.host));
  sstrncpy(vl.plugin, PLUGIN_NAME, sizeof(vl.plugin));
  if (plugin_inst != NULL) {
    sstrncpy(vl.plugin_instance, plugin_inst, sizeof(vl.plugin_instance));
  }
  sstrncpy(vl.type, "memory", sizeof(vl.type));

  plugin_dispatch_values(&vl);
}

//------------------------------------------------------------------------------
static void vram_submit_usage_clients(const vram_client_info_t *client_infos) {

  vram_client_info_t unique_client_infos;
  vram_clients_info_create_unique(&unique_client_infos, client_infos);

  for (int co = 0; co < unique_client_infos.entries_num; ++co) {
    const vram_client_entry_t *e = unique_client_infos.entries + co;
    vram_submit_usage(e->name, e->vram_used);
  }
}

//------------------------------------------------------------------------------
static void vram_notify_usage(gauge_t usage) {
  // do not notify invalid values
  if (usage < 0)
    return;

  DEBUG(LOG_KEY "notify vram memory=%f", usage);

  notification_t n = {NOTIF_OKAY, cdtime(), "", "",  PLUGIN_NAME,
                      "",         "memory", "", NULL};

  snprintf(n.message, sizeof(n.message), "memory total vram - usage(MB): %.0f",
           BYTES2MB(usage));

  plugin_dispatch_notification(&n);
  if (n.meta != NULL)
    plugin_notification_meta_free(n.meta);
}

//------------------------------------------------------------------------------
static void vram_notify_usage_clients(const vram_client_info_t *client_infos) {
  if (client_infos->entries_num <= 0)
    return;

  char buf[512];

  notification_t n = {NOTIF_OKAY, cdtime(), "", "",  PLUGIN_NAME,
                      "",         "memory", "", NULL};
  snprintf(n.message, sizeof(n.message),
           "memory vram top processes - process/usage(MB)/pid");

  int sz = vram_top_processes < client_infos->entries_num
               ? vram_top_processes
               : client_infos->entries_num;
  for (int co = 0; co < sz; ++co) {
    const vram_client_entry_t *e = &client_infos->entries[co];
    snprintf(buf, sizeof(buf), "%s/%.f/%d", e->name, BYTES2MB(e->vram_used),
             e->pid);
    if (plugin_notification_meta_add_string(&n, "", buf) != 0) {
      WARNING(LOG_KEY "Error adding meta information to notification.");
      break;
    }
  }

  plugin_dispatch_notification(&n);
  if (n.meta != NULL)
    plugin_notification_meta_free(n.meta);
}

//==============================================================================
// Reading
//==============================================================================

//------------------------------------------------------------------------------
static int vram_read_usage() {

  // is this metric enabled in configuration?
  if (!vram_usage)
    return 0;

  DEBUG(LOG_KEY "read_usage()");

  gauge_t total_vram_memory = vram_read_size(HGSL_TOTAL_MEM);
  if (total_vram_memory < 0) {
    ERROR(LOG_KEY "read_usage() failed: found size %f", total_vram_memory);
    return -1;
  }

  DEBUG(LOG_KEY "total vram memory=%f", total_vram_memory);
  vram_submit_usage("", total_vram_memory);
  vram_notify_usage(total_vram_memory);

  return 0;
}

//------------------------------------------------------------------------------
static int vram_read_top_processes() {

  // is this metric enabled in configuration?
  if (vram_top_processes <= 0)
    return 0;

  DEBUG(LOG_KEY "read_top_processes()");
  vram_client_info_t client_infos;
  vram_client_info_init(&client_infos);

  {
    struct dirent *entry;
    DIR *clients_dir;
    char *endptr;

    if ((clients_dir = opendir(HGSL_CLIENTS)) == NULL) {
      ERROR(LOG_KEY " opendir(%s) failed: %s ", HGSL_CLIENTS, STRERRNO);
      return -1;
    }

    int clients_fd = dirfd(clients_dir);
    if (clients_fd < 0) {
      ERROR(LOG_KEY "dirfd(%s) failed: %s", HGSL_CLIENTS, STRERRNO);
      return -1;
    }

    while ((entry = readdir(clients_dir)) != NULL) {
      // check for directories only
      if (entry->d_type != DT_DIR) {
        continue;
      }

      // only numbers are allowed as directory names
      pid_t pid = strtol(entry->d_name, &endptr, 10);
      if (*endptr != '\0')
        continue;

      int client_fd = openat(clients_fd, entry->d_name,
                             O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
      if (client_fd < 0) {
        WARNING(LOG_KEY "dirfd(%s/%s) failed: %s", HGSL_CLIENTS, entry->d_name,
                STRERRNO);
        continue;
      }

      int fd = openat(client_fd, "mem_alloc", O_RDONLY);
      if (fd >= 0) {

        gauge_t mem = vram_read_size_fd(fd);
        close(fd);
        if (mem >= 0) {
          if (vram_client_info_add(&client_infos, pid, mem) != 0) {
            break;
          }
        }
      } else {
        WARNING(LOG_KEY "no mem_alloc file available for %d", pid);
      }

      close(client_fd);
    }

    closedir(clients_dir);
  }

  qsort(client_infos.entries, client_infos.entries_num,
        sizeof(vram_client_entry_t), vram_clients_info_sort_by_mem);

#if COLLECT_DEBUG
  {
    gauge_t total_memory = 0.0;
    for (int co = 0; co < client_infos.entries_num; ++co) {
      const vram_client_entry_t *e = &client_infos.entries[co];
      total_memory += e->vram_used;
      DEBUG(LOG_KEY "%d: %s (%.f)", co, e->name, e->vram_used);
    }
    DEBUG(LOG_KEY "total memory: %.f bytes (%.f MB)", total_memory,
          BYTES2MB(total_memory));
  }
#endif

  vram_submit_usage_clients(&client_infos);
  vram_notify_usage_clients(&client_infos);

  return 0;
}

//==============================================================================
// Public interface
//==============================================================================

//------------------------------------------------------------------------------
static int vram_config(oconfig_item_t *ci) /* {{{ */
{
  for (int i = 0; i < ci->children_num; i++) {
    oconfig_item_t *child = ci->children + i;
    if (strcasecmp("NotifyTopProcess", child->key) == 0)
      cf_util_get_int(child, &vram_top_processes);
    else if (strcasecmp("NotifyUsage", child->key) == 0)
      cf_util_get_boolean(child, &vram_usage);
    else
      ERROR(LOG_KEY " Invalid configuration option: "
                    "\"%s\".",
            child->key);
  }
  return 0;
}

//------------------------------------------------------------------------------
static int vram_read(void) {
  if (vram_read_usage() != 0)
    return -1;

  if (vram_read_top_processes() != 0)
    return -1;

  return 0;
}

//------------------------------------------------------------------------------
void module_register(void) {
  plugin_register_complex_config("vram", vram_config);
  plugin_register_read("vram", vram_read);
}
