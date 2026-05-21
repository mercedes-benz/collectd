#include <stdio.h>
#include <stdlib.h>
#include "liboconfig/oconfig.h"
#include "osp_mount.c"
#include "testing.h"

static int notify_counter = 0;
static int endmntent_counter = 0;
static int plugin_dispatch_multivalue_counter = 0;
const char fake_mtab[] =
    "/dev/root / ext4 ro,nodev,relatime 0 0\n"
    "devtmpfs /dev devtmpfs "
    "rw,nosuid,noexec,relatime,size=1721244k,nr_inodes=430311,mode=755 0 0\n"
    "proc /proc proc rw,nosuid,nodev,noexec,relatime 0 0\n"
    "sysfs /sys sysfs rw,nosuid,nodev,noexec,relatime 0 0\n"
    "tmpfs /run/netns tmpfs "
    "rw,nosuid,nodev,noexec,size=748584k,nr_inodes=819200,mode=755 0 0\n"
    "/dev/mapper/camera_data_encrypt "
    "/run/filesystem_storage/camera/profile_specific/videocameraapp/Camera/"
    "0a0ae3c2-8e88-4535-ae57-241beba50001 ext4 "
    "rw,nosuid,nodev,noexec,noatime,discard,prjquota,stripe=128 0 0\n"
    "/dev/vdd /mnt/persist-encrypt "
    "rw,nosuid,noexec,relatime,size=1721244k,nr_inodes=430311,mode=755 0 0\n";

const char fake_mtab_with_duplicates[] =
    "/dev/mapper/ubuntu--vg-ubuntu--lv--root /scratch/fs1 ext4 rw,relatime 0 "
    "0\n"
    "sysfs /sys sysfs rw,nosuid,nodev,noexec,relatime 0 0\n"
    "/dev/loop0 /snap/bare/5 squashfs "
    "ro,nodev,relatime,errors=continue,threads=single 0 0\n"
    "/dev/loop2 /snap/core18/2979 squashfs "
    "ro,nodev,relatime,errors=continue,threads=single 0 0\n"
    "/dev/loop5 /snap/core22/2292 squashfs "
    "ro,nodev,relatime,errors=continue,threads=single 0 0\n"
    "/dev/loop4 /snap/core24/1267 squashfs "
    "ro,nodev,relatime,errors=continue,threads=single 0 0\n"
    "/dev/loop6 /snap/core24/1349 squashfs "
    "ro,nodev,relatime,errors=continue,threads=single 0 0\n"
    "/dev/loop7 /snap/gnome-3-28-1804/198 squashfs "
    "ro,nodev,relatime,errors=continue,threads=single 0 0\n"
    "/dev/loop8 /snap/gnome-42-2204/226 squashfs "
    "ro,nodev,relatime,errors=continue,threads=single 0 0\n"
    "/dev/loop9 /snap/gnome-42-2204/247 squashfs "
    "ro,nodev,relatime,errors=continue,threads=single 0 0\n"
    "/dev/loop10 /snap/multipass/16582 squashfs "
    "ro,nodev,relatime,errors=continue,threads=single 0 0\n"
    "/dev/loop12 /snap/gtk-common-themes/1535 squashfs "
    "ro,nodev,relatime,errors=continue,threads=single 0 0\n"
    "/dev/loop11 /snap/multipass/16638 squashfs "
    "ro,nodev,relatime,errors=continue,threads=single 0 0\n"
    "/dev/loop14 /snap/snapcraft/17062 squashfs "
    "ro,nodev,relatime,errors=continue,threads=single 0 0\n"
    "/dev/loop15 /snap/snapd/25577 squashfs "
    "ro,nodev,relatime,errors=continue,threads=single 0 0\n"
    "/dev/loop16 /snap/xibo-player/108 squashfs "
    "ro,nodev,relatime,errors=continue,threads=single 0 0\n"
    "/dev/loop17 /snap/snapd/25935 squashfs "
    "ro,nodev,relatime,errors=continue,threads=single 0 0\n"
    "/dev/loop16 /snap/xibo-player_2/108 squashfs "
    "ro,nodev,relatime,errors=continue,threads=single 0 0\n"
    "/dev/mapper/ubuntu--vg-ubuntu--lv--root / ext4 rw,relatime 0 0\n"
    "/dev/mapper/ubuntu--vg-ubuntu--lv--root /scratch/fs1 ext4 rw,relatime 0 "
    "0\n"
    "/dev/mapper/ubuntu--vg-ubuntu--lv--root /snap ext4 rw,relatime 0 0\n"
    "/dev/loop3 /snap/snapcraft/17123 squashfs "
    "ro,nodev,relatime,errors=continue,threads=single 0 0\n"
    "/dev/loop3 /snap/snapcraft/17123 squashfs "
    "ro,nodev,relatime,errors=continue,threads=single 0 0\n"
    "/dev/loop13 /snap/core18/2999 squashfs "
    "ro,nodev,relatime,errors=continue,threads=single 0 0\n"
    "/dev/loop13 /snap/core18/2999 squashfs "
    "ro,nodev,relatime,errors=continue,threads=single 0 0\n";

static int osp_notify_mock(char *buf, int buf_len) { return ++notify_counter; }
static FILE *setmntent_osp_mock(const char *buf, const char *mode) {
  return fmemopen((char *)buf, strlen(buf), "r");
}
static int endmntent_osp_mock(FILE *file) {
  endmntent(file);
  return ++endmntent_counter;
}

static int statvfs_osp_mock(const char *path, struct statvfs *buf) {
  // mock free data
  buf->f_bsize = 4096;
  buf->f_frsize = 4096;
  buf->f_blocks = 1000;
  buf->f_bfree = 500;
  buf->f_bavail = 400;
  buf->f_files = 100;
  buf->f_ffree = 50;
  buf->f_favail = 40;
  buf->f_fsid = 1234;
  buf->f_flag = 0;
  buf->f_namemax = 255;
  return 0;
}
int plugin_dispatch_multivalue(value_list_t const *vl, bool store_percentage,
                               int store_type, ...) {
  ++plugin_dispatch_multivalue_counter;
  return 0;
}

/* -------------------------------------------------------------------------- */

DEF_TEST(OneSendDataTest) {
  char tmpbuf[NOTIF_MAX_MSG_LEN] = {0};
  char buf[NOTIF_MAX_BUF_LEN] = {0};
  int r = snprintf(tmpbuf, NOTIF_MAX_MSG_LEN, "devtmpfs /dev %d %d", 123, 321);
  if (r < 0 || r > NOTIF_MAX_MSG_LEN) {
    ERROR(LOG_KEY "Error while adding udp info to buffer");
    return -1;
  }
  osp_mount_send_data(buf, tmpbuf, r, "mount info: ");
  EXPECT_EQ_INT(34, strlen(buf));
  EXPECT_EQ_INT(0, strlen(tmpbuf));
  return 0;
}

/* -------------------------------------------------------------------------- */

DEF_TEST(SendFullBufferTest) {
  char tmpbuf[NOTIF_MAX_MSG_LEN] = {0};
  char buf[NOTIF_MAX_BUF_LEN] = {0};
  for (int i = 0; i < 44; i++) {
    int r =
        snprintf(tmpbuf, NOTIF_MAX_MSG_LEN, "devtmpfs /dev %d %d", 123, 321);
    if (r < 0 || r > NOTIF_MAX_MSG_LEN) {
      ERROR(LOG_KEY "Error while adding udp info to buffer");
      return -1;
    }
    osp_mount_send_data(buf, tmpbuf, r, "mount info: ");
  }

  EXPECT_EQ_INT(980, strlen(buf));
  EXPECT_EQ_INT(0, strlen(tmpbuf));
  return 0;
}

/* -------------------------------------------------------------------------- */

DEF_TEST(NotifyTest) {
  char tmpbuf[NOTIF_MAX_MSG_LEN] = {0};
  char buf[NOTIF_MAX_BUF_LEN] = {0};
  notify = osp_notify_mock;
  for (int i = 0; i < 51; i++) {
    int r =
        snprintf(tmpbuf, NOTIF_MAX_MSG_LEN, "devtmpfs /dev %d %d", 123, 321);
    if (r < 0 || r > NOTIF_MAX_MSG_LEN) {
      ERROR(LOG_KEY "Error while adding udp info to buffer");
      return -1;
    }
    osp_mount_send_data(buf, tmpbuf, r, "mount info: ");
  }

  EXPECT_EQ_INT(144, strlen(buf));
  EXPECT_EQ_INT(0, strlen(tmpbuf));
  EXPECT_EQ_INT(1, notify_counter);
  return 0;
}

/* -------------------------------------------------------------------------- */

DEF_TEST(OspMountCollectdTest) {
  notify = osp_notify_mock;
  setmntent_osp = setmntent_osp_mock;
  endmntent_osp = endmntent_osp_mock;
  statvfs_osp = statvfs_osp_mock;
  value_t v[1];
  value_list_t vl = VALUE_LIST_INIT;
  vl.values = v;
  vl.values_len = STATIC_ARRAY_SIZE(v);
  sstrncpy(vl.host, hostname_g, sizeof(vl.host));
  sstrncpy(vl.plugin, "osp_mount", sizeof(vl.plugin));
  sstrncpy(vl.type, "packets", sizeof(vl.type));
  vl.time = cdtime();
  Node *m_root = NULL;
  osp_mount_collectd(&vl, &m_root, fake_mtab);
  EXPECT_EQ_INT(1, endmntent_counter);
  EXPECT_EQ_INT(5, plugin_dispatch_multivalue_counter);
  endmntent_counter = 0;
  plugin_dispatch_multivalue_counter = 0;
  osp_mount_removeTree(m_root);
  return 0;
}

DEF_TEST(OspMountDublicateCollectdTest) {
  notify = osp_notify_mock;
  setmntent_osp = setmntent_osp_mock;
  endmntent_osp = endmntent_osp_mock;
  statvfs_osp = statvfs_osp_mock;
  value_t v[1];
  value_list_t vl = VALUE_LIST_INIT;
  vl.values = v;
  vl.values_len = STATIC_ARRAY_SIZE(v);
  sstrncpy(vl.host, hostname_g, sizeof(vl.host));
  sstrncpy(vl.plugin, "osp_mount", sizeof(vl.plugin));
  sstrncpy(vl.type, "packets", sizeof(vl.type));
  vl.time = cdtime();
  Node *m_root = NULL;
  osp_mount_collectd(&vl, &m_root, fake_mtab_with_duplicates);
  EXPECT_EQ_INT(1, endmntent_counter);
  EXPECT_EQ_INT(19, plugin_dispatch_multivalue_counter);
  endmntent_counter = 0;
  plugin_dispatch_multivalue_counter = 0;
  osp_mount_removeTree(m_root);
  return 0;
}

int main(void) {
  RUN_TEST(OneSendDataTest);
  RUN_TEST(SendFullBufferTest);
  RUN_TEST(NotifyTest);
  RUN_TEST(OspMountCollectdTest);
  RUN_TEST(OspMountDublicateCollectdTest);
  END_TEST;
}