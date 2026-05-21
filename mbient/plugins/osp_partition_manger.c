/**
 * The MIT License
 *
 * Copyright (C) 2025 MBition GmbH
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
 *   Dmytro Nikitin <dmytro dot nikitin at intellias.com>
 *
 * OS platform plugin.
 * Features:
 *   - provide partition manager info
 */

#include "collectd.h"

#include "plugin.h"
#include "utils/common/common.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/types.h>

#include <dirent.h>

#define PLUGIN_NAME "osp_partition_manager"
#define LOG_KEY PLUGIN_NAME " plugin: "
#define NOTIF_MAX_BUF_LEN 1024

// Function pointers - need for tests
static int (*notify)(char *, int);
static DIR *(*opendir_osp)(const char *);
static struct dirent *(*readdir_osp)(DIR *);
static int (*closedir_osp)(DIR *);
static FILE *(*fopen_osp)(const char *, const char *);
static int (*fclose_osp)(FILE *);

struct _pm_counter {
  char key[10]; // key will be like device name => dev-swup (first part of file
                // dev-swup-counters.txt)
  char value[100]; // value will be string from file like 1,2,0
};

struct _pm_events {
  char key[10]; // key will be like device name => dev-swup (first part of file
                // dev-swup-events.txt)
  char value
      [100]; // value will be prepared string from the file like
             // <date_time><event>,<date_time><event>;<dev-certs>:<date_time><event>
};

struct _list {
  void *value; // value will be pm_events or pm_counter
  struct _list *next;
};

struct _list *add(struct _list *root, struct _list *node) {
  while (root->next != NULL) {
    root = root->next;
  }
  root->next = node;
  return root->next;
}

void clearList(struct _list *root) {
  struct _list *tmp = NULL;
  if (root != NULL) {
    while (root->next != NULL) {
      tmp = root->next;
      free(root->value);
      root->value = NULL;
      free(root);
      root = NULL;
      root = tmp;
    }
    free(root->value);
    free(root);
    root = NULL;
  }
}

int partition_manager_parser(char *path) {
  DIR *FD;
  struct dirent *in_file;
  FILE *entry_file;
  char dir[250] = {0};
  int dir_len = 0;
  char buf[NOTIF_MAX_MSG_LEN] = {0};
  char message_buf[1024] = {0};
  struct _pm_counter *pCounter =
      (struct _pm_counter *)malloc(sizeof(struct _pm_counter));
  struct _pm_events *pEvents =
      (struct _pm_events *)malloc(sizeof(struct _pm_events));

  struct _list *listCounterHead = NULL;
  struct _list *listEventsHead = NULL;
  struct _list *listCounterTail = NULL;
  struct _list *listEventsTail = NULL;

  struct _list *Node = NULL;
  int is_event = 0;

  memset(buf, 0, sizeof(buf));
  sprintf(dir, "%s", path);
  dir_len = strlen(dir);
  if (NULL == (FD = opendir_osp(dir))) {
    ERROR(LOG_KEY "Failed to open input directory: %s err: %s", dir, STRERRNO);
    return -1;
  }
  while ((in_file = readdir_osp(FD))) {
    if (!strncmp(in_file->d_name, ".", sizeof(in_file->d_name)))
      continue;
    if (!strncmp(in_file->d_name, "..", sizeof(in_file->d_name)))
      continue;
    strncpy(dir + dir_len, in_file->d_name, sizeof(buf) - strlen(buf));
    entry_file = fopen_osp(dir, "r");
    if (entry_file == NULL) {
      ERROR(LOG_KEY "fopen error %s", STRERRNO);
      return -1;
    }

    int new_event_file = 0;
    while (fgets(buf, 250, entry_file) != NULL) {
      char *pch;
      if ((pch = strstr(in_file->d_name, "counters")) != NULL) {
        if (pCounter == NULL) {
          pCounter = (struct _pm_counter *)malloc(sizeof(struct _pm_counter));
        }
        sstrncpy(pCounter->key, in_file->d_name,
                 strlen(in_file->d_name) - strlen(pch));
        sstrncpy(pCounter->value, buf, strlen(buf) + 1);
        Node = (struct _list *)malloc(sizeof(struct _list));
        Node->next = NULL;
        Node->value = pCounter;
        if (listCounterHead == NULL) {
          listCounterTail = listCounterHead = Node;
        } else {
          listCounterTail = add(listCounterHead, Node);
        }

        is_event = 0;
        pCounter = NULL;
      }
      if ((pch = strstr(in_file->d_name, "events")) != NULL) {
        char ch = ' ';
        if (pEvents == NULL) {
          pEvents = (struct _pm_events *)malloc(sizeof(struct _pm_events));
        }
        if (new_event_file == 0) {
          sstrncpy(pEvents->key, in_file->d_name,
                   strlen(in_file->d_name) - strlen(pch));
          new_event_file = 1;
        } else {
          memset(pEvents->key, 0, sizeof(pEvents->key));
        }
        sstrncpy(pEvents->value, buf, strlen(buf) + 1);
        sstrncpy(pEvents->value + strlen(pEvents->value), &ch, 2);
        Node = (struct _list *)malloc(sizeof(struct _list));
        Node->next = NULL;
        Node->value = pEvents;
        if (listEventsHead == NULL) {
          listEventsTail = listEventsHead = Node;
        } else {
          listEventsTail = add(listEventsHead, Node);
        }
        is_event = 1;
        pEvents = NULL;
      }
    }
    if (is_event) {
      struct _pm_events *_event = (struct _pm_events *)listEventsTail->value;
      sstrncpy(_event->value + strlen(_event->value), ";", 2);
    } else {
      struct _pm_counter *_counter =
          (struct _pm_counter *)listCounterTail->value;
      sstrncpy(_counter->value + strlen(_counter->value), ";", 2);
    }

    memset(buf, 0, sizeof(buf));
    memset(dir + dir_len, 0, strlen(in_file->d_name));
    fclose_osp(entry_file);
  }
  closedir_osp(FD);

  // Print Lists
  memset(message_buf, '\0', 1024);
  struct _list *temp = listCounterHead;
  while (temp != NULL) {
    struct _pm_counter *_counter = (struct _pm_counter *)temp->value;
    int len_buf = strlen(message_buf);
    int data_len = strlen(_counter->key) + strlen(_counter->value);
    if (len_buf == 0) {
      sprintf(message_buf, "partition manager counter info: ");
      strncat(message_buf, _counter->key, strlen(_counter->key));
      strncat(message_buf, _counter->value, strlen(_counter->value));
    } else if ((1024 - len_buf) > data_len) {
      strncat(message_buf, _counter->key, strlen(_counter->key));
      strncat(message_buf, _counter->value, strlen(_counter->value));
    } else {
      notify(message_buf, strlen(message_buf));
      memset(message_buf, '\0', 1024);
    }
    temp = temp->next;
  }

  if (strlen(message_buf) > 0) {
    notify(message_buf, strlen(message_buf));
  }

  memset(message_buf, 0, sizeof(buf));
  struct _list *temp1 = listEventsHead;
  while (temp1 != NULL) {
    char ch[2] = {':', ' '};
    struct _pm_events *_events = (struct _pm_events *)temp1->value;
    int len_buf = strlen(message_buf);
    int data_len = strlen(_events->key) + strlen(_events->value) + 2;
    if (len_buf == 0) {
      sprintf(message_buf, "partition manager events info: ");
      strncat(message_buf, _events->key, strlen(_events->key));
      if (_events->key[0] != '\0') {
        strncat(message_buf, ch, 2);
      }
      strncat(message_buf, _events->value, strlen(_events->value));
    } else if ((1022 - len_buf) > data_len) {
      strncat(message_buf, _events->key, strlen(_events->key));
      if (_events->key[0] != '\0') {
        strncat(message_buf, ch, 2);
      }
      strncat(message_buf, _events->value, strlen(_events->value));
    } else {
      notify(message_buf, strlen(message_buf));
      memset(message_buf, '\0', 1024);
    }
    temp1 = temp1->next;
  }

  if (strlen(message_buf) > 0) {
    notify(message_buf, strlen(message_buf));
  }

  clearList(listCounterHead);
  clearList(listEventsHead);
  return 0;
}

//------------------------------------------------------------------------------
static int osp_pm_config(oconfig_item_t *ci) /* {{{ */
{
  INFO(LOG_KEY "osp_pm_configuration");
  return 0;
}

static int osp_pm_init(void) { return 0; }

//------------------------------------------------------------------------------
static int osp_pm_notify(char *buf, int buf_len) {
  notification_t n = {NOTIF_OKAY, cdtime(),  "", "",  PLUGIN_NAME,
                      "",         "pm-info", "", NULL};

  if (plugin_notification_meta_add_string(&n, "", buf) != 0) {
    ERROR(LOG_KEY "Error adding meta information to notification.");
    return -1;
  }

  plugin_dispatch_notification(&n);
  if (n.meta != NULL)
    plugin_notification_meta_free(n.meta);

  return 0;
}

//------------------------------------------------------------------------------
static int osp_pm_read(void) {
  value_t v[1];
  value_list_t vl = VALUE_LIST_INIT;

  vl.values = v;
  vl.values_len = STATIC_ARRAY_SIZE(v);
  sstrncpy(vl.host, hostname_g, sizeof(vl.host));
  sstrncpy(vl.plugin, "osp_pm", sizeof(vl.plugin));
  sstrncpy(vl.type, "packets", sizeof(vl.type));
  vl.time = cdtime();
  notify = osp_pm_notify;
  opendir_osp = opendir;
  readdir_osp = readdir;
  closedir_osp = closedir;
  fopen_osp = fopen;
  fclose_osp = fclose;
  partition_manager_parser("/mnt/backup/partition-manager/");
  return 0;
}

//------------------------------------------------------------------------------
void module_register(void) {
  plugin_register_complex_config("osp_pm", osp_pm_config);
  plugin_register_init("osp_pm", osp_pm_init);
  plugin_register_read("osp_pm", osp_pm_read);
} /* void module_register */