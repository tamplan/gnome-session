/* session.c
 * Copyright (C) 2007 Novell, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#include <gtk/gtkmain.h>

#include <string.h>

#include "app-autostart.h"
#if 0
#include "app-resumed.h"
#endif
#include "gconf.h"
#include "gsm.h"
#include "session.h"
#include "xsmp.h"

static void append_autostart_apps     (GsmSession *session,
				       const char *dir);
static void append_saved_session_apps (GsmSession *session);
static void append_required_apps      (GsmSession *session);

static void client_saved_state         (GsmClient *client,
					gpointer   data);
static void client_request_phase2      (GsmClient *client,
					gpointer   data);
static void client_request_interaction (GsmClient *client,
					gpointer   data);
static void client_interaction_done    (GsmClient *client,
					gboolean   cancel_shutdown,
					gpointer   data);
static void client_save_yourself_done  (GsmClient *client,
					gpointer   data);
static void client_disconnected        (GsmClient *client,
					gpointer   data);

struct _GsmSession {
  /* Startup/resumed apps */
  GSList *apps;
  GHashTable *apps_by_name;

  /* Current status */
  GsmSessionPhase phase;
  guint timeout;
  GSList *pending_apps;
  gboolean shutting_down;

  /* SM clients */
  GSList *clients;

  /* When shutdown starts, all clients are put into shutdown_clients.
   * If they request phase2, they are moved from shutdown_clients to
   * phase2_clients. If they request interaction, they are appended
   * to interact_clients (the first client in interact_clients is
   * the one currently interacting). If they report that they're done,
   * they're removed from shutdown_clients/phase2_clients.
   *
   * Once shutdown_clients is empty, phase2 starts. Once phase2_clients
   * is empty, shutdown is complete.
   */
  GSList *shutdown_clients;
  GSList *interact_clients;
  GSList *phase2_clients;
};

#define GSM_SESSION_PHASE_TIMEOUT 10 /* seconds */

/**
 * gsm_session_new:
 * @failsafe: whether or not to do a failsafe session
 *
 * Creates a new session. If @failsafe is %TRUE, it will only load the
 * default session. If @failsafe is %FALSE, it will also load the
 * contents of autostart directories and saved session info.
 *
 * Return value: a new %GsmSession
 **/
GsmSession *
gsm_session_new (gboolean failsafe)
{
  GsmSession *session = g_new0 (GsmSession, 1);
  const char * const *system_config_dirs;
  const char * const *system_data_dirs;
  char *dir;
  int i;

  session->apps_by_name = g_hash_table_new (g_str_hash, g_str_equal);

  append_autostart_apps (session, GSM_DEFAULT_SESSION_DIR);
  if (failsafe)
    return session;

  /* fdo autostart dirs */
  system_config_dirs = g_get_system_config_dirs ();
  for (i = 0; system_config_dirs[i]; i++)
    {
      dir = g_build_filename (system_config_dirs[i], "autostart", NULL);
      append_autostart_apps (session, dir);
      g_free (dir);
    }

  /* legacy autostart dirs */
  system_data_dirs = g_get_system_data_dirs ();
  for (i = 0; system_data_dirs[i]; i++)
    {
      dir = g_build_filename (system_data_dirs[i], "gnome", "autostart", NULL);
      append_autostart_apps (session, dir);
      g_free (dir);

      dir = g_build_filename (system_data_dirs[i], "autostart", NULL);
      append_autostart_apps (session, dir);
      g_free (dir);
    }

  dir = g_build_filename (g_get_user_config_dir (), "autostart", NULL);
  append_autostart_apps (session, dir);
  g_free (dir);

#if 0
  append_saved_session_apps (session);
#endif

  /* We don't do this in the failsafe case, because the default
   * session should include all requirements anyway.
   */
  append_required_apps (session);

  return session;
}

static void
append_app (GsmSession *session, GsmApp *app)
{
  const char *basename = gsm_app_get_basename (app);
  GsmApp *dup = g_hash_table_lookup (session->apps_by_name, basename);

  if (dup)
    {
      /* FIXME */
      g_object_unref (app);
      return;
    }

  session->apps = g_slist_append (session->apps, app);
  g_hash_table_insert (session->apps_by_name, g_strdup (basename), app);
}

static void
append_autostart_apps (GsmSession *session, const char *path)
{
  GDir *dir;
  const char *name;

  printf ("append_autostart_apps (%s)\n", path);

  dir = g_dir_open (path, 0, NULL);
  if (!dir)
    return;

  while ((name = g_dir_read_name (dir)))
    {
      GsmApp *app;
      char *desktop_file, *client_id;

      if (!g_str_has_suffix (name, ".desktop"))
	continue;

      desktop_file = g_build_filename (path, name, NULL);
      client_id = gsm_xsmp_generate_client_id ();
      app = gsm_app_autostart_new (desktop_file, client_id);
      if (app)
	{
	  printf ("read %s\n", desktop_file);
	  append_app (session, app);
	}
      else
	printf ("could not read %s\n", desktop_file);

      g_free (desktop_file);
      g_free (client_id);
    }

  g_dir_close (dir);
}

#if 0
static void
append_modern_session_apps (GsmSession *session,
			    const char *session_filename,
			    gboolean discard)
{
  GKeyFile *saved;
  char **clients;
  gsize num_clients, i;

  saved = g_key_file_new ();
  if (!g_key_file_load_from_file (saved, session_filename, 0, NULL))
    {
      /* FIXME: error handling? */
      g_key_file_free (saved);
      return;
    }

  clients = g_key_file_get_groups (saved, &num_clients);
  for (i = 0; i < num_clients; i++)
    {
      GsmApp *app = gsm_app_resumed_new_from_session (saved, clients[i],
						      discard);
      if (app)
	append_app (session, app);
    }

  g_key_file_free (saved);
}

/* FIXME: need to make sure this only happens once */
static void
append_legacy_session_apps (GsmSession *session, const char *session_filename)
{
  GKeyFile *saved;
  int num_clients, i;

  saved = g_key_file_new ();
  if (!g_key_file_load_from_file (saved, session_filename, 0, NULL))
    {
      /* FIXME: error handling? */
      g_key_file_free (saved);
      return;
    }

  num_clients = g_key_file_get_integer (saved, "Default", "num_clients", NULL);
  for (i = 0; i < num_clients; i++)
    {
      GsmApp *app = gsm_app_resumed_new_from_legacy_session (saved, i);
      if (app)
	append_app (session, app);
    }

  g_key_file_free (saved);
}

static void
append_saved_session_apps (GsmSession *session)
{
  char *session_filename;

  /* Try resuming last session first */
  session_filename = g_build_filename (g_get_home_dir (), ".gnome2",
				       "session-state", "last", NULL);
  if (g_file_test (session_filename, G_FILE_TEST_EXISTS))
    {
      append_modern_session_apps (session, session_filename, TRUE);
      g_free (session_filename);
      return;
    }
  g_free (session_filename);

  /* Else, try resuming default session */
  session_filename = g_build_filename (g_get_home_dir (), ".gnome2",
				       "session-state", "default", NULL);
  if (g_file_test (session_filename, G_FILE_TEST_EXISTS))
    {
      append_modern_session_apps (session, session_filename, FALSE);
      g_free (session_filename);
      return;
    }
  g_free (session_filename);

  /* Finally, try resuming from the old gnome-session's files */
  session_filename = g_build_filename (g_get_home_dir (), ".gnome2",
				       "session", NULL);
  if (g_file_test (session_filename, G_FILE_TEST_EXISTS))
    {
      append_legacy_session_apps (session, session_filename);
      g_free (session_filename);
      return;
    }
  g_free (session_filename);
}
#endif

static void
append_required_apps (GsmSession *session)
{
  GSList *required_components, *r, *a;
  GConfEntry *entry;
  const char *service, *default_provider;
  GsmApp *app;
  gboolean found;

  required_components = gconf_client_all_entries (gsm_gconf_get_client (),
						  GSM_GCONF_REQUIRED_COMPONENTS_DIRECTORY, NULL);

  for (r = required_components; r; r = r->next)
    {
      entry = r->data;

      service = strrchr (entry->key, '/');
      if (!service)
	continue;
      service++;
      default_provider = gconf_value_get_string (entry->value);
      if (!default_provider)
	continue;

      for (a = session->apps, found = FALSE; a; a = a->next)
	{
	  app = a->data;

	  if (gsm_app_provides (app, service))
	    {
	      found = TRUE;
	      break;
	    }
	}

      if (!found)
	{
	  char *client_id;

	  client_id = gsm_xsmp_generate_client_id ();
	  app = gsm_app_autostart_new (default_provider, client_id);
	  g_free (client_id);
	  if (app)
	    append_app (session, app);
	  /* FIXME: else error */
	}

      gconf_entry_free (entry);
    }

  g_slist_free (required_components);
}

static void start_phase (GsmSession *session);

static void
end_phase (GsmSession *session)
{
  g_slist_free (session->pending_apps);
  session->pending_apps = NULL;

  session->phase++;
  if (session->phase < GSM_SESSION_PHASE_RUNNING)
    start_phase (session);
}

static void
app_registered (GsmApp *app, gpointer data)
{
  GsmSession *session = data;

  session->pending_apps = g_slist_remove (session->pending_apps, app);
  g_signal_handlers_disconnect_by_func (app, app_registered, session);
  if (!session->pending_apps)
    end_phase (session);
}

static gboolean
phase_timeout (gpointer data)
{
  GsmSession *session = data;
  GSList *a;

  session->timeout = 0;

  for (a = session->pending_apps; a; a = a->next)
    {
      g_warning ("Application '%s' failed to register before timeout",
		 gsm_app_get_basename (a->data));
      g_signal_handlers_disconnect_by_func (a->data, app_registered, session);

      /* FIXME: what if the app was filling in a required slot? */
    }

  end_phase (session);
  return FALSE;
}

static void
start_phase (GsmSession *session)
{
  GsmApp *app;
  GSList *a;
  GError *err = NULL;

  printf ("starting phase %d\n", session->phase);

  g_slist_free (session->pending_apps);
  session->pending_apps = NULL;

  for (a = session->apps; a; a = a->next)
    {
      app = a->data;
      if (gsm_app_get_phase (app) != session->phase)
	continue;

      if (gsm_app_is_disabled (app))
	continue;

      if (gsm_app_launch (app, &err))
	{
#if 0
	  if (session->phase < GSM_SESSION_PHASE_APPLICATION)
	    {
	      g_signal_connect (app, "registered",
				G_CALLBACK (app_registered), session);
	    }
	  if (session->phase == GSM_SESSION_PHASE_INITIALIZATION)
	    {
	      g_signal_connect (app, "exited",
				G_CALLBACK (app_registered), session);
	    }
#endif

	  session->pending_apps =
	    g_slist_prepend (session->pending_apps, app);
	}
      else
	{
	  g_warning ("Could not launch application '%s': %s",
		     gsm_app_get_basename (app), err->message);
	  g_error_free (err);
	  err = NULL;
	}
    }

  if (session->pending_apps)
    {
      if (session->phase < GSM_SESSION_PHASE_APPLICATION)
	{
	  session->timeout = g_timeout_add (GSM_SESSION_PHASE_TIMEOUT * 1000,
					    phase_timeout, session);
	}
    }
  else
    end_phase (session);
}

void
gsm_session_start (GsmSession *session)
{
  session->phase = GSM_SESSION_PHASE_INITIALIZATION;
  start_phase (session);
}

GsmSessionPhase
gsm_session_get_phase (GsmSession *session)
{
  return session->phase;
}

gboolean
gsm_session_register_client (GsmSession *session,
			     GsmClient  *client,
			     const char *id)
{
  /* FIXME: what if we're in shutdown? */

  g_debug ("Adding new client %p to session", client);

  g_signal_connect (client, "saved_state",
		    G_CALLBACK (client_saved_state), session);
  g_signal_connect (client, "request_phase2",
		    G_CALLBACK (client_request_phase2), session);
  g_signal_connect (client, "request_interaction",
		    G_CALLBACK (client_request_interaction), session);
  g_signal_connect (client, "interaction_done",
		    G_CALLBACK (client_interaction_done), session);
  g_signal_connect (client, "save_yourself_done",
		    G_CALLBACK (client_save_yourself_done), session);
  g_signal_connect (client, "disconnected",
		    G_CALLBACK (client_disconnected), session);

  session->clients = g_slist_prepend (session->clients, client);

  return FALSE;
}

static void
client_saved_state (GsmClient *client, gpointer data)
{
  /* FIXME */
}

void
gsm_session_initiate_shutdown (GsmSession *session,
			       gboolean    show_confirmation)
{
  GSList *cl;

  if (session->shutting_down)
    {
      /* already shutting down, nothing more to do */
      return;
    }

  session->shutting_down = TRUE;

  for (cl = session->clients; cl; cl = cl->next)
    {
      GsmClient *client = cl->data;

      session->shutdown_clients =
	g_slist_prepend (session->shutdown_clients, client);
      gsm_client_save_yourself (client, FALSE);
    }      
}

static void
session_shutdown_phase2 (GsmSession *session)
{
  GSList *cl;

  for (cl = session->phase2_clients; cl; cl = cl->next)
    gsm_client_save_yourself_phase2 (cl->data);
}

static void
session_cancel_shutdown (GsmSession *session)
{
  GSList *cl;

  session->shutting_down = FALSE;

  g_slist_free (session->shutdown_clients);
  session->shutdown_clients = NULL;
  g_slist_free (session->interact_clients);
  session->interact_clients = NULL;
  g_slist_free (session->phase2_clients);
  session->phase2_clients = NULL;

  for (cl = session->clients; cl; cl = cl->next)
    gsm_client_shutdown_cancelled (cl->data);
}

static void
session_shutdown (GsmSession *session)
{
  GSList *cl;

  /* FIXME: do this in reverse phase order */
  for (cl = session->clients; cl; cl = cl->next)
    gsm_client_die (cl->data);

  /* FIXME: shutdown scripts */
}

static void
client_request_phase2 (GsmClient *client, gpointer data)
{
  GsmSession *session = data;

  /* Move the client from shutdown_clients to phase2_clients */

  session->shutdown_clients =
    g_slist_remove (session->shutdown_clients, client);
  session->phase2_clients =
    g_slist_prepend (session->phase2_clients, client);
}

static void
client_request_interaction (GsmClient *client, gpointer data)
{
  GsmSession *session = data;

  session->interact_clients =
    g_slist_append (session->interact_clients, client);

  if (!session->interact_clients->next)
    gsm_client_interact (client);
}

static void
client_interaction_done (GsmClient *client, gboolean cancel_shutdown,
			 gpointer data)
{
  GsmSession *session = data;

  g_return_if_fail (session->interact_clients &&
		    (GsmClient *)session->interact_clients->data == client);

  if (cancel_shutdown)
    {
      session_cancel_shutdown (session);
      return;
    }

  /* Remove this client from interact_clients, and if there's another
   * client waiting to interact, let it interact now.
   */
  session->interact_clients =
    g_slist_remove (session->interact_clients, client);
  if (session->interact_clients)
    gsm_client_interact (session->interact_clients->data);
}

static void
client_save_yourself_done (GsmClient *client, gpointer data)
{
  GsmSession *session = data;

  session->shutdown_clients =
    g_slist_remove (session->shutdown_clients, client);
  session->interact_clients =
    g_slist_remove (session->interact_clients, client);
  session->phase2_clients =
    g_slist_remove (session->phase2_clients, client);

  if (session->shutting_down && !session->shutdown_clients)
    {
      if (session->phase2_clients)
	session_shutdown_phase2 (session);
      else
	session_shutdown (session);
    }
}

static void
client_disconnected (GsmClient *client, gpointer data)
{
  GsmSession *session = data;

  session->clients =
    g_slist_remove (session->clients, client);
  session->shutdown_clients =
    g_slist_remove (session->shutdown_clients, client);
  session->interact_clients =
    g_slist_remove (session->interact_clients, client);
  session->phase2_clients =
    g_slist_remove (session->phase2_clients, client);

  g_object_unref (client);

  if (session->shutting_down && !session->clients)
    gtk_main_quit ();
}