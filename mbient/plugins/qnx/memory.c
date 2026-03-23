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
 *   Emil Velikov <emil dot velikov at mercedes-benz.com>
 */

#include <sys/mman.h>
#include <sys/syspage.h>
#include <hw/sysinfo.h>
#include <fcntl.h>

#include "collectd.h"
#include "plugin.h"
#include "utils/common/common.h"

#define MAX_CARVEOUTS_TO_MONITOR            64
#define MAX_STRING_LEN                      128
#define MAX_SHMEM_ENTRIES                   512
#define MAX_SHMEM_FILENAME_SIZE             1024
#define PMEM_DIR                            "/dev/pmem/"
#define SHMEM_DIR                           "/dev/shmem/"

typedef struct {
  char name[MAX_STRING_LEN];
  int pid;
  uint64_t memory_used;
} carveout_user_t;

typedef struct {
  carveout_user_t* user;
  int num_users;
} carveout_users_t;

typedef struct {
  char name[MAX_STRING_LEN];
  uint64_t start_address;
  uint64_t end_address;
  uint64_t total_size;
  uint64_t free_size;
  carveout_users_t users;
} carveout_info_t;

typedef struct {
  carveout_info_t* carveout;
  int num_carveouts;
} carveout_infos_t;

typedef struct {
    char name[MAX_STRING_LEN];
    uint64_t size;
    uid_t owner_uid;
} shmem_file_stat_t;

typedef struct {
    uint64_t total_size;
    uint32_t num_files;
    shmem_file_stat_t files[MAX_SHMEM_ENTRIES];
} shmem_dir_stat_t;

static carveout_infos_t* infos;
static shmem_dir_stat_t* shmem_info;
static uint64_t total_sysram_memory;
static uint64_t total_free_memory;

static const char *config_keys[] = {"MonitorCarvout", "PerProcessCarveoutUsage"};
static int config_keys_num = STATIC_ARRAY_SIZE(config_keys);
static char carveouts_to_monitor[MAX_CARVEOUTS_TO_MONITOR][MAX_STRING_LEN];
static bool report_per_process_carvout_usage = false;
static bool monitor_all_carveouts = false;

static uint64_t get_total_free_memory() {
  struct stat buf;
  if (stat("/proc", &buf) == -1) {
    ERROR("Could not stat /proc");
    return -1;
  }
  return buf.st_size;
}

static int get_carveout_memory_free_size(char* carveout_name, int* free_size) {
  int ret, mem_fd;
  struct posix_typed_mem_info info;

  mem_fd = posix_typed_mem_open(carveout_name, O_RDONLY, POSIX_TYPED_MEM_ALLOCATE);
  if (mem_fd == -1) {
    ERROR("posix_typed_mem_open(\"%s\") failed (error %s)", carveout_name, strerror(errno));
    return -1;
  }

  ret = posix_typed_mem_get_info(mem_fd, &info);
  if (ret == -1) {
    ERROR("posix_typed_mem_get_info(\"%s\") failed (error %s)", carveout_name, strerror(errno));
    close(mem_fd);
    return -1;
  }
  close(mem_fd);

  if (free_size)
    *free_size = info.posix_tmi_length;

  return 0;
}

static uint64_t get_total_sysram_memory() {
  uint64_t size = 0;
  /* For whatever reason posix_typed_mem_get_info("sysram") does not always return correct value
   * Because of this get the total sysram memory from the syspage
   */
  struct asinfo_entry* asinfo = SYSPAGE_ENTRY(asinfo);
  for (int i = 0; i < (_syspage_ptr->asinfo.entry_size / (sizeof(*asinfo))); i++, asinfo++) {
    if (strcmp(__hwi_find_string(asinfo->name), "sysram") == 0) {
      size += asinfo->end - asinfo->start + 1;
    }
  }
  return size;
}

static carveout_info_t* find_carveout_info_by_name(const char* name) {
  for (int i = 0; i < infos->num_carveouts; i++) {
    if (strcmp(infos->carveout[i].name, name) == 0) {
      return &infos->carveout[i];
    }
  }
  return NULL;
}

static carveout_info_t* find_carveout_info_by_pa(uint64_t pa) {
  for (int i = 0; i < infos->num_carveouts; i++) {
    if (pa >= infos->carveout[i].start_address && pa <= infos->carveout[i].end_address) {
      return &infos->carveout[i];
    }
  }
  return NULL;
}

static carveout_user_t* find_user_by_pid(carveout_info_t* carveout, int pid) {
  for (int i = 0; i < carveout->users.num_users; i++) {
    if (carveout->users.user[i].pid == pid) {
      return &carveout->users.user[i];
    }
  }
  return NULL;
}

static carveout_user_t* find_spare_user(carveout_info_t* carveout) {
  for (int i = 0; i < carveout->users.num_users; i++) {
    if (carveout->users.user[i].pid == -1) {
      return &carveout->users.user[i];
    }
  }
  return NULL;
}

static inline bool is_carveout_to_monitor(const char* name) {
  if (monitor_all_carveouts) {
    return true;
  }
  for (int i = 0; i < MAX_CARVEOUTS_TO_MONITOR && *carveouts_to_monitor[i]; i++) {
    if (strcmp(carveouts_to_monitor[i], name) == 0) {
      return true;
    }
  }
  return false;
}

static int init_carveout_infos() {
  uint64_t num_asinfo;
  char* name;
  struct asinfo_entry* asinfo;

  num_asinfo = (_syspage_ptr->asinfo.entry_size / (sizeof(*asinfo)));
  asinfo = SYSPAGE_ENTRY(asinfo);

  for (; num_asinfo != 0; num_asinfo--, asinfo++) {
    name = __hwi_find_string(asinfo->name);
    if (is_carveout_to_monitor(name)) {
      if (find_carveout_info_by_name(name) == NULL) {
        carveout_info_t* new_carveout = realloc(infos->carveout, (infos->num_carveouts + 1) * sizeof(carveout_info_t));
        if (!new_carveout) {
          ERROR("Failed to allocate memory for carveout infos (error %s)!", strerror(errno));
          free(infos->carveout);
          return -1;
        }
        infos->carveout = new_carveout;
        carveout_info_t* curr = &infos->carveout[infos->num_carveouts];
        sstrncpy(curr->name, name, sizeof(curr->name));
        curr->start_address = asinfo->start;
        curr->end_address = asinfo->end;
        curr->total_size = asinfo->end - asinfo->start + 1;
        curr->free_size = 0; /* Will be calculated later */
        curr->users.user = NULL;
        curr->users.num_users = 0;
        infos->num_carveouts++;
      } else {
        /* In our system only "sysram" is splitted in separate regions. So we should not enter here. */
        WARNING("Carveout \"%s\" already exists in the carveout infos!", name);
      }
    }
  }
  return 0;
}

static void reset_carveout_users() {
  for (int i = 0; i < infos->num_carveouts; i++) {
    carveout_info_t* carveout = &infos->carveout[i];
    for (int j = 0; j < carveout->users.num_users; j++) {
      carveout->users.user[j].pid = -1;
      carveout->users.user[j].memory_used = 0;
    }
  }
}

static void get_per_process_carveout_usage() {
  const char* const no_str = "No";
  const char* const size_str = "Size";
  const char* const pid_str = "pid";
  const char* const pa_str = "pa";

  reset_carveout_users();

  /* Traverse the /dev/pmem/ directory */
  DIR* dir = opendir(PMEM_DIR);
  if (!dir) {
    ERROR("Failed to open %s directory (error %s)!", PMEM_DIR, strerror(errno));
    return;
  }

  struct dirent* entry;
  char* line = NULL;
  size_t len = 0;

  while ((entry = readdir(dir)) != NULL) {
    char filename[MAX_STRING_LEN] = {0};
    ssnprintf(filename, sizeof(filename), "%s%s", PMEM_DIR, entry->d_name);
    FILE* fp = fopen(filename, "r");
    if (!fp) {
      ERROR("Failed to open %s (error %s)!", filename, strerror(errno));
      continue;
    }
    /* Read the file line by line */
    while (getline(&line, &len, fp) != -1) {
      int pid;
      uint64_t size, pa;
      /* If there are no PMEM allocations for the given process the file contains "No PMEM allocations\n" string.
       * To spare some CPU power check only the first two characters.
       */
      if (strncmp(line, no_str, strlen(no_str)) == 0) {
        continue;
      }

      /* Get the "Size" entry from the line */
      char* substr = strstr(line, size_str);
      if (substr == NULL) {
        ERROR("Failed to find \"Size\" in %s!", line);
        continue;
      }
      substr += strlen(size_str);
      size = strtoul(substr, NULL, 16);

      /* Get the "pid" entry */
      substr = strstr(line, pid_str);
      if (substr == NULL) {
        ERROR("Failed to find \"pid\" in %s!", line);
        continue;
      }
      substr += strlen(pid_str);
      pid = strtol(substr, NULL, 10);

      /* Finally, get the "pa" */
      substr = strstr(line, pa_str);
      if (substr == NULL) {
        ERROR("Failed to find \"pa\" in %s!", line);
        continue;
      }
      substr += strlen(pa_str);
      pa = strtoul(substr, NULL, 16);

      /* Now check in which carveout the entry falls into */
      carveout_info_t* carveout = find_carveout_info_by_pa(pa);
      if (carveout == NULL) {
        /* Ignore this as it may be part of a carveout which we are not interested in and not monitoring */
        continue;
      }

      /* First try to find if we already have that user for the given carveout */
      carveout_user_t* user = find_user_by_pid(carveout, pid);
      if (user == NULL) {
        /* If not, check if there is already an allocated "user" which is not being used */
        user = find_spare_user(carveout);
        if (user == NULL) {
          /* If not, allocate a new entry */
          carveout_user_t* new_user = realloc(carveout->users.user, (carveout->users.num_users + 1) * sizeof(carveout_user_t));
          if (!new_user) {
            ERROR("Failed to allocate memory for user \"%s\" in carveout \"%s\" (error %s)!",
                    entry->d_name, carveout->name, strerror(errno));
            continue;
          }
          carveout->users.user = new_user;
          user = &carveout->users.user[carveout->users.num_users];
          carveout->users.num_users++;
        }
        user->pid = pid;
        user->memory_used = 0; /* memory_used will be incremented below */
        sstrncpy(user->name, entry->d_name, sizeof(user->name));
      }
      user->memory_used += size;
    }
    if (ferror(fp)) {
      ERROR("Error reading from \"%s\" (error %s)!", filename, strerror(errno));
    }
    if (fclose(fp) == EOF) {
      ERROR("Failed to close \"%s\" (error %s)!", filename, strerror(errno));
    }
  }
  if (closedir(dir) == -1) {
    ERROR("Failed to close %s directory (error %s)!", PMEM_DIR, strerror(errno));
  }
  free(line);
}

static void add_carveout_to_monitor(const char* name) {
  static int num_carveouts_to_monitor = 0;
  if (num_carveouts_to_monitor >= MAX_CARVEOUTS_TO_MONITOR) {
    ERROR("Exceeded the maximum number of carveouts to monitor!");
    return;
  }
  sstrncpy(carveouts_to_monitor[num_carveouts_to_monitor++], name, MAX_STRING_LEN);
}

static int compare_files(const void* f1, const void* f2) {
    shmem_file_stat_t* file1 = (shmem_file_stat_t*)f1;
    shmem_file_stat_t* file2 = (shmem_file_stat_t*)f2;

    /* We want to sort in descending order */
    if (file1->size < file2->size) {
        return 1;
    } else if (file1->size > file2->size) {
        return -1;
    } else {
        return 0;
    }
}

// Go through the files in /dev/shmem/ and check their names, sizes and owner user
void traverse_dir(const char* dir, shmem_dir_stat_t* dir_stat) {
  if (!dir_stat) {
    ERROR("dir_stat is NULL");
    return;
  }

  DIR *d = opendir(dir);
  if (!d) {
    ERROR("Failed to open %s directory (error %s)!", dir, strerror(errno));
    return;
  }

  struct dirent *entry;
  while ((entry = readdir(d)) != NULL) {
    if (entry->d_name[0] == '.') {
      continue;
    }

    char path[MAX_SHMEM_FILENAME_SIZE] = {0};
    snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);

    struct stat st;
    if (stat(path, &st) == -1) {
      ERROR("Failed to stat %s directory (error %s)!", path, strerror(errno));
      closedir(d);
      return;
    }

    /* Directories cannot be created under /dev/shmem manually.
     * The only directory is /dev/shmem/slogger2 which does not have subdirectories.
     * So not problem of calling of traverse_dir() recursively.
     */
    if (S_ISDIR(st.st_mode)) {
      traverse_dir(path, dir_stat);
    } else {
      if (dir_stat->num_files >= MAX_SHMEM_ENTRIES) {
        WARNING("Maximum number of shmem entries (%d) reached, ignoring remaining files",
                MAX_SHMEM_ENTRIES);
        break;  // Break out of loop, closedir() will be called below
      }

      shmem_file_stat_t* file_stat = &dir_stat->files[dir_stat->num_files];
      snprintf(file_stat->name, sizeof(file_stat->name), "%s", entry->d_name);
      file_stat->size = st.st_size;
      file_stat->owner_uid = st.st_uid;

      dir_stat->total_size += st.st_size;
      dir_stat->num_files++;
    }
  }

  closedir(d);
}

static void get_shmem_usage() {
  memset(shmem_info, 0, sizeof(shmem_dir_stat_t));
  traverse_dir(SHMEM_DIR, shmem_info);
  qsort(shmem_info->files, shmem_info->num_files, sizeof(shmem_file_stat_t), compare_files);
}

static void memory_submit() {
  value_list_t vl = VALUE_LIST_INIT;
  value_t values[2];

  vl.values = values;
  sstrncpy(vl.plugin, "memory", sizeof(vl.plugin));
  sstrncpy(vl.plugin_instance, "sysram", sizeof(vl.plugin_instance));
  sstrncpy(vl.type, "carveout", sizeof(vl.type));

  vl.values[0].counter = total_sysram_memory;
  vl.values[1].counter = total_free_memory;
  vl.values_len = 2;
  plugin_dispatch_values(&vl);

  for (int i = 0; i < infos->num_carveouts; i++) {
    carveout_info_t* carveout = &infos->carveout[i];
    sstrncpy(vl.plugin_instance, carveout->name, sizeof(vl.plugin_instance));
    vl.values[0].counter = carveout->total_size;
    vl.values[1].counter = carveout->free_size;
    plugin_dispatch_values(&vl);
  }
}

static void memory_notify() {
  char buf[MAX_STRING_LEN] = {0};
  int buf_size = sizeof(buf);
  int bytes_written;

  cdtime_t now = cdtime();
  notification_t n = {NOTIF_OKAY, now, "", "", "memory",
                        "", "", "", NULL};

  char* msg = n.message;
  int msg_size = sizeof(n.message);

  uint64_t total_used_memory = total_sysram_memory - total_free_memory;
  ssnprintf(msg, msg_size,
        "memory total SafeOS - usage(MB)/usage(%%)/total(MB): %.2f/%.2f%%/%d", total_used_memory / 1024.0 / 1024.0,
                                                                         (total_used_memory / (double)total_sysram_memory) * 100,
                                                                         (int)(total_sysram_memory / 1024 / 1024));

  plugin_dispatch_notification(&n);

  sstrncpy(msg, "memory regions SafeOS - name/usage(MB)/usage(%)/total(MB)", msg_size);
  for (int i = 0; i < infos->num_carveouts; i++) {
    carveout_info_t* carveout = &infos->carveout[i];
    uint64_t used_size = carveout->total_size - carveout->free_size;
    bytes_written = ssnprintf(buf, buf_size,
          "%s/%.2f/%.2f%%/%.2f",
          carveout->name, used_size / 1024.0 / 1024.0,
          (used_size / (double)carveout->total_size) * 100,
          carveout->total_size / 1024.0 / 1024.0);

    if (bytes_written < 0) {
      ERROR("Failed to write to buffer for memory carveouts' notification");
      return;
    } else if (bytes_written > buf_size) {
      WARNING("Buffer is truncated! Increase the buffer size.");
    }
    if (plugin_notification_meta_add_string(&n, "", buf) != 0) {
      ERROR("Error adding meta information to notification.");
      break;
    }
  }

  plugin_dispatch_notification(&n);
  if (n.meta != NULL) {
    plugin_notification_meta_free(n.meta);
    /* n.meta is freed in plugin_notification_meta_free() but is not set to NULL
     * So we need to set it explicitly to avoid double-free below.
     */
    n.meta = NULL;
  }

  if (report_per_process_carvout_usage) {
    memset(&buf, 0, sizeof(buf));
    for (int i = 0; i < infos->num_carveouts; i++) {
      carveout_info_t* carveout = &infos->carveout[i];
      ssnprintf(msg, msg_size, "per process memory usage for \"%s\" - name/PID/usage(%%)/usage(KB)%s",
                carveout->name, carveout->users.num_users == 0 ? ": No users" : "");

      for (int j = 0; j < carveout->users.num_users; j++) {
        carveout_user_t* user = &carveout->users.user[j];
        bytes_written = ssnprintf(buf, buf_size, "%s/%d/%.2f%%/%ld", user->name, user->pid,
                  (user->memory_used / (double)carveout->total_size) * 100, user->memory_used / 1024);
        if (bytes_written < 0) {
          ERROR("Failed to write to buffer for memory carveouts' notification");
          return;
        } else if (bytes_written > buf_size) {
          WARNING("Buffer is truncated! Increase the buffer size.");
        }
        if (plugin_notification_meta_add_string(&n, "", buf) != 0) {
          ERROR("Error adding meta information to notification.");
          break;
        }
      }
      plugin_dispatch_notification(&n);
      if (n.meta != NULL) {
        plugin_notification_meta_free(n.meta);
        n.meta = NULL;
      }
    }
  }

  /* Report shmem usage */
  ssnprintf(msg, msg_size, "shared memory usage - total(MB)/num_files: %.2f/%d",
            shmem_info->total_size / 1024.0 / 1024.0, shmem_info->num_files);
  plugin_dispatch_notification(&n);

  sstrncpy(msg, "top 10 shmem allocations - name/size(MB)/user", msg_size);
  memset(&buf, 0, sizeof(buf));
  for (int i = 0; i < 10; i++) {
    if (i >= shmem_info->num_files) {
      break;
    }
    shmem_file_stat_t* file_stat = &shmem_info->files[i];
    bytes_written = ssnprintf(buf, sizeof(buf),
          "%s/%.2f/%d",
          file_stat->name, file_stat->size / 1024.0 / 1024.0, file_stat->owner_uid);
    if (bytes_written < 0) {
      ERROR("Failed to write to buffer for top shmem users' notification.");
      return;
    } else if (bytes_written >= buf_size) {
      WARNING("Buffer is truncated! Increase the buffer size.");
    }
    if (plugin_notification_meta_add_string(&n, "", buf) != 0) {
      ERROR("Error adding meta information to notification.");
      break;
    }
  }

  plugin_dispatch_notification(&n);
  if (n.meta != NULL) {
    plugin_notification_meta_free(n.meta);
    n.meta = NULL;
  }
}

static int memory_init(void) {
  infos = (carveout_infos_t*)calloc(1, sizeof(carveout_infos_t));
  if (!infos) {
    ERROR("Failed to allocate memory for infos!");
    return -1;
  }

  shmem_info = (shmem_dir_stat_t*)calloc(1, sizeof(shmem_dir_stat_t));
  if (!shmem_info) {
    ERROR("Failed to allocate memory for shmem_info!");
    return -1;
  }

  /* Total sysram memory will not change so get it on init */
  total_sysram_memory = get_total_sysram_memory();
  if (total_sysram_memory == -1) {
    ERROR("Failed to get total sysram memory!");
    return -1;
  }
  return init_carveout_infos();
}

static int memory_config(char const *key, char const *value) {
  if (strcasecmp(key, "MonitorCarvout") == 0) {
    if (strcasecmp(value, "all") == 0) {
      /* Check the there was already a specified carveout to monitor */
      if (carveouts_to_monitor[0][0] != '\0') {
        ERROR("Cannot monitor all carveouts and specific carveouts at the same time!");
        return -1;
      }
      monitor_all_carveouts = true;
    } else {
      if (monitor_all_carveouts) {
        ERROR("Cannot monitor all carveouts and specific carveouts at the same time!");
        return -1;
      }
      add_carveout_to_monitor(value);
    }
  } else if ((strcasecmp(key, "PerProcessCarveoutUsage") == 0)) {
    if (IS_TRUE(value)) {
      report_per_process_carvout_usage = true;
    }
  }
  return 0;
}

static int memory_read() {
  total_free_memory = get_total_free_memory();
  if (total_free_memory == -1) {
    ERROR("Failed to get total free memory!");
    return -1;
  }

  for (int i = 0; i < infos->num_carveouts; i++) {
    int free_size;
    carveout_info_t* carveout = &infos->carveout[i];
    if (get_carveout_memory_free_size(carveout->name, &free_size) == -1) {
      ERROR("Failed to get free memory size for carveout \"%s\"", carveout->name);
      continue;
    }
    carveout->free_size = free_size;
  }

  if (report_per_process_carvout_usage) {
    get_per_process_carveout_usage();
  }

  get_shmem_usage();

  memory_submit();
  memory_notify();
  return 0;
}

static int memory_shutdown() {
  for (int i = 0; i < infos->num_carveouts; i++) {
    carveout_info_t* carveout = &infos->carveout[i];
    free(carveout->users.user);
  }
  free(infos->carveout);
  free(infos);
  free(shmem_info);
  return 0;
}

void module_register(void) {
  plugin_register_init("memory", memory_init);
  plugin_register_read("memory", memory_read);
  plugin_register_config("memory", memory_config, config_keys, config_keys_num);
  plugin_register_shutdown("memory", memory_shutdown);
}
