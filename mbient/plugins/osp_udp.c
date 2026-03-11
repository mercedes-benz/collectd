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
 *   - UDP packet loss statistics
 * Credits to the authors of `memory.c` for their inspiration.
 */

#include "collectd.h"

#include "plugin.h"
#include "utils/common/common.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <mntent.h>
#include <netinet/in.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/types.h>

#define PLUGIN_NAME "osp_udp"
#define LOG_KEY PLUGIN_NAME " plugin: "
#define NOTIF_MAX_BUF_LEN 1024
#define SUBMIT_VARS(...)                                                       \
  plugin_dispatch_multivalue(vl, 0, DS_TYPE_DERIVE, __VA_ARGS__, NULL)

// Function pointers - need for tests
static int (*notify)(char *, int);
static FILE *(*fopen_osp)(const char *, const char *);
static int (*fclose_osp)(FILE *);

struct osp_udp_proc_udp {
  char bucket[10];
  char local_address[10];
  char local_port[10];
  char rem_address[10];
  char rem_port[10];
  char state[3];
  char transmit_queue[10];
  char receive_queue[10];
  char timer_active[2];
  char tm_when[10];
  char retrnsmt[10];
  uint32_t uid;
  char user_name[100];
  char timeout[10];
  char inode[70];
  char socket_reference_count[100];
  char pointer[100];
  int32_t drop;
};

/**************************************************************
 * rbtree
 * Authors:
 *   Dmytro Nikitin <dmytro dot nikitin at intellias.com>
 **************************************************************/
typedef enum { RED, BLACK } Color;

struct osp_udp_node {
  uint32_t m_key;
  uint32_t m_val;
  Color m_color;
  struct osp_udp_node *m_left;
  struct osp_udp_node *m_right;
  struct osp_udp_node *m_parent;
};

typedef struct osp_udp_node Node;

static bool osp_udp_is_red(Node *node) {
  if (node == NULL) {
    return false;
  }

  if (node->m_color == RED)
    return true;

  return false;
}

static void osp_udp_flip_colors(Node *node) {
  node->m_color = RED;
  node->m_left->m_color = BLACK;
  node->m_right->m_color = BLACK;
}

static Node *osp_udp_rotate_left(Node *node) {
  Node *balancedNode = node->m_right;
  balancedNode->m_parent = node->m_parent;

  node->m_parent = balancedNode;

  node->m_right = balancedNode->m_left;

  if (balancedNode->m_left)
    balancedNode->m_left->m_parent = node;

  balancedNode->m_left = node;

  balancedNode->m_color = node->m_color;
  node->m_color = RED;

  return balancedNode;
}

static Node *osp_udp_rotate_right(Node *node) {
  Node *balancedNode = node->m_left;
  balancedNode->m_parent = node->m_parent;
  node->m_parent = balancedNode;
  node->m_left = balancedNode->m_right;
  if (balancedNode->m_right)
    balancedNode->m_right->m_parent = node;

  balancedNode->m_right = node;
  balancedNode->m_color = node->m_color;
  node->m_color = RED;
  return balancedNode;
}

static Node *osp_udp_insert(Node *node, uint32_t key, uint32_t val) {
  if (node == NULL) {
    node = (Node *)malloc(sizeof(Node));
    node->m_color = RED;
    node->m_val = val;
    node->m_key = key;
    node->m_left = NULL;
    node->m_right = NULL;
    node->m_parent = NULL;
  } else if (node->m_key > key) {
    node->m_left = osp_udp_insert(node->m_left, key, val);
    node->m_left->m_parent = node;
  } else if (node->m_key < key) {
    node->m_right = osp_udp_insert(node->m_right, key, val);
    node->m_right->m_parent = node;
  } else // m_key == key
  {
    node->m_val += val;
  }

  if (osp_udp_is_red(node->m_right) && !osp_udp_is_red(node->m_left))
    node = osp_udp_rotate_left(node);
  if (osp_udp_is_red(node->m_left) && osp_udp_is_red(node->m_left->m_left))
    node = osp_udp_rotate_right(node);
  if (osp_udp_is_red(node->m_left) && osp_udp_is_red(node->m_right))
    osp_udp_flip_colors(node);

  return node;
}

static void osp_udp_removeTree(Node *root) {
  if (root == NULL) {
    return;
  }
  if (root->m_left)
    osp_udp_removeTree(root->m_left);

  if (root->m_right)
    osp_udp_removeTree(root->m_right);

  free(root);
}

static int osp_udp_parse_udp(struct osp_udp_proc_udp *udp, char *buf) {
  struct in_addr addr;
  uint16_t host_port = 0;
  uid_t _uid = 0;
  struct passwd *result;
  char *pch;

  // bucket
  pch = strtok(buf, " :");
  if (pch == NULL)
    return -1;
  sstrncpy(udp->bucket, pch, sizeof(udp->bucket));

  // local IP
  pch = strtok(NULL, " :");
  if (pch == NULL)
    return -1;
  sscanf(pch, "%x", &addr.s_addr);
  sstrncpy(udp->local_address, inet_ntoa(addr), sizeof(udp->local_address));

  // local Port
  pch = strtok(NULL, " :");
  if (pch == NULL)
    return -1;
  sscanf(pch, "%hx", &host_port);
  snprintf(udp->local_port, sizeof(udp->local_port), "%hu", ntohs(host_port));

  // rem IP
  pch = strtok(NULL, " :");
  if (pch == NULL)
    return -1;
  sscanf(pch, "%x", &addr.s_addr);
  sstrncpy(udp->rem_address, inet_ntoa(addr), sizeof(udp->rem_address));

  // rem Port
  pch = strtok(NULL, " :");
  if (pch == NULL)
    return -1;
  sscanf(pch, "%hx", &host_port);
  snprintf(udp->rem_port, sizeof(udp->rem_port), "%hu", ntohs(host_port));

  // state
  pch = strtok(NULL, " :");
  if (pch == NULL)
    return -1;
  sstrncpy(udp->state, pch, sizeof(udp->state));

  // transmit_queue
  pch = strtok(NULL, " :");
  if (pch == NULL)
    return -1;
  sstrncpy(udp->transmit_queue, pch, sizeof(udp->transmit_queue));

  // receive_queue
  pch = strtok(NULL, " :");
  if (pch == NULL)
    return -1;
  sstrncpy(udp->receive_queue, pch, sizeof(udp->receive_queue));

  // timer_active
  pch = strtok(NULL, " :");
  if (pch == NULL)
    return -1;
  sstrncpy(udp->timer_active, pch, sizeof(udp->timer_active));

  // tm_when
  pch = strtok(NULL, " :");
  if (pch == NULL)
    return -1;
  sstrncpy(udp->tm_when, pch, sizeof(udp->tm_when));

  // retrnsmt
  pch = strtok(NULL, " :");
  if (pch == NULL)
    return -1;
  sstrncpy(udp->retrnsmt, pch, sizeof(udp->retrnsmt));

  // uid
  pch = strtok(NULL, " :");
  if (pch == NULL)
    return -1;
  udp->uid = atoi(pch);
  sscanf(pch, "%x", &_uid);
  result = getpwuid(_uid);
  if (result != NULL) {
    sstrncpy(udp->user_name, result->pw_name, sizeof(udp->user_name));
  }

  // timeout
  pch = strtok(NULL, " :");
  if (pch == NULL)
    return -1;
  sstrncpy(udp->timeout, pch, sizeof(udp->timeout));

  // inode
  pch = strtok(NULL, " :");
  if (pch == NULL)
    return -1;
  sstrncpy(udp->inode, pch, sizeof(udp->inode));

  // socket_reference_count
  pch = strtok(NULL, " :");
  if (pch == NULL)
    return -1;
  sstrncpy(udp->socket_reference_count, pch,
           sizeof(udp->socket_reference_count));

  // pointer
  pch = strtok(NULL, " :");
  if (pch == NULL)
    return -1;
  sstrncpy(udp->pointer, pch, sizeof(udp->pointer));

  // drop
  pch = strtok(NULL, " :");
  if (pch == NULL)
    return -1;
  sscanf(pch, "%d", &udp->drop);

  return 0;
}

/**************************************************************
 *
 * Function send_data accumulate buffer for send in the DLT.
 * The max length of DLT message is 1024 char. If the data size exceeds 1024
 * characters, the data is distributed across multiple messages.
 * buf - buffer  wich will be send
 * tmpbuf - data which will be passed to the buffer
 * r - size of data
 * info - Shows what the data refers to
 * Example:
 * udp info: <uid> <name> dropped 5; <uid> <name> dropped 7; <uid> <name>
 * dropped 1
 * info => "udp info:"
 * tmpbuf => "<uid> <name> dropped 5"
 *
 **************************************************************/
static void osp_udp_send_data(char *buf, char *tmpbuf, int r, char *info) {
  int len_buf = strlen(buf);
  if (len_buf == 0) {
    sprintf(buf, "%s", info);
    strncat(buf, tmpbuf, r);
    strncat(buf, ";", 2);
    memset(tmpbuf, '\0', 256);
    // check if enough space in the buffer for data and ";\0"
  } else if ((1022 - len_buf) > r) {
    strncat(buf, tmpbuf, r);
    strncat(buf, ";", 2);
    memset(tmpbuf, '\0', 256);
  } else {
    notify(buf, strlen(buf));
    memset(buf, '\0', 1024);
    if (strlen(tmpbuf) > 0) {
      sprintf(buf, "%s", info);
      strncat(buf, tmpbuf, r);
      strncat(buf, ";", 2);
      memset(tmpbuf, '\0', 256);
    }
  }
}

static void osp_udp_submit_vars(value_list_t *vl, int uid, int drop) {
  int err = 0;
  char uid_name[20] = {0};
  char dropname[20] = {0};
  snprintf(uid_name, 20, "udp-local-uid-%d", uid);
  snprintf(dropname, 20, "udp-drops-uid-%d", uid);
  err = SUBMIT_VARS(uid_name, (derive_t)uid, dropname, (derive_t)drop);
  if (err != 0) {
    ERROR("osp-udp plugin: could not submit, err=%d\n", err);
  }
}

static int32_t osp_udp_submit_data(Node *node, char *buf, value_list_t *vl) {
  char tmpbuf[NOTIF_MAX_MSG_LEN] = {0};
  if (node->m_left)
    osp_udp_submit_data(node->m_left, buf, vl);

  osp_udp_submit_vars(vl, node->m_key, node->m_val);

  memset(tmpbuf, '\0', NOTIF_MAX_MSG_LEN);
  int r = snprintf(tmpbuf, NOTIF_MAX_MSG_LEN, "uid %d dropped %d", node->m_key,
                   node->m_val);
  if (r < 0 || r > NOTIF_MAX_MSG_LEN) {
    ERROR(LOG_KEY "Error while adding udp info to buffer");
    return -1;
  }
  osp_udp_send_data(buf, tmpbuf, r, "udp info: ");
  if (node->m_right)
    osp_udp_submit_data(node->m_right, buf, vl);
  return 0;
}

static int osp_udp_collectd(value_list_t *vl, Node **m_root, const char *filename) {
  struct osp_udp_proc_udp udp = {0};
  FILE *udpFile;
  int result = 0;
  char tmpbuf[NOTIF_MAX_MSG_LEN] = {0};
  char buf[NOTIF_MAX_BUF_LEN] = {0};

  memset(tmpbuf, '\0', NOTIF_MAX_MSG_LEN);

  udpFile = fopen_osp(filename, "r");

  if (udpFile == NULL) {
    ERROR(LOG_KEY "fopen %s failed %s", filename, STRERRNO);
    return -1;
  }

  while (fgets(tmpbuf, 250, udpFile) != NULL) {
    if (strstr(tmpbuf, "local_address") != NULL) {
      continue; // skip first string with description like st local_addr, etc
    }
    result = osp_udp_parse_udp(&udp, tmpbuf);
    if (result == -1) {
      ERROR(LOG_KEY "upd parser error buf: %s", tmpbuf);
      continue;
    }

    if (udp.drop == 0) {
      continue;
    }

    *m_root = osp_udp_insert(*m_root, udp.uid, udp.drop);
  }

  if (*m_root != NULL) {
    osp_udp_submit_data(*m_root, buf, vl);

    if (strlen(buf) > 0) {
      notify(buf, strlen(buf));
    }
  }

  fclose_osp(udpFile);
  return 0;
}

//------------------------------------------------------------------------------
static int osp_udp_config(oconfig_item_t *ci) /* {{{ */
{
  INFO(LOG_KEY "configuration");
  return 0;
}

static int osp_udp_init(void) { return 0; }

//------------------------------------------------------------------------------
static int osp_udp_notify(char *buf, int buf_len) {
  notification_t n = {NOTIF_OKAY, cdtime(),   "", "",  PLUGIN_NAME,
                      "",         "udp-info", "", NULL};

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
static int osp_udp_read(void) {
  value_t v[1];
  value_list_t vl = VALUE_LIST_INIT;

  vl.values = v;
  vl.values_len = STATIC_ARRAY_SIZE(v);
  sstrncpy(vl.host, hostname_g, sizeof(vl.host));
  sstrncpy(vl.plugin, "osp_udp", sizeof(vl.plugin));
  sstrncpy(vl.type, "packets", sizeof(vl.type));
  vl.time = cdtime();
  notify = osp_udp_notify;
  fopen_osp = fopen;
  fclose_osp = fclose;
  Node *m_root = NULL;
  osp_udp_collectd(&vl, &m_root, "/proc/net/udp");
  if (m_root != NULL) {
    osp_udp_removeTree(m_root);
    m_root = NULL;
  }

  return 0;
}

//------------------------------------------------------------------------------
void module_register(void) {
  plugin_register_complex_config("osp_udp", osp_udp_config);
  plugin_register_init("osp_udp", osp_udp_init);
  plugin_register_read("osp_udp", osp_udp_read);
} /* void module_register */