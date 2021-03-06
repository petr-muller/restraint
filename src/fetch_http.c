/*  
    This file is part of Restraint.

    Restraint is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Restraint is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Restraint.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include <glib/gprintf.h>
#include <gio/gio.h>
#include <fcntl.h>
#include <archive.h>
#include <archive_entry.h>
#include <unistd.h>
#include <libsoup/soup.h>
#include <libsoup/soup-uri.h>

#include "fetch.h"
#include "fetch_http.h"

static SoupSession *session;

static ssize_t
myread(struct archive *a, void *client_data, const void **abuf)
{
    FetchData *fetch_data = client_data;
    gsize len = 0;
    *abuf = fetch_data->buf;

    GError *error = NULL;

    len = g_input_stream_read (fetch_data->istream, fetch_data->buf, sizeof (fetch_data->buf),
                               NULL, &error);

    if (error) {
        archive_set_error(fetch_data->a, error->code, "%s", error->message);
        return -1;
    }
    if (len == 0)
        return 0;
    return len;
}

static gboolean
myopen(FetchData *fetch_data, GError **error)
{
    g_return_val_if_fail(fetch_data != NULL, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
    SoupRequestHTTP *reqh;

    GError *tmp_error = NULL;

    reqh = soup_session_request_http_uri (session, "GET", fetch_data->url, NULL);
    fetch_data->istream = soup_request_send (SOUP_REQUEST (reqh), NULL, &tmp_error);

    if (tmp_error != NULL) {
        gchar *url = soup_uri_to_string (fetch_data->url, TRUE);
        g_propagate_prefixed_error(error, tmp_error,
                "While connecting to %s: ", url);
        g_free (url);
        return FALSE;
    }

    g_object_unref (reqh);
    return TRUE;
}

static int
myclose(struct archive *a, void *client_data)
{
    FetchData *fetch_data = client_data;
    GError * error = NULL;

    g_input_stream_close(fetch_data->istream,
                      NULL,
                      &error);
    g_object_unref(fetch_data->istream);
    if (error != NULL) {
        archive_set_error(fetch_data->a, error->code, "%s", error->message);
        return ARCHIVE_FATAL;
    }
    return ARCHIVE_OK;
}

static gboolean
archive_finish_callback (gpointer user_data)
{
    FetchData *fetch_data = (FetchData *) user_data;
    gint free_result;

    if (fetch_data == NULL) {
        g_warning("%s: fetch_data is NULL", __func__);
        return FALSE;
    }

    if (fetch_data->ext != NULL) {
        free_result = archive_write_free(fetch_data->ext);
        if (free_result != ARCHIVE_OK)
            g_warning("Failed to free archive_write_disk");
    }
    if (fetch_data->a != NULL) {
        free_result = archive_read_free(fetch_data->a);
        if (free_result != ARCHIVE_OK)
            g_warning("Failed to free archive_read");
    }

    if (fetch_data->finish_callback) {
        fetch_data->finish_callback (fetch_data->error,
                                     fetch_data->user_data);
    } else {
        g_clear_error(&fetch_data->error);
    }

    soup_session_abort (session);
    g_object_unref (session);
    g_slice_free(FetchData, fetch_data);
    return FALSE;
}

static gboolean
http_archive_read_callback (gpointer user_data)
{
    FetchData *fetch_data = (FetchData *) user_data;

    gint r;
    struct archive_entry *entry;
    gchar *newPath = NULL;

    r = archive_read_next_header(fetch_data->a, &entry);
    if (r == ARCHIVE_EOF) {
        if (fetch_data->extracted_cnt == 0) {
            g_set_error(&fetch_data->error, RESTRAINT_FETCH_LIBARCHIVE_ERROR, ARCHIVE_WARN,
                    "Nothing was extracted from archive");
        }
        g_idle_add (archive_finish_callback, fetch_data);
        return FALSE;
    }

    if (r != ARCHIVE_OK) {
        g_set_error(&fetch_data->error, RESTRAINT_FETCH_LIBARCHIVE_ERROR, r,
                "archive_read_next_header failed: %s", archive_error_string(fetch_data->a));
        g_idle_add (archive_finish_callback, fetch_data);
        return FALSE;
    }

    if (fetch_data->archive_entry_callback) {
        fetch_data->archive_entry_callback (archive_entry_pathname (entry),
                                            fetch_data->user_data);
    }

    const gchar *fragment = soup_uri_get_fragment(fetch_data->url);
    const gchar *entry_path = archive_entry_pathname(entry);
    if (fragment == NULL || (g_strrstr(entry_path, fragment) != NULL &&
            !(fragment[strlen(fragment)] != '/' && strlen(entry_path) ==
                strlen(fragment) + 1))
            ) {
        // Update pathname
        gchar *basename = g_path_get_basename(entry_path);
        newPath = g_build_filename(fetch_data->base_path, basename, NULL);
        archive_entry_set_pathname( entry, newPath );
        g_free(newPath);
        g_free(basename);

        if (fetch_data->keepchanges == FALSE ||
                access(archive_entry_pathname(entry), F_OK) == -1) {
            r = archive_read_extract2(fetch_data->a, entry, fetch_data->ext);
            if (r != ARCHIVE_OK) {
                g_set_error(&fetch_data->error, RESTRAINT_FETCH_LIBARCHIVE_ERROR, r,
                        "archive_read_extract2 failed: %s", archive_error_string(fetch_data->ext));
                g_idle_add (archive_finish_callback, fetch_data);
                return FALSE;
            }
            fetch_data->extracted_cnt++;
        }
    }
    return TRUE;
}

void
restraint_fetch_http (SoupURI *url,
                     const gchar *base_path,
                     gboolean keepchanges,
                     ArchiveEntryCallback archive_entry_callback,
                     FetchFinishCallback finish_callback,
                     gpointer user_data)
{
    g_return_if_fail(url != NULL);
    g_return_if_fail(base_path != NULL);

    FetchData *fetch_data = g_slice_new0(FetchData);
    fetch_data->archive_entry_callback = archive_entry_callback;
    fetch_data->finish_callback = finish_callback;
    fetch_data->user_data = user_data;
    fetch_data->url = url;
    fetch_data->base_path = base_path;
    fetch_data->extracted_cnt = 0;
    fetch_data->keepchanges = keepchanges;

    GError *tmp_error = NULL;
    gint r;

    if (keepchanges == FALSE) {
        rmrf(base_path);
    }

    session = soup_session_new();

    fetch_data->a = archive_read_new();
    if (fetch_data->a == NULL) {
        g_set_error(&fetch_data->error, RESTRAINT_FETCH_LIBARCHIVE_ERROR, 0,
                "archive_read_new failed");
        g_idle_add (archive_finish_callback, fetch_data);
        return;
    }

    fetch_data->ext = archive_write_disk_new();
    if (fetch_data->ext == NULL) {
        g_set_error(&fetch_data->error, RESTRAINT_FETCH_LIBARCHIVE_ERROR, 0,
                "archive_write_disk_new failed");
        g_idle_add (archive_finish_callback, fetch_data);
        return;
    }
    archive_read_support_filter_all(fetch_data->a);
    archive_read_support_format_all(fetch_data->a);
    gboolean open_succeeded = myopen(fetch_data, &tmp_error);
    if (!open_succeeded) {
        g_propagate_error(&fetch_data->error, tmp_error);
        g_idle_add (archive_finish_callback, fetch_data);
        return;
    }
    r = archive_read_open(fetch_data->a, fetch_data, NULL, myread, myclose);
    if (r != ARCHIVE_OK) {
        g_set_error(&fetch_data->error, RESTRAINT_FETCH_LIBARCHIVE_ERROR, r,
                "archive_read_open failed: %s", archive_error_string(fetch_data->a));
        g_idle_add (archive_finish_callback, fetch_data);
        return;
    }
    g_idle_add (http_archive_read_callback, fetch_data);
}
