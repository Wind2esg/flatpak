/*
 * Copyright © 2014-2018 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"

#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/personality.h>
#include <grp.h>
#include <unistd.h>
#include <gio/gunixfdlist.h>

#include <glib/gi18n.h>

#include <gio/gio.h>
#include "libglnx/libglnx.h"

#include "flatpak-exports.h"
#include "flatpak-run.h"
#include "flatpak-proxy.h"
#include "flatpak-utils.h"
#include "flatpak-dir.h"
#include "flatpak-systemd-dbus.h"
#include "document-portal/xdp-dbus.h"
#include "lib/flatpak-error.h"

/* We don't want to export paths pointing into these, because they are readonly
   (so we can't create mountpoints there) and don't match whats on the host anyway */
const char *dont_export_in[] = {
  "/lib", "/lib32", "/lib64", "/bin", "/sbin", "/usr", "/etc", "/app", "/dev", NULL
};

static char *
make_relative (const char *base, const char *path)
{
  GString *s = g_string_new ("");

  while (*base != 0)
    {
      while (*base == '/')
        base++;

      if (*base != 0)
        g_string_append (s, "../");

      while (*base != '/' && *base != 0)
        base++;
    }

  while (*path == '/')
    path++;

  g_string_append (s, path);

  return g_string_free (s, FALSE);
}

#define FAKE_MODE_DIR -1 /* Ensure a dir, either on tmpfs or mapped parent */
#define FAKE_MODE_TMPFS 0
#define FAKE_MODE_SYMLINK G_MAXINT

typedef struct {
  char *path;
  gint mode;
} ExportedPath;

struct _FlatpakExports {
  GHashTable *hash;
  FlatpakFilesystemMode host_fs;
};

static void
exported_path_free (ExportedPath *exported_path)
{
  g_free (exported_path->path);
  g_free (exported_path);
}

FlatpakExports *
flatpak_exports_new (void)
{
  FlatpakExports *exports = g_new0 (FlatpakExports, 1);
  exports->hash = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GFreeFunc)exported_path_free);
  return exports;
}

void
flatpak_exports_free (FlatpakExports *exports)
{
  g_hash_table_destroy (exports->hash);
  g_free (exports);
}

/* Returns TRUE if the location of this export
   is not visible due to parents being exported */
static gboolean
path_parent_is_mapped (const char **keys,
                       guint n_keys,
                       GHashTable *hash_table,
                       const char *path)
{
  guint i;
  gboolean is_mapped = FALSE;

  /* The keys are sorted so shorter (i.e. parents) are first */
  for (i = 0; i < n_keys; i++)
    {
      const char *mounted_path = keys[i];
      ExportedPath *ep = g_hash_table_lookup (hash_table, mounted_path);

      if (flatpak_has_path_prefix (path, mounted_path) &&
          (strcmp (path, mounted_path) != 0))
        {
          /* FAKE_MODE_DIR has same mapped value as parent */
          if (ep->mode == FAKE_MODE_DIR)
            continue;

          is_mapped = ep->mode != FAKE_MODE_TMPFS;
        }
    }

  return is_mapped;
}

static gboolean
path_is_mapped (const char **keys,
                guint n_keys,
                GHashTable *hash_table,
                const char *path)
{
  guint i;
  gboolean is_mapped = FALSE;

  /* The keys are sorted so shorter (i.e. parents) are first */
  for (i = 0; i < n_keys; i++)
    {
      const char *mounted_path = keys[i];
      ExportedPath *ep = g_hash_table_lookup (hash_table, mounted_path);

      if (flatpak_has_path_prefix (path, mounted_path))
        {
          /* FAKE_MODE_DIR has same mapped value as parent */
          if (ep->mode == FAKE_MODE_DIR)
            continue;

          if (ep->mode == FAKE_MODE_SYMLINK)
            is_mapped = strcmp (path, mounted_path) == 0;
          else
            is_mapped = ep->mode != FAKE_MODE_TMPFS;
        }
    }

  return is_mapped;
}

static gint
compare_eps (const ExportedPath *a,
             const ExportedPath *b)
{
  return g_strcmp0 (a->path, b->path);
}

/* This differs from g_file_test (path, G_FILE_TEST_IS_DIR) which
   returns true if the path is a symlink to a dir */
static gboolean
path_is_dir (const char *path)
{
  struct stat s;

  if (lstat (path, &s) != 0)
    return FALSE;

  return S_ISDIR (s.st_mode);
}

static gboolean
path_is_symlink (const char *path)
{
  struct stat s;

  if (lstat (path, &s) != 0)
    return FALSE;

  return S_ISLNK (s.st_mode);
}

void
flatpak_exports_append_bwrap_args (FlatpakExports *exports,
                                   FlatpakBwrap *bwrap)
{
  guint n_keys;
  g_autofree const char **keys = (const char **)g_hash_table_get_keys_as_array (exports->hash, &n_keys);
  g_autoptr(GList) eps = NULL;
  GList *l;

  eps = g_hash_table_get_values (exports->hash);
  eps = g_list_sort (eps, (GCompareFunc)compare_eps);

  g_qsort_with_data (keys, n_keys, sizeof (char *), (GCompareDataFunc) flatpak_strcmp0_ptr, NULL);

  for (l = eps; l != NULL; l = l->next)
    {
      ExportedPath *ep = l->data;
      const char *path = ep->path;

      if (ep->mode == FAKE_MODE_SYMLINK)
        {
          if (!path_parent_is_mapped (keys, n_keys, exports->hash, path))
            {
              g_autofree char *resolved = flatpak_resolve_link (path, NULL);
              if (resolved)
                {
                  g_autofree char *parent = g_path_get_dirname (path);
                  g_autofree char *relative = make_relative (parent, resolved);
                  flatpak_bwrap_add_args (bwrap, "--symlink", relative, path,  NULL);
                }
            }
        }
      else if (ep->mode == FAKE_MODE_TMPFS)
        {
          /* Mount a tmpfs to hide the subdirectory, but only if there
             is a pre-existing dir we can mount the path on. */
          if (path_is_dir (path))
            {
              if (!path_parent_is_mapped (keys, n_keys, exports->hash, path))
                /* If the parent is not mapped, it will be a tmpfs, no need to mount another one */
                flatpak_bwrap_add_args (bwrap, "--dir", path, NULL);
              else
                flatpak_bwrap_add_args (bwrap, "--tmpfs", path, NULL);
            }
        }
      else if (ep->mode == FAKE_MODE_DIR)
        {
          if (path_is_dir (path))
            flatpak_bwrap_add_args (bwrap, "--dir", path, NULL);
        }
      else
        {
          flatpak_bwrap_add_args (bwrap,
                                  (ep->mode == FLATPAK_FILESYSTEM_MODE_READ_ONLY) ? "--ro-bind" : "--bind",
                                  path, path, NULL);
        }
    }

  if (exports->host_fs != 0)
    {
      if (g_file_test ("/usr", G_FILE_TEST_IS_DIR))
        flatpak_bwrap_add_args (bwrap,
                                (exports->host_fs == FLATPAK_FILESYSTEM_MODE_READ_ONLY) ? "--ro-bind" : "--bind",
                                "/usr", "/run/host/usr", NULL);
      if (g_file_test ("/etc", G_FILE_TEST_IS_DIR))
        flatpak_bwrap_add_args (bwrap,
                                (exports->host_fs == FLATPAK_FILESYSTEM_MODE_READ_ONLY) ? "--ro-bind" : "--bind",
                                "/etc", "/run/host/etc", NULL);
    }
}

gboolean
flatpak_exports_path_is_visible (FlatpakExports *exports,
                                 const char *path)
{
  guint n_keys;
  g_autofree const char **keys = (const char **)g_hash_table_get_keys_as_array (exports->hash, &n_keys);
  g_autofree char *canonical = NULL;
  g_auto(GStrv) parts = NULL;
  int i;
  g_autoptr(GString) path_builder = g_string_new ("");
  struct stat st;

  g_qsort_with_data (keys, n_keys, sizeof (char *), (GCompareDataFunc) flatpak_strcmp0_ptr, NULL);

  path = canonical = flatpak_canonicalize_filename (path);

  parts = g_strsplit (path+1, "/", -1);

  /* A path is visible in the sandbox if no parent
   * path element that is mapped in the sandbox is
   * a symlink, and the final element is mapped.
   * If any parent is a symlink we resolve that and
   * continue with that instead.
   */
  for (i = 0; parts[i] != NULL; i++)
    {
      g_string_append (path_builder, "/");
      g_string_append (path_builder, parts[i]);

      if (path_is_mapped (keys, n_keys, exports->hash, path_builder->str))
        {
          if (lstat (path_builder->str, &st) != 0)
            return FALSE;

          if (S_ISLNK (st.st_mode))
            {
              g_autofree char *resolved = flatpak_resolve_link (path_builder->str, NULL);
              g_autoptr(GString) path2_builder = NULL;
              int j;

              if (resolved == NULL)
                return FALSE;
              path2_builder = g_string_new (resolved);

              for (j = i + 1; parts[j] != NULL; j++)
                {
                  g_string_append (path2_builder, "/");
                  g_string_append (path2_builder, parts[j]);
                }


              return flatpak_exports_path_is_visible (exports, path2_builder->str);
            }
        }
      else if (parts[i+1] == NULL)
        return FALSE; /* Last part was not mapped */
    }

  return TRUE;
}

static gboolean
never_export_as_symlink (const char *path)
{
  /* Don't export /tmp as a symlink even if it is on the host, because
     that will fail with the pre-existing directory we created for /tmp,
     and anyway, it being a symlink is not useful in the sandbox */
  if (strcmp (path, "/tmp") == 0)
    return TRUE;

  return FALSE;
}

static void
do_export_path (FlatpakExports *exports,
                const char *path,
                gint mode)
{
  ExportedPath *old_ep = g_hash_table_lookup (exports->hash, path);
  ExportedPath *ep;

  ep = g_new0 (ExportedPath, 1);
  ep->path = g_strdup (path);

  if (old_ep != NULL)
    ep->mode = MAX (old_ep->mode, mode);
  else
    ep->mode = mode;

  g_hash_table_replace (exports->hash, ep->path, ep);
}


/* We use level to avoid infinite recursion */
static gboolean
_exports_path_expose (FlatpakExports *exports,
                      int mode,
                      const char *path,
                      int level)
{
  g_autofree char *canonical = NULL;
  struct stat st;
  char *slash;
  int i;

  if (level > 40) /* 40 is the current kernel ELOOP check */
    {
      g_debug ("Expose too deep, bail");
      return FALSE;
    }

  if (!g_path_is_absolute (path))
    {
      g_debug ("Not exposing relative path %s", path);
      return FALSE;
    }

  /* Check if it exists at all */
  if (lstat (path, &st) != 0)
    return FALSE;

  /* Don't expose weird things */
  if (!(S_ISDIR (st.st_mode) ||
        S_ISREG (st.st_mode) ||
        S_ISLNK (st.st_mode) ||
        S_ISSOCK (st.st_mode)))
    return FALSE;

  path = canonical = flatpak_canonicalize_filename (path);

  for (i = 0; dont_export_in[i] != NULL; i++)
    {
      /* Don't expose files in non-mounted dirs like /app or /usr, as
         they are not the same as on the host, and we generally can't
         create the parents for them anyway */
      if (flatpak_has_path_prefix (path, dont_export_in[i]))
        {
          g_debug ("skipping export for path %s", path);
          return FALSE;
        }
    }

  /* Handle any symlinks prior to the target itself. This includes path itself,
     because we expose the target of the symlink. */
  slash = canonical;
  do
    {
      slash = strchr (slash + 1, '/');
      if (slash)
        *slash = 0;

      if (path_is_symlink (path) && !never_export_as_symlink (path))
        {
          g_autofree char *resolved = flatpak_resolve_link (path, NULL);
          g_autofree char *new_target = NULL;

          if (resolved)
            {
              if (slash)
                new_target = g_build_filename (resolved, slash + 1, NULL);
              else
                new_target = g_strdup (resolved);

              if (_exports_path_expose (exports, mode, new_target, level + 1))
                {
                  do_export_path (exports, path, FAKE_MODE_SYMLINK);
                  return TRUE;
                }
            }

          return FALSE;
        }
      if (slash)
        *slash = '/';
    }
  while (slash != NULL);

  do_export_path (exports, path, mode);
  return TRUE;
}

void
flatpak_exports_add_path_expose (FlatpakExports *exports,
                                 FlatpakFilesystemMode mode,
                                 const char *path)
{
  _exports_path_expose (exports, mode, path, 0);
}

void
flatpak_exports_add_path_tmpfs (FlatpakExports *exports,
                                const char *path)
{
  _exports_path_expose (exports, FAKE_MODE_TMPFS, path, 0);
}

void
flatpak_exports_add_path_expose_or_hide (FlatpakExports *exports,
                                         FlatpakFilesystemMode mode,
                                         const char *path)
{
  if (mode == 0)
    flatpak_exports_add_path_tmpfs (exports, path);
  else
    flatpak_exports_add_path_expose (exports, mode, path);
}

void
flatpak_exports_add_path_dir (FlatpakExports *exports,
                              const char *path)
{
  _exports_path_expose (exports, FAKE_MODE_DIR, path, 0);
}

void
flatpak_exports_add_home_expose (FlatpakExports *exports,
                                 FlatpakFilesystemMode mode)
{
  exports->host_fs = mode;
}
