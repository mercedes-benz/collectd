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
 *   - provide mount info statistics
 */

#include "collectd.h"

#include "plugin.h"
#include "utils/common/common.h"

#include <mntent.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/types.h>

#define PLUGIN_NAME "osp_mount"
#define LOG_KEY PLUGIN_NAME " plugin: "
#define NOTIF_MAX_BUF_LEN 1024
#define _MB(mem) ((mem) / 1024 / 1024)
#define SUBMIT_VARS(...)                                                       \
  plugin_dispatch_multivalue(vl, 0, DS_TYPE_DERIVE, __VA_ARGS__, NULL)

// Function pointers - need for tests
static int (*notify)(char *, int);
static FILE *(*setmntent_osp)(const char *, const char *);
static int (*endmntent_osp)(FILE *);
static int (*statvfs_osp)(const char *, struct statvfs *);

/**************************************************************
 * rbtree
 * Authors:
 *   Dmytro Nikitin <dmytro dot nikitin at intellias.com>
 **************************************************************/
typedef enum { RED, BLACK } Color;

struct osp_mount_node {
  char m_mnt_fsname[20];
  char m_mnt_dir[100];
  uint32_t m_size;
  uint32_t m_free_size;
  Color m_color;
  struct osp_mount_node *m_left;
  struct osp_mount_node *m_right;
  struct osp_mount_node *m_parent;
};

typedef struct osp_mount_node Node;

static bool osp_mount_is_red(Node *node) {
  if (node == NULL) {
    return false;
  }

  if (node->m_color == RED)
    return true;

  return false;
}

static void osp_mount_flip_colors(Node *node) {
  node->m_color = RED;
  node->m_left->m_color = BLACK;
  node->m_right->m_color = BLACK;
}

static Node *osp_mount_rotate_left(Node *node) {
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

static Node *osp_mount_rotate_right(Node *node) {
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

static Node *osp_mount_insert(Node *node, char *mnt_fsname, char *mnt_dir,
                              uint32_t _size, uint32_t free_size) {
  if (node == NULL) {
    node = (Node *)malloc(sizeof(Node));
    node->m_color = RED;
    sstrncpy(node->m_mnt_fsname, mnt_fsname, sizeof(node->m_mnt_fsname));
    sstrncpy(node->m_mnt_dir, mnt_dir, sizeof(node->m_mnt_dir));
    node->m_size = _size;
    node->m_free_size = free_size;
    node->m_left = NULL;
    node->m_right = NULL;
    node->m_parent = NULL;
  } else if (strncmp(node->m_mnt_fsname, mnt_fsname,
                     strlen(node->m_mnt_fsname)) > 0) {
    node->m_left =
        osp_mount_insert(node->m_left, mnt_fsname, mnt_dir, _size, free_size);
    node->m_left->m_parent = node;
  } else if (strncmp(node->m_mnt_fsname, mnt_fsname,
                     strlen(node->m_mnt_fsname)) < 0) {
    node->m_right =
        osp_mount_insert(node->m_right, mnt_fsname, mnt_dir, _size, free_size);
    node->m_right->m_parent = node;
  }

  if (osp_mount_is_red(node->m_right) && !osp_mount_is_red(node->m_left))
    node = osp_mount_rotate_left(node);
  if (osp_mount_is_red(node->m_left) && osp_mount_is_red(node->m_left->m_left))
    node = osp_mount_rotate_right(node);
  if (osp_mount_is_red(node->m_left) && osp_mount_is_red(node->m_right))
    osp_mount_flip_colors(node);

  return node;
}

static void osp_mount_removeTree(Node *root) {
  assert((root != NULL) && "osp_mount_removeTree root is NULL");
  if (root == NULL) {
    return;
  }
  if (root->m_left)
    osp_mount_removeTree(root->m_left);

  if (root->m_right)
    osp_mount_removeTree(root->m_right);

  free(root);
}

static Node *osp_mount_searchNode(Node *root, char *key) {
  Node *result = NULL;
  if (root != NULL) {
    if (strncmp(root->m_mnt_fsname, key, strlen(root->m_mnt_fsname)) > 0) {
      if (root->m_left)
        result = osp_mount_searchNode(root->m_left, key);
    } else if (strncmp(root->m_mnt_fsname, key, strlen(root->m_mnt_fsname)) <
               0) {
      if (root->m_right)
        result = osp_mount_searchNode(root->m_right, key);
    } else {
      result = root;
    }
  }
  return result;
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
 * mount info: fsname <mnt_fsname>, mount dir <mnt_dir>, size <_size>, free size
 * <free size>; <mnt_dir> <_size> <free size> info => "mount info:" tmpbuf =>
 * "fsname <mnt_fsname>, mount dir <mnt_dir>, size <_size>, free size <free
 * size>"
 *
 **************************************************************/
static void osp_mount_send_data(char *buf, char *tmpbuf, int r, char *info) {
  int len_buf = strlen(buf);
  if (len_buf == 0) {
    sprintf(buf, "%s", info);
    strncat(buf, tmpbuf, r);
    strncat(buf, ";", 2);
    memset(tmpbuf, '\0', NOTIF_MAX_MSG_LEN);
    // check if enough space in the buffer for data and ";\0"
  } else if ((1022 - len_buf) > r) {
    strncat(buf, tmpbuf, r);
    strncat(buf, ";", 2);
    memset(tmpbuf, '\0', NOTIF_MAX_MSG_LEN);
  } else {
    notify(buf, strlen(buf));
    memset(buf, '\0', NOTIF_MAX_BUF_LEN);
    if (strlen(tmpbuf) > 0) {
      sprintf(buf, "%s", info);
      strncat(buf, tmpbuf, r);
      strncat(buf, ";", 2);
      memset(tmpbuf, '\0', NOTIF_MAX_MSG_LEN);
    }
  }
}

static void osp_mount_submit_vars(value_list_t *vl, char *mnt_fsname,
                                  int64_t _size, int64_t free_size) {
  int err = 0;
  char mnt_fsname_size[20] = {0};
  char mnt_fsname_free[20] = {0};
  snprintf(mnt_fsname_size, 20, "size-%s", mnt_fsname);
  snprintf(mnt_fsname_free, 20, "free_size-%s", mnt_fsname);
  err = SUBMIT_VARS(mnt_fsname_size, (derive_t)_size, mnt_fsname_free,
                    (derive_t)free_size);
  if (err != 0) {
    ERROR("osp-mount plugin: could not submit, err=%d\n", err);
  }
}

static int32_t osp_mount_submit_data(Node *node, char *buf, value_list_t *vl) {
  char tmpbuf[NOTIF_MAX_MSG_LEN] = {0};
  if (node->m_left)
    osp_mount_submit_data(node->m_left, buf, vl);

  osp_mount_submit_vars(vl, node->m_mnt_fsname, node->m_size,
                        node->m_free_size);

  memset(tmpbuf, '\0', NOTIF_MAX_MSG_LEN);
  int r = snprintf(tmpbuf, NOTIF_MAX_MSG_LEN,
                   "fsname %s, mount dir %s, size %d, free size %d",
                   node->m_mnt_fsname, node->m_mnt_dir, node->m_size,
                   node->m_free_size);
  if (r < 0 || r > NOTIF_MAX_MSG_LEN) {
    ERROR(LOG_KEY "Error while adding mount info to buffer");
    return -1;
  }
  osp_mount_send_data(buf, tmpbuf, r, "mount info: ");
  if (node->m_right)
    osp_mount_submit_data(node->m_right, buf, vl);
  return 0;
}

static int osp_mount_collectd(value_list_t *vl, Node **m_root,
                              const char *filename) {
  struct mntent *ent;
  FILE *aFile;
  struct statvfs sb;
  char buf[NOTIF_MAX_BUF_LEN] = {0};

  aFile = setmntent_osp(filename, "r");
  if (aFile == NULL) {
    ERROR(LOG_KEY "setmntent error %s", STRERRNO);
    return -1;
  }
  while (NULL != (ent = getmntent(aFile))) {
    if ((statvfs_osp(ent->mnt_dir, &sb)) == 0) {
      if (strstr(ent->mnt_fsname, "/proc") ||
          strstr(ent->mnt_fsname, "tmpfs")) {
        continue;
      }
      if (*m_root == NULL) {
        *m_root = osp_mount_insert(*m_root, ent->mnt_fsname, ent->mnt_dir,
                                   _MB(sb.f_blocks * sb.f_frsize),
                                   _MB(sb.f_bfree * sb.f_frsize));
      } else if (osp_mount_searchNode(*m_root, ent->mnt_fsname) == NULL) {
        *m_root = osp_mount_insert(*m_root, ent->mnt_fsname, ent->mnt_dir,
                                   _MB(sb.f_blocks * sb.f_frsize),
                                   _MB(sb.f_bfree * sb.f_frsize));
      }
    } else {
      ERROR(LOG_KEY "statvfs error %s", STRERRNO);
    }
  }

  if (*m_root != NULL) {
    osp_mount_submit_data(*m_root, buf, vl);

    if (strlen(buf) > 0) {
      notify(buf, strlen(buf));
    }
  }
  endmntent_osp(aFile);
  return 0;
}

//------------------------------------------------------------------------------
static int osp_mount_config(oconfig_item_t *ci) /* {{{ */
{
  INFO(LOG_KEY "osp_mount_configuration");
  return 0;
}

static int osp_mount_init(void) { return 0; }

//------------------------------------------------------------------------------
static int osp_mount_notify(char *buf, int buf_len) {
  notification_t n = {NOTIF_OKAY, cdtime(),     "", "",  PLUGIN_NAME,
                      "",         "mount-info", "", NULL};

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
static int osp_mount_read(void) {
  value_t v[1];
  value_list_t vl = VALUE_LIST_INIT;

  vl.values = v;
  vl.values_len = STATIC_ARRAY_SIZE(v);
  sstrncpy(vl.host, hostname_g, sizeof(vl.host));
  sstrncpy(vl.plugin, "osp_mount", sizeof(vl.plugin));
  sstrncpy(vl.type, "packets", sizeof(vl.type));
  vl.time = cdtime();
  notify = osp_mount_notify;
  setmntent_osp = setmntent;
  endmntent_osp = endmntent;
  statvfs_osp = statvfs;
  Node *m_root = NULL;
  osp_mount_collectd(&vl, &m_root, "/proc/mounts");
  if (m_root != NULL) {
    osp_mount_removeTree(m_root);
    m_root = NULL;
  }
  return 0;
}

//------------------------------------------------------------------------------
void module_register(void) {
  plugin_register_complex_config("osp_mount", osp_mount_config);
  plugin_register_init("osp_mount", osp_mount_init);
  plugin_register_read("osp_mount", osp_mount_read);
} /* void module_register */