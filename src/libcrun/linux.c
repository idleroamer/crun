/*
 * crun - OCI runtime written in C
 *
 * Copyright (C) 2017 Giuseppe Scrivano <giuseppe@scrivano.org>
 * libocispec is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * libocispec is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with crun.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE

#include <config.h>
#include "linux.h"
#include "utils.h"
#include <string.h>
#include <sched.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <sys/capability.h>
#include <sys/time.h>
#include <sys/resource.h>

struct linux_namespace_s
{
  const char *name;
  int value;
};

static struct linux_namespace_s namespaces[] =
  {
    {"mount", CLONE_NEWNS},
    {"cgroup", CLONE_NEWCGROUP},
    {"network", CLONE_NEWNET},
    {"ipc", CLONE_NEWIPC},
    {"pid", CLONE_NEWPID},
    {"uts", CLONE_NEWUTS},
    {"user", CLONE_NEWUSER},
    {NULL, 0}
  };

static int
find_namespace (const char *name)
{
  struct linux_namespace_s *it;
  for (it = namespaces; it->name; it++)
    if (strcmp (it->name, name) == 0)
      return it->value;
  return -1;
}


int
libcrun_set_namespaces (crun_container *container, char **err)
{
  oci_container *def = container->container_def;
  size_t i;
  int flags = 0;
  for (i = 0; i < def->linux->namespaces_len; i++)
    {
      int value = find_namespace (def->linux->namespaces[i]->type);
      if (UNLIKELY (value < 0))
        return crun_static_error (err, 0, "invalid namespace type: %s", def->linux->namespaces[i]->type);
      flags |= value;
    }

  container->unshare_flags = flags;

  if (UNLIKELY (unshare (flags) < 0))
    return crun_static_error (err, errno, "unshare");

  for (i = 0; i < def->linux->namespaces_len; i++)
    {
      int value;
      cleanup_close int fd = -1;
      if (def->linux->namespaces[i]->path == NULL)
        continue;

      value = find_namespace (def->linux->namespaces[i]->type);
      fd = open (def->linux->namespaces[i]->path, O_RDONLY);
      if (UNLIKELY (fd < 0))
        return crun_static_error (err, errno, "open '%s'", def->linux->namespaces[i]->path);

      if (UNLIKELY (setns (fd, value) < 0))
        return crun_static_error (err, errno, "setns '%s'", def->linux->namespaces[i]->path);
    }

  return 0;
}

struct propagation_flags_s
  {
    const char *name;
    int flags;
  };

static struct propagation_flags_s propagation_flags[] =
  {
    {"rshared", MS_REC | MS_SHARED},
    {"rslave", MS_REC | MS_SLAVE},
    {"rprivate", MS_REC | MS_PRIVATE},
    {"shared", MS_SHARED},
    {"slave", MS_SLAVE},
    {"private", MS_PRIVATE},
    {"unbindable", MS_UNBINDABLE},
    {"nosuid", MS_NOSUID},
    {"noexec", MS_NOEXEC},
    {"nodev", MS_NODEV},
    {"dirsync", MS_DIRSYNC},
    {"lazytime", MS_LAZYTIME},
    {"nodiratime", MS_NODIRATIME},
    {"noatime", MS_NOATIME},
    {"ro", MS_RDONLY},
    {"relatime", MS_RELATIME},
    {"strictatime", MS_STRICTATIME},
    {"synchronous", MS_SYNCHRONOUS},
    {NULL, 0}
  };

static unsigned long
get_mount_flags (const char *name)
{
  struct propagation_flags_s *it;

  for (it = propagation_flags; it->name; it++)
    if (strcmp (it->name, name) == 0)
      return it->flags;
  return 0;
}

static unsigned long
get_mount_flags_or_option (const char *name, char **option)
{
  unsigned long flags = get_mount_flags (name);
  cleanup_free char *prev = NULL;
  if (flags)
    return flags;

  prev = *option;
  if (*option)
    xasprintf (option, "%s,%s", *option, name);
  else
    *option = xstrdup (name);

  return 0;
}

int
pivot_root (const char * new_root, const char * put_old)
{
  return syscall (__NR_pivot_root, new_root, put_old);
}

static int
do_pivot (crun_container *container, const char *rootfs, char **err)
{
  int ret;
  cleanup_close int oldrootfd = open ("/", O_DIRECTORY | O_RDONLY);
  cleanup_close int newrootfd = open (rootfs, O_DIRECTORY | O_RDONLY);

  if (UNLIKELY (oldrootfd < 0))
    return crun_static_error (err, errno, "open '/'");
  if (UNLIKELY (newrootfd < 0))
    return crun_static_error (err, errno, "open '%s'", rootfs);

  ret = fchdir (newrootfd);
  if (UNLIKELY (ret < 0))
    return crun_static_error (err, errno, "fchdir '%s'", rootfs);

  ret = pivot_root (".", ".");
  if (UNLIKELY (ret < 0))
    return crun_static_error (err, errno, "pivot_root");

  ret = fchdir (oldrootfd);
  if (UNLIKELY (ret < 0))
    return crun_static_error (err, errno, "fchdir '%s'", rootfs);

  ret = mount ("", ".", "", MS_PRIVATE | MS_REC, "");
  if (UNLIKELY (ret < 0))
    return crun_static_error (err, errno, "mount oldroot rprivate '%s'", rootfs);

  ret = umount2 (".", MNT_DETACH);
  if (UNLIKELY (ret < 0))
    return crun_static_error (err, errno, "umount oldroot");

  ret = chdir ("/");
  if (UNLIKELY (ret < 0))
    return crun_static_error (err, errno, "chdir to newroot");

  return 0;
}

static int
get_default_flags (crun_container *container, const char *destination, char **data)
{
  int userflags = container->host_uid == 0 ? 0 : MS_PRIVATE | MS_REC;
  if (strcmp (destination, "/proc") == 0)
      return 0;
  if (strcmp (destination, "/dev/cgroup") == 0
      || strcmp (destination, "/sys/fs/cgroup") == 0)
    {
      *data = xstrdup ("none,name=");
      return MS_NOEXEC | MS_NOSUID | MS_STRICTATIME;
    }
  if (strcmp (destination, "/dev") == 0)
    {
      *data = xstrdup ("mode=755");
      return MS_NOEXEC | MS_STRICTATIME;
    }
  if (strcmp (destination, "/dev/shm") == 0)
    {
      *data = xstrdup ("mode=1777,size=65536k");
      return MS_NOEXEC | MS_NOSUID | MS_NODEV;
    }
  if (strcmp (destination, "/dev/mqueue") == 0)
      return MS_NOEXEC | MS_NOSUID | MS_NODEV;
  if (strcmp (destination, "/dev/pts") == 0)
    {
      if (container->host_uid == 0)
        *data = xstrdup ("newinstance,ptmxmode=0666,mode=620,gid=5");
      else
        *data = xstrdup ("newinstance,ptmxmode=0666,mode=620");
      return MS_NOEXEC | MS_NOSUID;
    }
  if (strcmp (destination, "/sys") == 0)
      return MS_NOEXEC | MS_NOSUID | MS_NODEV;

  return 0;
}

static int
do_mounts (crun_container *container, const char *rootfs, char **err)
{
  size_t i;
  int ret;
  oci_container *def = container->container_def;
  for (i = 0; i < def->mounts_len; i++)
    {
      cleanup_free char *target = NULL;
      cleanup_free char *data = NULL;
      char *type;
      char *source;
      int flags = 0;

      if (rootfs)
        xasprintf (&target, "%s/%s", rootfs, def->mounts[i]->destination + 1);
      else
        target = xstrdup (def->mounts[i]->destination);

      ret = crun_ensure_directory (target, 0755, err);
      if (UNLIKELY (ret < 0))
        return ret;

      if (def->mounts[i]->options == NULL)
        flags = get_default_flags (container, def->mounts[i]->destination, &data);
      else
        {
          size_t j;
          for (j = 0; j < def->mounts[i]->options_len; j++)
            flags |= get_mount_flags_or_option (def->mounts[i]->options[j], &data);
        }

      type = def->mounts[i]->type;

      if (strcmp (type, "bind") == 0)
        flags |= MS_BIND;

      flags &= ~MS_RDONLY;

      source = def->mounts[i]->source ? def->mounts[i]->source : type;

      if (strcmp (type, "cgroup") == 0)
        {
          /* TODO */
          continue;
        }

      ret = mount (source, target, def->mounts[i]->type, flags, data);
      if (UNLIKELY (ret < 0))
        return crun_static_error (err, errno, "mount '%s'", def->mounts[i]->destination);
    }
}

int
libcrun_set_mounts (crun_container *container, const char *rootfs, char **err)
{
  oci_container *def = container->container_def;
  int ret;
  unsigned long rootfsPropagation = 0;

  if (def->linux->rootfs_propagation)
    rootfsPropagation = get_mount_flags (def->linux->rootfs_propagation);
  else
    rootfsPropagation = MS_REC | MS_SLAVE;

  ret = mount ("", "/", "", MS_REC | rootfsPropagation, NULL);
  if (UNLIKELY (ret < 0))
    return crun_static_error (err, errno, "remount root");

  ret = mount (def->root->path, rootfs, "", MS_BIND | MS_REC | rootfsPropagation, NULL);
  if (UNLIKELY (ret < 0))
    return crun_static_error (err, errno, "mount rootfs");

  ret = do_mounts (container, rootfs, err);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = do_pivot (container, rootfs, err);
  if (UNLIKELY (ret < 0))
    return ret;

  return 0;
}

int
libcrun_set_usernamespace (crun_container *container, char **err)
{
  cleanup_free char *uid_map = NULL;
  cleanup_free char *gid_map = NULL;
  int uid_map_len, gid_map_len;
  int ret;

  if (container->host_uid == 0)
    {
      uid_map_len = xasprintf (&uid_map, "0 0 65536");
      gid_map_len = xasprintf (&gid_map, "0 0 65536");
    }
  else
    {
      uid_map_len = xasprintf (&uid_map, "0 %d 1", container->host_uid);
      gid_map_len = xasprintf (&gid_map, "0 %d 1", container->host_gid);
    }
  ret = write_file ("/proc/self/setgroups", "deny", 4, err);
  if (UNLIKELY (ret < 0))
    return ret;

  if (container->host_uid)
    {
      ret = write_file ("/proc/self/gid_map", gid_map, gid_map_len, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }

  ret = write_file ("/proc/self/uid_map", uid_map, uid_map_len, err);
  if (UNLIKELY (ret < 0))
    return ret;
}

#define CAP_TO_MASK_0(x) (1L << ((x) & 31))
#define CAP_TO_MASK_1(x) CAP_TO_MASK_0(x - 32)

struct all_caps_s
{
  unsigned long effective[2];
  unsigned long permitted[2];
  unsigned long inheritable[2];
  unsigned long ambient[2];
  unsigned long bounding[2];
};

static int
set_required_caps (struct all_caps_s *caps, int no_new_privs, char **err)
{
  unsigned long cap;
  int ret;
  struct __user_cap_header_struct hdr = { _LINUX_CAPABILITY_VERSION_3, 0 };
  struct __user_cap_data_struct data[2] = { { 0 } };

  ret = prctl (PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0);
  if (UNLIKELY (ret < 0 && !(errno == EINVAL || errno == EPERM)))
    return crun_static_error (err, errno, "prctl reset ambient");

  for (cap = 0; cap <= CAP_LAST_CAP; cap++)
    if ((cap < 32 && CAP_TO_MASK_0 (cap) & caps->ambient[0])
        || (cap >= 32 && CAP_TO_MASK_1 (cap) & caps->ambient[1]))
      {
        ret = prctl (PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, cap, 0, 0);
        if (UNLIKELY (ret < 0 && !(errno == EINVAL || errno == EPERM)))
          return crun_static_error (err, errno, "prctl ambient raise");
      }

  for (cap = 0; cap <= CAP_LAST_CAP; cap++)
    if ((cap < 32 && ((CAP_TO_MASK_0 (cap) & caps->bounding[0]) == 0))
        || (cap >= 32 && ((CAP_TO_MASK_1 (cap) & caps->bounding[1]) == 0)))
      {
        ret = prctl (PR_CAPBSET_DROP, cap, 0, 0, 0);
        if (UNLIKELY (ret < 0 && !(errno == EINVAL || errno == EPERM)))
          return crun_static_error (err, errno, "prctl drop bounding");
      }

  data[0].effective = caps->effective[0];
  data[1].effective = caps->effective[1];
  data[0].inheritable = caps->inheritable[0];
  data[1].inheritable = caps->inheritable[1];
  data[0].permitted = caps->permitted[0];
  data[1].permitted = caps->permitted[1];

  ret = capset (&hdr, data) < 0;
  if (UNLIKELY (ret < 0))
    return crun_static_error (err, errno, "capset");

  if (no_new_privs)
    if (UNLIKELY (prctl (PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0))
      return crun_static_error (err, errno, "no new privs");

  return 0;
}

static int
read_caps (unsigned long caps[2], char **values, size_t len, char **err)
{
  size_t i;
  for (i = 0; i < len; i++)
    {
      cap_value_t cap;
      if (cap_from_name (values[i], &cap) < 0)
        {
          xasprintf (err, "unknown cap: %s", values[i]);
          return -1;
        }
      if (cap < 32)
          caps[0] |= CAP_TO_MASK_0 (cap);
      else
          caps[1] |= CAP_TO_MASK_1 (cap);
    }
  return 0;
}

int
libcrun_set_caps (crun_container *container, char **err)
{
  int ret;
  struct all_caps_s caps;
  oci_container *def = container->container_def;
  memset (&caps, 0, sizeof (caps));
  if (def->process->capabilities)
    {
      ret = read_caps (caps.effective,
                       def->process->capabilities->effective,
                       def->process->capabilities->effective_len,
                       err);
      if (ret < 0)
        return ret;

      ret = read_caps (caps.inheritable,
                       def->process->capabilities->inheritable,
                       def->process->capabilities->inheritable_len,
                       err);
      if (ret < 0)
        return ret;

      ret = read_caps (caps.ambient,
                       def->process->capabilities->ambient,
                       def->process->capabilities->ambient_len,
                       err);
      if (ret < 0)
        return ret;

      ret = read_caps (caps.bounding,
                       def->process->capabilities->bounding,
                       def->process->capabilities->bounding_len,
                       err);
      if (ret < 0)
        return ret;

      ret = read_caps (caps.permitted,
                       def->process->capabilities->permitted,
                       def->process->capabilities->permitted_len,
                       err);
      if (ret < 0)
        return ret;
    }
  return set_required_caps (&caps, def->process->no_new_privileges, err);
}

struct rlimit_s
{
  const char *name;
  int value;
};

struct rlimit_s rlimits[] =
  {
    {"RLIMIT_AS", RLIMIT_AS},
    {"RLIMIT_CORE", RLIMIT_CORE},
    {"RLIMIT_CPU", RLIMIT_CPU},
    {"RLIMIT_DATA", RLIMIT_DATA},
    {"RLIMIT_FSIZE", RLIMIT_FSIZE},
    {"RLIMIT_LOCKS", RLIMIT_LOCKS},
    {"RLIMIT_MEMLOCK", RLIMIT_MEMLOCK},
    {"RLIMIT_MSGQUEUE", RLIMIT_MSGQUEUE},
    {"RLIMIT_NICE", RLIMIT_NICE},
    {"RLIMIT_NOFILE", RLIMIT_NOFILE},
    {"RLIMIT_NPROC", RLIMIT_NPROC},
    {"RLIMIT_RSS", RLIMIT_RSS},
    {"RLIMIT_RTPRIO", RLIMIT_RTPRIO},
    {"RLIMIT_RTTIME", RLIMIT_RTTIME},
    {"RLIMIT_SIGPENDING", RLIMIT_SIGPENDING},
    {"RLIMIT_STACK", RLIMIT_STACK},
    {NULL, 0}
  };

static int
get_rlimit_resource (const char *name)
{
  struct rlimit_s *it;
  for (it = rlimits; it->name; it++)
    if (strcmp (it->name, name) == 0)
      return it->value;
  return -1;
}

int
libcrun_set_rlimits (crun_container *container, char **err)
{
    oci_container *def = container->container_def;
    size_t i;
    if (def->process->rlimits == NULL)
      return 0;
    for (i = 0; i < def->process->rlimits_len; i++)
      {
        struct rlimit limit;
        char *type = def->process->rlimits[i]->type;
        int resource = get_rlimit_resource (type);
        if (UNLIKELY (resource < 0))
          return crun_static_error (err, 0, "invalid rlimit '%s'", type);
        limit.rlim_cur = def->process->rlimits[i]->soft;
        limit.rlim_max = def->process->rlimits[i]->hard;
        if (UNLIKELY (setrlimit (resource, &limit) < 0))
          return crun_static_error (err, errno, "setrlimit '%s'", type);
      }
}