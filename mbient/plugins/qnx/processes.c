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

 #include "collectd.h"

#include "plugin.h"
#include "utils/common/common.h"
#include "utils/avltree/avltree.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/procfs.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <pthread.h>

#define NSEC_TO_SEC   1000000000ULL
#define DEFAULT_NUM_USERS   10
#define MAX_STRING_LEN 128
#define MAX_NUM_MAP_REGIONS 2048
#define DEFAULT_THREAD_NAME "(noname)"

static const char *config_keys[] = {"NotifyCpuPerThread",
                                    "SubmitCpuPerThread",
                                    "NumTopUsersToNotify",
                                    "NumTopUsersToSubmit",
                                    "NotifyInSingleLine"};
static int config_keys_num = STATIC_ARRAY_SIZE(config_keys);

typedef c_avl_tree_t* process_tree_t;

typedef struct {
  uint64_t execution_time;
  uint64_t last_execution_time;
  uint64_t elapsed_time;
  uint64_t last_elapsed_time;
  float cpu_usage;
} stats_t;

typedef struct {
  uint64_t stack;
  uint64_t heap;
  uint64_t code;
  uint64_t data;
  uint64_t total;
} process_memory_t;

typedef struct {
  int pid;
  char* name;
  c_avl_tree_t* threads;
  process_memory_t memory;
  stats_t cpu_stats;
} process_entry_t;

typedef struct {
  int tid;
  char* name;
  uint64_t start_time;
  process_entry_t* parent;
  stats_t cpu_stats;
  uint8_t priority;
} thread_entry_t;

typedef enum {
  TOP_CPU_PROCESSES,
  TOP_CPU_THREADS,
  TOP_MEMORY_PROCESSES
} top_category_t;

typedef struct {
  process_entry_t** entries;
  int current_size;
} top_processes_t;

typedef struct {
  thread_entry_t** entries;
  int current_size;
} top_threads_t;


static bool measure_cpu_per_thread = false;
static bool measure_cpu_per_process = false;
static bool notify_cpu_per_thread = false;
static bool submit_cpu_per_thread = false;
static bool notify_in_single_line = false;
static int num_top_users_to_notify = DEFAULT_NUM_USERS;
static int num_top_users_to_submit = DEFAULT_NUM_USERS;
static int num_top_users_to_monitor = DEFAULT_NUM_USERS;
static int num_cpus = 0;
static process_tree_t process_tree;

static top_processes_t top_cpu_processes, top_memory_processes;
static top_threads_t top_cpu_threads;

static void cleanup_zombies(uint32_t);

static int compare_processes(const process_entry_t* this, const process_entry_t* other) {
  if (this->pid < other->pid) return -1;
  else if (this->pid > other->pid) return 1;
  return 0;
}

static int compare_threads(const thread_entry_t* this, const thread_entry_t* other) {
  if (this->tid < other->tid) return -1;
  else if (this->tid > other->tid) return 1;
  return 0;
}

static inline uint64_t get_current_time() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (ts.tv_sec * NSEC_TO_SEC) + ts.tv_nsec;
}

static void get_process_name(int pid, char* name) {
  int fd;
  char proc_filename[MAX_STRING_LEN] = {0};
  char name_tmp[MAX_STRING_LEN] = {0};

  /* Get the name of the process */
  snprintf(proc_filename, MAX_STRING_LEN, "/proc/%d/exefile", pid);
  fd = open(proc_filename, O_RDONLY);
  if (fd == -1) {
    /* Some processes may die before we open this file. Ignore those errors. */
    if (errno != ESRCH) {
      ERROR("Failed to open %s (%s)", proc_filename, strerror(errno));
    }
    sstrncpy(name, "(unknown)", MAX_STRING_LEN);
    return;
  }

  ssize_t bytes_read = read(fd, name_tmp, MAX_STRING_LEN - 1);
  if (bytes_read > 0) {
    name_tmp[bytes_read] = '\0';
    char* stripped_name = strrchr(name_tmp, '/');
    sstrncpy(name, stripped_name ? stripped_name + 1: name_tmp, MAX_STRING_LEN);
  } else {
    sstrncpy(name, "(unknown)", MAX_STRING_LEN);
  }

  close(fd);
}

static uint64_t get_idle_time(int fd) {
  int ret;
  uint64_t idle_time = 0;
  procfs_status thread_status;
  for (int i = 1; i <= num_cpus; i++) {
    thread_status.tid = i;
    ret = devctl(fd, DCMD_PROC_TIDSTATUS, &thread_status, sizeof(thread_status), NULL);
    if (ret != EOK) {
      ERROR("Failed to get idle time");
      continue;
    }
    idle_time += thread_status.sutime;
  }
  return idle_time;
}

/* Returns 0 if the entry to add is equal to the entry in the top list
 *        -1 if the entry to add is less than the entry in the top list
 *         1 if the entry to add is greater than the entry in the top list
 */
static int compare_top_entries(void* entry, int top_entries_index, top_category_t cat) {
  float result = 0.0;

  switch (cat) {
    case TOP_CPU_PROCESSES:
      result = ((process_entry_t*)entry)->cpu_stats.cpu_usage -
              top_cpu_processes.entries[top_entries_index]->cpu_stats.cpu_usage;
    break;
    case TOP_CPU_THREADS:
      result = ((thread_entry_t*)entry)->cpu_stats.cpu_usage -
              top_cpu_threads.entries[top_entries_index]->cpu_stats.cpu_usage;
    break;
    case TOP_MEMORY_PROCESSES:
      result = (float)(((process_entry_t*)entry)->memory.total) -
              (float)(top_memory_processes.entries[top_entries_index]->memory.total);
    break;
  }
  return result < 0 ? -1 : result > 0 ? 1 : 0;
}

static int find_position_to_insert(void* entry, int array_size, top_category_t cat) {
  int low = 0;
  int high = array_size - 1;
  int mid;
  int result;

  while (low <= high) {
    mid = low + (high - low) / 2;
    result = compare_top_entries(entry, mid, cat);
    if (result == 0) {
      return mid;
    } else if (result > 0) {
      high = mid - 1;
    } else {
      low = mid + 1;
    }
  }

  return low;
}

static bool check_entry_if_top_user(void* entry, top_category_t cat) {
  bool result = true;

  switch (cat) {
    case TOP_CPU_PROCESSES:
      if (top_cpu_processes.entries[num_top_users_to_monitor - 1])
        result = ((process_entry_t*)entry)->cpu_stats.cpu_usage > top_cpu_processes.entries[num_top_users_to_monitor - 1]->cpu_stats.cpu_usage;
      break;
    case TOP_CPU_THREADS:
      if (top_cpu_threads.entries[num_top_users_to_monitor - 1])
        result = ((thread_entry_t*)entry)->cpu_stats.cpu_usage > top_cpu_threads.entries[num_top_users_to_monitor - 1]->cpu_stats.cpu_usage;
      break;
    case TOP_MEMORY_PROCESSES:
      if (top_memory_processes.entries[num_top_users_to_monitor - 1])
        result = ((process_entry_t*)entry)->memory.total > top_memory_processes.entries[num_top_users_to_monitor - 1]->memory.total;
      break;
  }

  return result;
}

static void insert_entry_at_pos_in_top_users(void* entry, int pos, void* top_users) {
  if (pos == -1) {
    *((char**)(top_users)) = (char*)entry;
  } else {
    /* Move all the entries back, freeing the "pos" position */
    for (int i = num_top_users_to_monitor - 1; i > pos; i--) {
      *((char**)(top_users) + i) = *((char**)(top_users) + i - 1);
    }
    *((char**)(top_users) + pos) = (char*)entry;
  }
}

static void add_entry_to_top_users(void* entry, top_category_t cat) {
  void* top_users;
  int* top_users_size;

  switch (cat) {
    case TOP_CPU_PROCESSES:
      top_users = top_cpu_processes.entries;
      top_users_size = &top_cpu_processes.current_size;
      break;
    case TOP_CPU_THREADS:
      top_users = top_cpu_threads.entries;
      top_users_size = &top_cpu_threads.current_size;
      break;
    case TOP_MEMORY_PROCESSES:
      top_users = top_memory_processes.entries;
      top_users_size = &top_memory_processes.current_size;
      break;
  }

  if (*top_users_size != 0) {
    int pos = find_position_to_insert(entry, *top_users_size, cat);
    insert_entry_at_pos_in_top_users(entry, pos, top_users);
  } else {
    insert_entry_at_pos_in_top_users(entry, -1, top_users);
  }

  if (*top_users_size < num_top_users_to_monitor)
    (*top_users_size)++;
}

static void reset_top_metrics() {
  if (measure_cpu_per_process) {
    memset(top_cpu_processes.entries, 0, sizeof(process_entry_t*) * num_top_users_to_monitor);
    top_cpu_processes.current_size = 0;
  }

  if (measure_cpu_per_thread) {
    memset(top_cpu_threads.entries, 0, sizeof(thread_entry_t*) * num_top_users_to_monitor);
    top_cpu_threads.current_size = 0;
  }

  memset(top_memory_processes.entries, 0, sizeof(process_entry_t*) * num_top_users_to_monitor);
  top_memory_processes.current_size = 0;
}

static void send_notification_for_top_cpu_proc() {
  cdtime_t now = cdtime();

  notification_t n = {NOTIF_OKAY, now, "", "", "processes",
                        "", "cpu_top", "", NULL};

  int size = notify_cpu_per_thread ? top_cpu_threads.current_size : top_cpu_processes.current_size;

  /* If for some reason processes/threads are less that how many we want to notify */
  size = min(size, num_top_users_to_notify);

  char* msg = n.message;
  int msg_size = sizeof(n.message);

  if (notify_in_single_line) {
    if (notify_cpu_per_thread) {
      ssnprintf(msg, msg_size,
        "cpu top threads - thread/cpu usage(%%)/tid/parent process/pid");
    } else {
      ssnprintf(msg, msg_size,
        "cpu top processes - process/cpu usage(%%)/pid");
    }

    for (int i = 0; i < size; i++) {
      char buf[MAX_STRING_LEN] = {0};
      int buf_size = sizeof(buf);
      int bytes_written;

      if (notify_cpu_per_thread) {
        thread_entry_t* thread = top_cpu_threads.entries[i];
        bytes_written = ssnprintf(buf, buf_size, "%s/%.1f%%/%u/%s/%u",
                  thread->name ? thread->name : DEFAULT_THREAD_NAME, thread->cpu_stats.cpu_usage, thread->tid,
                  thread->parent->name ? thread->parent->name : DEFAULT_THREAD_NAME, thread->parent->pid);
        if (bytes_written < 0 || bytes_written > buf_size) {
          ERROR("Failed to write to buffer for top threads notification");
          return;
        }
      } else {
        process_entry_t* process = top_cpu_processes.entries[i];
        bytes_written = ssnprintf(buf, buf_size, "%s/%.1f%%/%u",
                  process->name, process->cpu_stats.cpu_usage, process->pid);
        if (bytes_written < 0 || bytes_written > buf_size) {
          ERROR("Failed to write to buffer for top processes notification");
          return;
        }
      }
      if (plugin_notification_meta_add_string(&n, "", buf) != 0) {
        ERROR("Error adding meta information to notification.");
        break;
      }
    }
    plugin_dispatch_notification(&n);
    if (n.meta != NULL)
      plugin_notification_meta_free(n.meta);
  } else {
    for (int i = 0; i < size; i++) {
      if (notify_cpu_per_thread) {
        thread_entry_t* thread = top_cpu_threads.entries[i];
        ssnprintf(msg, msg_size,
                  "top-cpu-thread-%d: Name: %s, TID: %d, PID: %d[%s] Total CPU time: %f",
                  i, thread->name ? thread->name : DEFAULT_THREAD_NAME, thread->tid,
                  thread->parent->pid, thread->parent->name ? thread->parent->name : DEFAULT_THREAD_NAME,
                  thread->cpu_stats.cpu_usage);
      } else {
        process_entry_t* process = top_cpu_processes.entries[i];
        ssnprintf(msg, msg_size,
                  "top-cpu-process-%d: Name: %s, PID: %d, Total CPU time: %f",
                  i, process->name, process->pid,
                  process->cpu_stats.cpu_usage);
      }
      plugin_dispatch_notification(&n);
      if (n.meta != NULL)
        plugin_notification_meta_free(n.meta);
    }
  }
}

static void send_notification_for_top_mem_proc() {
  cdtime_t now = cdtime();

  notification_t n = {NOTIF_OKAY, now, "", "", "processes",
                        "", "mem_top", "", NULL};

  char* msg = n.message;
  int msg_size = sizeof(n.message);

  if (notify_in_single_line) {
    ssnprintf(msg, msg_size,
      "memory top processes - process/total memory(KB)/heap(KB)/pid");

    for (int i = 0; i < top_memory_processes.current_size; i++) {
      char buf[MAX_STRING_LEN] = {0};
      int buf_size = sizeof(buf);
      int bytes_written;

      process_entry_t* process = top_memory_processes.entries[i];
      bytes_written = ssnprintf(buf, buf_size, "%s/%lu/%lu/%u",
                process->name, process->memory.total/1024, process->memory.heap/1024, process->pid);
      if (bytes_written < 0 || bytes_written > buf_size) {
        ERROR("Failed to write to buffer for top memory processes notification");
        return;
      }
      if (plugin_notification_meta_add_string(&n, "", buf) != 0) {
        ERROR("Error adding meta information to notification.");
        break;
      }
    }
    plugin_dispatch_notification(&n);
    if (n.meta != NULL)
      plugin_notification_meta_free(n.meta);
  } else {
    for (int i = 0; i < top_memory_processes.current_size; i++) {
      ssnprintf(msg, msg_size,
                "top-mem-%d: Name: %s, PID: %d, Memory: %lu(KB), Heap: %lu(KB)",
                i, top_memory_processes.entries[i]->name, top_memory_processes.entries[i]->pid,
                top_memory_processes.entries[i]->memory.total/1024, top_memory_processes.entries[i]->memory.heap/1024);
      plugin_dispatch_notification(&n);
      if (n.meta != NULL)
        plugin_notification_meta_free(n.meta);
    }
  }
}

static bool is_stack_section(const procfs_mapinfo* ainfo) {
  return ((ainfo->flags & MAP_STACK)
      && (ainfo->flags & PROT_WRITE)
      && (ainfo->flags & PROT_READ)
      && (ainfo->flags & MAP_PRIVATE)
      && (ainfo->flags & MAP_SYSRAM)
      && (ainfo->flags & MAP_ANON));
}

static bool is_heap_section(const procfs_mapinfo* ainfo) {
  return ((!(ainfo->flags & MAP_STACK))
      && (ainfo->flags & MAP_ANON)
      && (ainfo->flags & PROT_WRITE)
      && (ainfo->flags & PROT_READ)
      && (ainfo->flags & MAP_SYSRAM)
      && (ainfo->flags & MAP_PRIVATE));
}

static bool is_data_section(const procfs_mapinfo* ainfo) {
  return ((!(ainfo->flags & MAP_STACK))
      && (ainfo->flags & MAP_ELF)
      && (ainfo->flags & PROT_READ));
}

static bool is_code_section(const procfs_mapinfo* ainfo) {
  return (!(ainfo->flags & PROT_WRITE))
      && (ainfo->flags & PROT_EXEC)
      && (ainfo->flags & PROT_READ);
}

static bool is_code_section_bss(const procfs_mapinfo* ainfo) {
  return  ((!(ainfo->flags & PROT_EXEC))                                                                  
      && (!(ainfo->flags & PROT_WRITE))                                                                 
      && (ainfo->flags & MAP_PRIVATE)     
      && (ainfo->flags & MAP_LAZY)
      && (ainfo->flags & MAP_ELF));
}

/* Here is the main logic */
static int get_info_from_proc(process_tree_t processes) {
  int ret;
  struct dirent *dirent;
  DIR *dir;
  int pid;
  int fd;
  char buf[MAX_STRING_LEN];
  char name_buf[MAX_STRING_LEN];
  char process_name[MAX_STRING_LEN];
  procfs_mapinfo mapinfo[MAX_NUM_MAP_REGIONS];
  procfs_info info;
  int num_mapinfos;
  uint32_t num_processes = 0;

  /* Each loop the "top" entries are reset */
  reset_top_metrics();

  dir = opendir("/proc");
  if (dir != NULL) {
    while ((dirent = readdir(dir))) {
      if (isdigit(*dirent->d_name)) {
        memset(buf, 0, sizeof(buf));
        memset(name_buf, 0, sizeof(name_buf));
        memset(process_name, 0, sizeof(process_name));

        pid = atoi(dirent->d_name);

        process_entry_t* process = NULL;
        process_entry_t process_key;
        process_key.pid = pid;

        ret = c_avl_get(processes, (const void*)&process_key, (void*)&process);
        if (ret != 0) {
          /* If 'c_avl_get' returns negative then we still don't have entry for
           * this PID in the process tree. Add it here.
           */
          process = calloc(1, sizeof(process_entry_t));
          if (!process) {
            ERROR("Failed to allocate memory for process entry!");
            closedir(dir);
            return -1;
          }
          process->pid = pid;

          get_process_name(pid, process_name);
          process->name = calloc(strlen(process_name) + 1, sizeof(char));
          if (!process->name) {
            ERROR("Failed to allocate memory for process name");
            free(process);
            continue;
          }
          strlcpy(process->name, process_name, strlen(process_name) + 1);

          ret = c_avl_insert(processes, (void*)process, (void*)process);
          if (ret != EOK) {
            ERROR("Failed to store to AVL tree! CPU percentage may be incorrect!");
          }
        }

        snprintf(buf, MAX_STRING_LEN, "/proc/%d/ctl", pid);
        fd = open(buf, O_RDONLY);
        if (fd == -1) {
          /* Some processes may die before we open this file. Ignore those errors. */
          if (errno != ESRCH) {
            ERROR("Failed to open %s (%s)", name_buf, strerror(errno));
          }
          continue;
        }

        /* Get the process' memory consumption. Reset the previous readings. */
        memset((void*)&process->memory, 0, sizeof(process->memory));
        /* TODO: Measuring the memory of the QVM process takes a long time (> 3s). For now skip it */
        if (strncmp(process->name, "qvm", 4) != 0) {
          /* Get the number of map entries */
          ret = devctl(fd, DCMD_PROC_MAPINFO, NULL, 0, &num_mapinfos);
          if (ret != EOK) {
            ERROR("Failed to get number of map entries for %s", buf);
            close(fd);
            continue;
          }
          if (num_mapinfos > MAX_NUM_MAP_REGIONS) {
            WARNING("Too many regions for process %d: %d", pid, num_mapinfos);
            close(fd);
            continue;
          }

          /* Get the map entries */
          ret = devctl(fd, DCMD_PROC_MAPINFO, mapinfo, sizeof(mapinfo), &num_mapinfos);
          if (ret != EOK) {
            ERROR("Failed to get map entries for %s", buf);
            close(fd);
            continue;
          }

          for (int i = 0; i < num_mapinfos; i++) {
            if (is_stack_section(&mapinfo[i])) {
              process->memory.stack += mapinfo[i].size;
            } else if (is_code_section(&mapinfo[i])) {
              process->memory.code += mapinfo[i].size;
            } else if (is_data_section(&mapinfo[i]) || is_code_section_bss(&mapinfo[i])) {
              process->memory.data += mapinfo[i].size;
            } else if (is_heap_section(&mapinfo[i])) {
              process->memory.heap += mapinfo[i].size;
            }
          }
          process->memory.total = process->memory.stack + process->memory.code + process->memory.heap + process->memory.data;

          if (check_entry_if_top_user(process, TOP_MEMORY_PROCESSES)) {
            add_entry_to_top_users(process, TOP_MEMORY_PROCESSES);
          }
        }

        ret = devctl(fd, DCMD_PROC_INFO, &info, sizeof(info), NULL);
        if (ret != EOK) {
          ERROR("Failed to get DCMD_PROC_INFO for %s", buf);
          close(fd);
          continue;
        }

        if (measure_cpu_per_process) {
          /* Get the actual time since the previous reading for this process.
           * This is done for better precision compared to using the plugin interval value.
           */
          uint64_t current_timestamp = get_current_time();
          process->cpu_stats.elapsed_time = current_timestamp - process->cpu_stats.last_elapsed_time;
          process->cpu_stats.last_elapsed_time = current_timestamp;

          /* Get the execution time for the process. Remove the execution time for idle threads of procnto. */
          uint64_t execution_time;
          if (pid == 1) {
            uint64_t idle_time = get_idle_time(fd);
            execution_time = (info.stime + info.utime - idle_time);
          } else {
            execution_time = info.stime + info.utime;
          }
          process->cpu_stats.execution_time = execution_time - process->cpu_stats.last_execution_time;
          process->cpu_stats.last_execution_time = execution_time;
          if (process->cpu_stats.elapsed_time == 0) {
            process->cpu_stats.elapsed_time = 1;
          }
          process->cpu_stats.cpu_usage = ((float)process->cpu_stats.execution_time / process->cpu_stats.elapsed_time) * 100.0;
          process->cpu_stats.cpu_usage /= num_cpus;

          if (check_entry_if_top_user(process, TOP_CPU_PROCESSES)) {
            add_entry_to_top_users(process, TOP_CPU_PROCESSES);
          }
        }

        if (measure_cpu_per_thread) {
          /* If the process is newly created, create its thread tree now */
          if (process->threads == NULL) {
            process->threads = c_avl_create((int (*)(const void *, const void *))compare_threads);
          }

          /* We don't need to get the load of the idle threads - PID 1, TID [1, num_cpus] */
          for (int i = (pid != 1) ? 1 : num_cpus + 1; i <= info.num_threads; i++) {
            procfs_status thread_status;
            thread_status.tid = i;
            ret = devctl(fd, DCMD_PROC_TIDSTATUS, &thread_status, sizeof(thread_status), NULL);
            if (ret != EOK) {
              ERROR("Failed to get DCMD_PROC_TIDSTATUS for %s", buf);
              continue;
            }

            thread_entry_t* thread = NULL;
            thread_entry_t thread_key;
            thread_key.tid = i;

            ret = c_avl_get(process->threads, (const void*)&thread_key, (void*)&thread);
            /* If a given TID for the given pid does not exist, create it now */
            if (ret != EOK) {
              thread = calloc(1, sizeof(thread_entry_t));
              if (!thread) {
                ERROR("Failed to allocate memory for thread entry!");
                continue;
              }
              thread->tid = i;
              thread->parent = process;
              ret = c_avl_insert(process->threads, (void*)thread, (void*)thread);
              if (ret != EOK) {
                ERROR("Failed to insert thread item into thread tree");
                free(thread);
                continue;
              }
            }

            uint64_t current_timestamp = get_current_time();
            thread->cpu_stats.elapsed_time = current_timestamp - thread->cpu_stats.last_elapsed_time;
            thread->cpu_stats.last_elapsed_time = current_timestamp;

            /* If the start time for a current TID is different than the initial one then
             * the thread has been joined and another one created
             */
            uint64_t execution_time = thread_status.sutime;
            if (thread->start_time != thread_status.start_time) {
              char thread_name[MAX_STRING_LEN] = {0};
              __getset_thread_name(process->pid, thread->tid, NULL, -1, thread_name, sizeof(thread_name));
              if (thread_name[0] == '\0') {
                strlcpy(thread_name, DEFAULT_THREAD_NAME, sizeof(thread_name));
              }
              char *new_name = realloc(thread->name, strlen(thread_name) + 1);
              if (!new_name) {
                ERROR("Failed to allocate memory for thread name");
                free(thread->name);
                thread->name = NULL;
              } else {
                thread->name = new_name;
                strlcpy(thread->name, thread_name, strlen(thread_name) + 1);
              }

              thread->start_time = thread_status.start_time;
              thread->cpu_stats.execution_time = execution_time;
            } else {
              thread->cpu_stats.execution_time = execution_time - thread->cpu_stats.last_execution_time;
            }
            thread->cpu_stats.last_execution_time = execution_time;
            thread->priority = thread_status.real_priority;
            if (thread->cpu_stats.elapsed_time == 0) {
              thread->cpu_stats.elapsed_time = 1;
            }
            thread->cpu_stats.cpu_usage = ((float)thread->cpu_stats.execution_time / thread->cpu_stats.elapsed_time) * 100.0;
            thread->cpu_stats.cpu_usage /= num_cpus;
            if (check_entry_if_top_user(thread, TOP_CPU_THREADS)) {
              add_entry_to_top_users(thread, TOP_CPU_THREADS);
            }
          }
        }

        num_processes++;

        close(fd);
      }
    }
    closedir(dir);
  }

  cleanup_zombies(num_processes);
  return EOK;
}

static void free_threads(process_entry_t* process) {
  if (!process->threads) return;

  int thread_count = c_avl_size(process->threads);
  thread_entry_t** threads = calloc(thread_count, sizeof(thread_entry_t*));
  if (!threads) {
    ERROR("Failed to allocate memory for freeing threads");
    return;
  }
  int count = 0;

  thread_entry_t* thread = NULL;
  c_avl_iterator_t* it = c_avl_get_iterator(process->threads);
  if (it) {
    while ((c_avl_iterator_next(it, (void*)&thread, (void*)&thread)) == 0) {
      threads[count++] = thread;
    }
    c_avl_iterator_destroy(it);
  }

  for (int i = 0; i < count; i++) {
    c_avl_remove(process->threads, threads[i], NULL, NULL);
    free(threads[i]->name);
    free(threads[i]);
  }

  free(threads);
  c_avl_destroy(process->threads);
  process->threads = NULL;
}

void cleanup_zombies(uint32_t num_processes) {
  /* If we have more than 10 processes that are in the process tree
   * but are not there in the last traversal of the /proc tree we
   * need to clean them up. We are determining the stale processes
   * by checking their "last_elapsed_time". If it is more than
   * three times the plugin interval, this means that it hasn't been
   * present in the last three loops.
   */
  if (c_avl_size(process_tree) - num_processes <= 10) {
    return;
  }

  uint64_t current_timestamp = get_current_time();
  uint64_t stale_threshold = 3 * plugin_get_interval();

  process_entry_t** stale_processes = calloc(c_avl_size(process_tree), sizeof(process_entry_t*));
  if (!stale_processes) {
    ERROR("Failed to allocate memory for stale processes cleanup");
    return;
  }

  int stale_count = 0;
  process_entry_t* process = NULL;
  c_avl_iterator_t* it = c_avl_get_iterator(process_tree);
  if (it) {
    while ((c_avl_iterator_next(it, (void*)&process, (void*)&process)) == 0) {
      if (current_timestamp > stale_threshold + process->cpu_stats.last_elapsed_time) {
        stale_processes[stale_count++] = process;
      }
    }
    c_avl_iterator_destroy(it);
  }

  for (int i = 0; i < stale_count; i++) {
    process = stale_processes[i];
    INFO("Found stale process with PID %d. Removing ...\n", process->pid);
    free_threads(process);
    c_avl_remove(process_tree, process, NULL, NULL);
    free(process->name);
    free(process);
  }

  free(stale_processes);
}

static void submit_process(const process_entry_t* process) {
  value_list_t vl = VALUE_LIST_INIT;
  value_t values[4];

  vl.values = values;
  sstrncpy(vl.plugin, "processes", sizeof(vl.plugin));
  sstrncpy(vl.plugin_instance, process->name, sizeof(vl.plugin_instance));
  sstrncpy(vl.type, "process", sizeof(vl.type));
  vl.values[0].counter = process->pid;
  vl.values[1].gauge = process->cpu_stats.cpu_usage;
  vl.values[2].counter = process->memory.total;
  vl.values_len = 3;
  plugin_dispatch_values(&vl);
}

static void submit_thread(const thread_entry_t* thread) {
  value_list_t vl = VALUE_LIST_INIT;
  value_t values[3];

  vl.values = values;
  sstrncpy(vl.plugin, "processes", sizeof(vl.plugin));
  sstrncpy(vl.plugin_instance, thread->name ? thread->name : DEFAULT_THREAD_NAME,
            sizeof(vl.plugin_instance));

  sstrncpy(vl.type, "thread", sizeof(vl.type));
  vl.values[0].counter = thread->tid;
  vl.values[1].counter = thread->parent->pid;
  vl.values[2].gauge = thread->cpu_stats.cpu_usage;
  vl.values_len = 3;
  plugin_dispatch_values(&vl);
}

static void ps_submit() {
  int size = submit_cpu_per_thread ? top_cpu_threads.current_size : top_cpu_processes.current_size;
  size = min(size, num_top_users_to_submit);

  if (submit_cpu_per_thread) {
    for (int i = 0; i < size; i++) {
      submit_thread(top_cpu_threads.entries[i]);
    }
  } else {
    for (int i = 0; i < size; i++) {
      submit_process(top_cpu_processes.entries[i]);
    }
  }
}

static int ps_config(char const *key, char const *value) {
  if (strcasecmp(key, "NotifyCpuPerThread") == 0) {
    if (IS_TRUE(value)) {
      notify_cpu_per_thread = true;
    }
  }
  if (strcasecmp(key, "SubmitCpuPerThread") == 0) {
    if (IS_TRUE(value)) {
      submit_cpu_per_thread = true;
    }
  }
  if (strcasecmp(key, "NotifyInSingleLine") == 0) {
    if (IS_TRUE(value)) {
      notify_in_single_line = true;
    }
  }
  if (strcasecmp(key, "NumTopUsersToNotify") == 0) {
    num_top_users_to_notify = atoi(value);
  }
  if (strcasecmp(key, "NumTopUsersToSubmit") == 0) {
    num_top_users_to_submit = atoi(value);
  }

  num_top_users_to_monitor = max(num_top_users_to_notify, num_top_users_to_submit);
  return 0;
}

static int ps_init() {
  measure_cpu_per_thread = notify_cpu_per_thread || submit_cpu_per_thread;
  /* If we want to notify and submit per thread then we don't need to get per process CPU info */
  measure_cpu_per_process = !(notify_cpu_per_thread && submit_cpu_per_thread);

  if (measure_cpu_per_thread) {
    top_cpu_threads.entries = (thread_entry_t**)calloc(num_top_users_to_monitor, sizeof(thread_entry_t*));
    if (top_cpu_threads.entries == NULL) {
      ERROR("Failed to allocate memory for top_cpu_threads");
      goto cleanup;
    }
  }

  if (measure_cpu_per_process) {
    top_cpu_processes.entries = (process_entry_t**)calloc(num_top_users_to_monitor, sizeof(process_entry_t*));
    if (top_cpu_processes.entries == NULL) {
      ERROR("Failed to allocate memory for top_cpu_processes");
      goto cleanup;
    }
  }

  top_memory_processes.entries = (process_entry_t**)calloc(num_top_users_to_monitor, sizeof(process_entry_t*));
  if (top_memory_processes.entries == NULL) {
    ERROR("Failed to allocate memory for top_memory_processes");
    goto cleanup;
  }

  process_tree = c_avl_create((int (*)(const void *, const void *))compare_processes);
  if (!process_tree) {
    ERROR("Failed to create process tree");
    goto cleanup;
  }

  num_cpus = sysconf(_SC_NPROCESSORS_CONF);
  if (num_cpus <= 0) {
    ERROR("Failed to get number of CPUs");
    goto cleanup;
  }

  return 0;

cleanup:
  if (top_cpu_threads.entries) {
    free(top_cpu_threads.entries);
    top_cpu_threads.entries = NULL;
  }
  if (top_cpu_processes.entries) {
    free(top_cpu_processes.entries);
    top_cpu_processes.entries = NULL;
  }
  if (top_memory_processes.entries) {
    free(top_memory_processes.entries);
    top_memory_processes.entries = NULL;
  }
  return -1;
}

static int ps_read() {
  get_info_from_proc(process_tree);

  send_notification_for_top_cpu_proc();
  send_notification_for_top_mem_proc();

  ps_submit();
  return 0;
}

static int ps_shutdown() {
  int process_count = c_avl_size(process_tree);
  if (process_count == 0) {
    c_avl_destroy(process_tree);
    goto cleanup_arrays;
  }

  process_entry_t** all_processes = calloc(process_count, sizeof(process_entry_t*));
  if (!all_processes) {
    ERROR("Failed to allocate memory for shutdown cleanup");
    c_avl_destroy(process_tree);
    goto cleanup_arrays;
  }

  int count = 0;
  process_entry_t* process = NULL;
  c_avl_iterator_t* it = c_avl_get_iterator(process_tree);
  if (it) {
    while (c_avl_iterator_next(it, (void*)&process, (void*)&process) == 0) {
      all_processes[count++] = process;
    }
    c_avl_iterator_destroy(it);
  }

  for (int i = 0; i < count; i++) {
    process = all_processes[i];
    free_threads(process);
    c_avl_remove(process_tree, process, NULL, NULL);
    free(process->name);
    free(process);
  }

  free(all_processes);
  c_avl_destroy(process_tree);

cleanup_arrays:
  if (top_cpu_processes.entries) {
    free(top_cpu_processes.entries);
    top_cpu_processes.entries = NULL;
  }
  if (top_cpu_threads.entries) {
    free(top_cpu_threads.entries);
    top_cpu_threads.entries = NULL;
  }
  if (top_memory_processes.entries) {
    free(top_memory_processes.entries);
    top_memory_processes.entries = NULL;
  }

  return 0;
}

void module_register(void) {
  plugin_register_init("processes", ps_init);
  plugin_register_config("processes", ps_config, config_keys, config_keys_num);
  plugin_register_read("processes", ps_read);
  plugin_register_shutdown("processes", ps_shutdown);
}
