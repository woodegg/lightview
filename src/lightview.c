#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <json-glib/json-glib.h>
#include <gio/gunixsocketaddress.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX_REQUEST (1024 * 1024)
#define MAX_RESPONSE (4 * 1024 * 1024)
#define MAX_CLIENTS 16
#define LIGHTVIEW_VERSION "0.1.8"
#define RECOVERY_TIMEOUT_SECONDS 10
#define RECOVERY_BURST_SECONDS 300
#define RECOVERY_BURST_LIMIT 2
#define RESET_FLASH_SECONDS 5
#define MINIMUM_KILL_THRESHOLD_MIB 3072

typedef enum {
    ENGINE_READY,
    ENGINE_RESETTING,
    ENGINE_RECOVERING,
    ENGINE_FAILED
} EngineState;

typedef struct {
    GtkWidget *window, *entry, *back, *forward, *message, *view_box, *telemetry;
    GtkWidget *reset_button, *version_button, *version_dialog, *mode_toggle;
    WebKitWebView *view;
    WebKitWebContext *context;
    WebKitWebsiteDataManager *manager;
    WebKitSettings *settings;
    WebKitMemoryPressureSettings *pressure;
    GSocketService *service;
    GList *page_requests;
    char *socket_path, *load_error, *download_dir, *download_message;
    char *last_committed_uri, *last_termination_reason, *last_reset_at;
    guint clients, memory_limit, memory_kill_threshold, downloads_active;
    guint recovery_timeout, flash_timeout;
    guint resource_timer, resource_processes;
    guint64 web_process_generation, reset_count;
    guint64 memory_pss_kib, previous_cpu_ticks;
    guint recovery_burst_count;
    gint64 last_recovery_us, previous_cpu_sample_us;
    double cpu_percent;
    EngineState engine_state;
    gboolean private_mode, low_memory, memory_limit_explicit, browser_cache, no_images;
    gboolean sandbox_enabled, hard_reset_used, mode_toggle_syncing;
} Browser;

typedef struct {
    Browser *browser;
    GSocketConnection *connection;
    GCancellable *cancel;
    GCancellable *operation_cancel;
    GString *input;
    char *output;
    guint timeout;
    gboolean quit, page_bound, reset_cancelled;
} Request;

static void process_terminated(WebKitWebView *view,
    WebKitWebProcessTerminationReason reason, gpointer data);
static gboolean decide_policy(WebKitWebView *view, WebKitPolicyDecision *decision,
    WebKitPolicyDecisionType type, gpointer data);
static void youtube_navigation_message(WebKitUserContentManager *manager,
    WebKitJavascriptResult *result, gpointer data);
static void download_started(WebKitWebContext *context, WebKitDownload *download,
    gpointer data);
static void hard_engine_reset(Browser *b);
static gboolean start_reset(Browser *b, const char *source);
static gboolean switch_low_memory_mode(Browser *b, gboolean enabled);
static void update_ui(Browser *b);

static gboolean quit_browser(gpointer unused)
{
    (void)unused;
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}

static void request_free(Request *r)
{
    if (r->page_bound) {
        r->browser->page_requests = g_list_remove(r->browser->page_requests, r);
        r->page_bound = FALSE;
    }
    if (r->timeout) g_source_remove(r->timeout);
    g_io_stream_close(G_IO_STREAM(r->connection), NULL, NULL);
    g_object_unref(r->connection);
    g_object_unref(r->cancel);
    g_object_unref(r->operation_cancel);
    g_string_free(r->input, TRUE);
    g_free(r->output);
    r->browser->clients--;
    g_free(r);
}

static gboolean request_timeout(gpointer data)
{
    Request *r = data;
    r->timeout = 0;
    g_cancellable_cancel(r->operation_cancel);
    g_cancellable_cancel(r->cancel);
    g_io_stream_close(G_IO_STREAM(r->connection), NULL, NULL);
    /* The pending read, JavaScript, or write callback releases the request. */
    return G_SOURCE_REMOVE;
}

static void written(GObject *object, GAsyncResult *result, gpointer data)
{
    Request *r = data;
    g_output_stream_write_all_finish(G_OUTPUT_STREAM(object), result, NULL, NULL);
    if (r->quit) g_idle_add(quit_browser, NULL);
    request_free(r);
}

static void reply_full(Request *r, JsonNode *value, const char *error,
                       const char *error_code, gboolean retryable)
{
    g_autoptr(JsonBuilder) b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "ok");
    json_builder_add_boolean_value(b, error == NULL);
    json_builder_set_member_name(b, error ? "error" : "result");
    if (error) json_builder_add_string_value(b, error);
    else if (value) json_builder_add_value(b, json_node_copy(value));
    else json_builder_add_null_value(b);
    if (error_code) {
        json_builder_set_member_name(b, "error_code");
        json_builder_add_string_value(b, error_code);
        json_builder_set_member_name(b, "retryable");
        json_builder_add_boolean_value(b, retryable);
    }
    json_builder_end_object(b);
    g_autoptr(JsonNode) root = json_builder_get_root(b);
    g_autofree char *json = json_to_string(root, FALSE);
    if (strlen(json) > MAX_RESPONSE) {
        reply_full(r, NULL, "Response exceeds 4 MiB; extract a smaller result",
            "response_too_large", FALSE);
        return;
    }
    r->output = g_strconcat(json, "\n", NULL);
    g_output_stream_write_all_async(
        g_io_stream_get_output_stream(G_IO_STREAM(r->connection)),
        r->output, strlen(r->output), G_PRIORITY_DEFAULT, r->cancel, written, r);
}

static void reply(Request *r, JsonNode *value, const char *error)
{
    reply_full(r, value, error, NULL, FALSE);
}

static void reply_engine_unavailable(Request *r)
{
    reply_full(r, NULL, "WebKit engine is recovering; retry when engine_state is ready",
        "webkit_recovering", TRUE);
}

static const char *string_member(JsonObject *o, const char *name)
{
    JsonNode *n = json_object_get_member(o, name);
    return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING
        ? json_node_get_string(n) : NULL;
}

static const char *engine_state_name(EngineState state)
{
    switch (state) {
    case ENGINE_READY: return "ready";
    case ENGINE_RESETTING: return "resetting";
    case ENGINE_RECOVERING: return "recovering";
    case ENGINE_FAILED: return "failed";
    }
    return "failed";
}

static const char *termination_reason_name(WebKitWebProcessTerminationReason reason)
{
    switch (reason) {
    case WEBKIT_WEB_PROCESS_CRASHED: return "crashed";
    case WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT: return "memory-limit";
    case WEBKIT_WEB_PROCESS_TERMINATED_BY_API: return "terminated-by-api";
    }
    return "unknown";
}

static char *version_text(void)
{
    return g_strdup_printf("Lightview %s (WebKitGTK %u.%u.%u)", LIGHTVIEW_VERSION,
        webkit_get_major_version(), webkit_get_minor_version(), webkit_get_micro_version());
}

static char *timestamp_now(void)
{
    g_autoptr(GDateTime) now = g_date_time_new_now_utc();
    return g_date_time_format_iso8601(now);
}

static const char *running_mode_name(const Browser *b)
{
    if (b->private_mode && b->low_memory) return "Private / Low memory";
    if (b->private_mode) return "Private";
    if (b->low_memory) return "Low memory";
    return "Normal";
}

static const char *running_mode_short_name(const Browser *b)
{
    if (b->private_mode && b->low_memory) return "P/LM";
    if (b->private_mode) return "P";
    if (b->low_memory) return "LM";
    return "N";
}

static guint64 process_pss_kib(pid_t pid)
{
    g_autofree char *path = g_strdup_printf("/proc/%ld/smaps_rollup", (long)pid);
    g_autofree char *contents = NULL;
    if (g_file_get_contents(path, &contents, NULL, NULL)) {
        g_auto(GStrv) lines = g_strsplit(contents, "\n", -1);
        for (guint i = 0; lines[i]; i++)
            if (g_str_has_prefix(lines[i], "Pss:"))
                return g_ascii_strtoull(lines[i] + 4, NULL, 10);
    }

    g_free(g_steal_pointer(&contents));
    g_free(g_steal_pointer(&path));
    path = g_strdup_printf("/proc/%ld/statm", (long)pid);
    if (!g_file_get_contents(path, &contents, NULL, NULL)) return 0;
    g_auto(GStrv) fields = g_strsplit_set(contents, " \t\n", -1);
    guint logical = 0;
    for (guint i = 0; fields[i]; i++) {
        if (!*fields[i]) continue;
        if (logical++ == 1) {
            guint64 pages = g_ascii_strtoull(fields[i], NULL, 10);
            return pages * (guint64)sysconf(_SC_PAGESIZE) / 1024;
        }
    }
    return 0;
}

static guint64 process_cpu_ticks(pid_t pid)
{
    g_autofree char *path = g_strdup_printf("/proc/%ld/stat", (long)pid);
    g_autofree char *contents = NULL;
    if (!g_file_get_contents(path, &contents, NULL, NULL)) return 0;
    char *command_end = strrchr(contents, ')');
    if (!command_end || command_end[1] != ' ') return 0;
    g_auto(GStrv) fields = g_strsplit_set(command_end + 2, " \t\n", -1);
    guint logical = 0;
    guint64 user = 0, system = 0;
    for (guint i = 0; fields[i]; i++) {
        if (!*fields[i]) continue;
        if (logical == 11) user = g_ascii_strtoull(fields[i], NULL, 10);
        else if (logical == 12) {
            system = g_ascii_strtoull(fields[i], NULL, 10);
            break;
        }
        logical++;
    }
    return user + system;
}

static void collect_process_tree(pid_t pid, GHashTable *seen, guint64 *pss_kib,
                                 guint64 *cpu_ticks, guint *processes)
{
    gpointer key = GINT_TO_POINTER((gint)pid);
    if (pid <= 0 || g_hash_table_contains(seen, key)) return;
    g_hash_table_add(seen, key);
    (*processes)++;
    *pss_kib += process_pss_kib(pid);
    *cpu_ticks += process_cpu_ticks(pid);

    g_autofree char *path = g_strdup_printf("/proc/%ld/task/%ld/children",
        (long)pid, (long)pid);
    g_autofree char *contents = NULL;
    if (!g_file_get_contents(path, &contents, NULL, NULL)) return;
    g_auto(GStrv) children = g_strsplit_set(contents, " \t\n", -1);
    for (guint i = 0; children[i]; i++) {
        if (!*children[i]) continue;
        gint64 child = g_ascii_strtoll(children[i], NULL, 10);
        if (child > 0 && child <= G_MAXINT)
            collect_process_tree((pid_t)child, seen, pss_kib, cpu_ticks, processes);
    }
}

static gboolean sample_resource_usage(gpointer data)
{
    Browser *b = data;
    g_autoptr(GHashTable) seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    guint64 pss_kib = 0, cpu_ticks = 0;
    guint processes = 0;
    collect_process_tree(getpid(), seen, &pss_kib, &cpu_ticks, &processes);
    gint64 now = g_get_monotonic_time();
    long ticks_per_second = sysconf(_SC_CLK_TCK);
    if (b->previous_cpu_sample_us && cpu_ticks >= b->previous_cpu_ticks &&
        ticks_per_second > 0 && now > b->previous_cpu_sample_us) {
        double elapsed = (double)(now - b->previous_cpu_sample_us) / G_USEC_PER_SEC;
        b->cpu_percent = (double)(cpu_ticks - b->previous_cpu_ticks) * 100.0 /
            ((double)ticks_per_second * elapsed);
    } else b->cpu_percent = 0.0;
    b->memory_pss_kib = pss_kib;
    b->resource_processes = processes;
    b->previous_cpu_ticks = cpu_ticks;
    b->previous_cpu_sample_us = now;
    update_ui(b);
    return G_SOURCE_CONTINUE;
}

static void update_ui(Browser *b)
{
    const char *title = b->view ? webkit_web_view_get_title(b->view) : NULL;
    const char *uri = b->view ? webkit_web_view_get_uri(b->view) : NULL;
    g_autofree char *caption = g_strdup_printf(
        "%s — Lightview · MODE:%s, %.0fM, CPU %.1f%%",
        title && *title ? title : "Browser", running_mode_short_name(b),
        (double)b->memory_pss_kib / 1024.0, b->cpu_percent);
    gtk_window_set_title(GTK_WINDOW(b->window), caption);
    if (b->telemetry) {
        g_autofree char *markup = g_markup_printf_escaped(
            "<b>MODE:%s</b>, %.0fM, CPU %.1f%%", running_mode_short_name(b),
            (double)b->memory_pss_kib / 1024.0, b->cpu_percent);
        gtk_label_set_markup(GTK_LABEL(b->telemetry), markup);
    }
    if (!gtk_widget_has_focus(b->entry))
        gtk_entry_set_text(GTK_ENTRY(b->entry), uri ? uri : "");
    gboolean ready = b->engine_state == ENGINE_READY && b->view;
    gtk_widget_set_sensitive(b->entry, ready);
    if (b->reset_button) gtk_widget_set_sensitive(b->reset_button,
        b->engine_state != ENGINE_RESETTING && b->engine_state != ENGINE_RECOVERING);
    if (b->mode_toggle) gtk_widget_set_sensitive(b->mode_toggle,
        b->engine_state != ENGINE_RESETTING && b->engine_state != ENGINE_RECOVERING);
    gtk_widget_set_sensitive(b->back, ready && webkit_web_view_can_go_back(b->view));
    gtk_widget_set_sensitive(b->forward, ready && webkit_web_view_can_go_forward(b->view));
    gboolean loading = b->view && webkit_web_view_is_loading(b->view);
    const char *engine_message = NULL;
    if (b->engine_state == ENGINE_RESETTING) engine_message = "Resetting WebKit…";
    else if (b->engine_state == ENGINE_RECOVERING) engine_message = "Recovering WebKit…";
    else if (b->engine_state == ENGINE_FAILED) engine_message = "WebKit recovery failed; use Reset WebKit to retry.";
    const char *message = engine_message ? engine_message : (b->load_error ? b->load_error :
        (loading ? "Loading…" : b->download_message));
    gtk_label_set_text(GTK_LABEL(b->message), message ? message : "");
    gtk_widget_set_visible(b->message, message != NULL);
}

static gboolean reset_flash_finished(gpointer data)
{
    Browser *b = data;
    b->flash_timeout = 0;
    gtk_style_context_remove_class(gtk_widget_get_style_context(b->entry),
        "webkit-reset-flash");
    return G_SOURCE_REMOVE;
}

static void flash_reset_warning(Browser *b)
{
    if (b->flash_timeout) g_source_remove(b->flash_timeout);
    gtk_style_context_add_class(gtk_widget_get_style_context(b->entry),
        "webkit-reset-flash");
    b->flash_timeout = g_timeout_add_seconds(RESET_FLASH_SECONDS,
        reset_flash_finished, b);
}

static void notify_view(GObject *object, GParamSpec *pspec, gpointer data)
{
    (void)object; (void)pspec;
    update_ui(data);
}

static void complete_recovery(Browser *b)
{
    if (b->recovery_timeout) {
        g_source_remove(b->recovery_timeout);
        b->recovery_timeout = 0;
    }
    b->engine_state = ENGINE_READY;
    b->hard_reset_used = FALSE;
    b->web_process_generation++;
    g_clear_pointer(&b->load_error, g_free);
    g_message("WebKit recovery ready: generation=%" G_GUINT64_FORMAT
        " reset_count=%" G_GUINT64_FORMAT, b->web_process_generation, b->reset_count);
    update_ui(b);
}

static void load_changed(WebKitWebView *view, WebKitLoadEvent event, gpointer data)
{
    Browser *b = data;
    if (event == WEBKIT_LOAD_STARTED) g_clear_pointer(&b->load_error, g_free);
    if (event == WEBKIT_LOAD_COMMITTED && b->engine_state == ENGINE_READY) {
        const char *uri = webkit_web_view_get_uri(view);
        if (uri && *uri) {
            g_free(b->last_committed_uri);
            b->last_committed_uri = g_strdup(uri);
        }
    }
    if (event == WEBKIT_LOAD_FINISHED && b->engine_state == ENGINE_RECOVERING)
        complete_recovery(b);
    update_ui(b);
}

static gboolean load_failed(WebKitWebView *view, WebKitLoadEvent event,
                            const char *uri, GError *error, gpointer data)
{
    (void)view; (void)event; (void)uri;
    Browser *b = data;
    if (g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED))
        return TRUE;
    g_free(b->load_error);
    b->load_error = g_strdup(error->message);
    update_ui(b);
    return TRUE;
}

static char *safe_download_name(const char *suggested)
{
    g_autofree char *basename = g_path_get_basename(
        suggested && *suggested ? suggested : "download");
    if (g_str_equal(basename, ".") || g_str_equal(basename, "..") || !*basename)
        return g_strdup("download");
    for (char *p = basename; *p; p++)
        if (*p == '/' || *p == '\\' || ((guchar)*p < 0x20) || *p == 0x7f) *p = '_';
    return g_steal_pointer(&basename);
}

static char *available_download_path(Browser *b, const char *suggested)
{
    g_autofree char *name = safe_download_name(suggested);
    char *dot = strrchr(name, '.');
    g_autofree char *stem = NULL;
    const char *suffix = "";
    if (dot && dot != name) {
        stem = g_strndup(name, dot - name);
        suffix = dot;
    } else stem = g_strdup(name);
    for (guint n = 0; n < 10000; n++) {
        g_autofree char *candidate_name = n == 0 ? g_strdup(name) :
            g_strdup_printf("%s (%u)%s", stem, n, suffix);
        char *candidate = g_build_filename(b->download_dir, candidate_name, NULL);
        if (!g_file_test(candidate, G_FILE_TEST_EXISTS)) return candidate;
        g_free(candidate);
    }
    return NULL;
}

static gboolean decide_download_destination(WebKitDownload *download,
                                             const char *suggested, gpointer data)
{
    Browser *b = data;
    g_autofree char *path = available_download_path(b, suggested);
    if (!path) {
        webkit_download_cancel(download);
        g_free(b->download_message);
        b->download_message = g_strdup("Download failed: no available filename");
        update_ui(b);
        return TRUE;
    }
    g_autoptr(GError) error = NULL;
    g_autofree char *uri = g_filename_to_uri(path, NULL, &error);
    if (!uri) {
        webkit_download_cancel(download);
        g_free(b->download_message);
        b->download_message = g_strdup_printf("Download failed: %s", error->message);
        update_ui(b);
        return TRUE;
    }
    webkit_download_set_allow_overwrite(download, FALSE);
    webkit_download_set_destination(download, uri);
    g_object_set_data_full(G_OBJECT(download), "lightview-download-path",
        g_strdup(path), g_free);
    g_free(b->download_message);
    b->download_message = g_strdup_printf("Downloading %s…", suggested);
    update_ui(b);
    return TRUE;
}

static void download_progress(GObject *object, GParamSpec *pspec, gpointer data)
{
    (void)pspec;
    Browser *b = data;
    WebKitDownload *download = WEBKIT_DOWNLOAD(object);
    const char *path = g_object_get_data(object, "lightview-download-path");
    if (!path) return;
    g_autofree char *name = g_path_get_basename(path);
    guint percent = (guint)(webkit_download_get_estimated_progress(download) * 100.0);
    g_free(b->download_message);
    b->download_message = g_strdup_printf("Downloading %s (%u%%)…", name, percent);
    update_ui(b);
}

static void download_failed(WebKitDownload *download, GError *error, gpointer data)
{
    Browser *b = data;
    g_object_set_data(G_OBJECT(download), "lightview-download-failed", GINT_TO_POINTER(1));
    g_free(b->download_message);
    b->download_message = g_strdup_printf("Download failed: %s", error->message);
    update_ui(b);
}

static void download_finished(WebKitDownload *download, gpointer data)
{
    Browser *b = data;
    if (b->downloads_active) b->downloads_active--;
    if (!g_object_get_data(G_OBJECT(download), "lightview-download-failed")) {
        const char *path = g_object_get_data(G_OBJECT(download), "lightview-download-path");
        g_free(b->download_message);
        b->download_message = g_strdup_printf("Downloaded: %s", path ? path : b->download_dir);
        update_ui(b);
    }
}

static void download_started(WebKitWebContext *context, WebKitDownload *download, gpointer data)
{
    (void)context;
    Browser *b = data;
    b->downloads_active++;
    g_free(b->download_message);
    b->download_message = g_strdup("Preparing download…");
    update_ui(b);
    g_signal_connect(download, "decide-destination",
        G_CALLBACK(decide_download_destination), b);
    g_signal_connect(download, "notify::estimated-progress",
        G_CALLBACK(download_progress), b);
    g_signal_connect(download, "failed", G_CALLBACK(download_failed), b);
    g_signal_connect(download, "finished", G_CALLBACK(download_finished), b);
}

static void cancel_page_requests(Browser *b)
{
    GList *requests = g_list_copy(b->page_requests);
    for (GList *item = requests; item; item = item->next) {
        Request *request = item->data;
        request->reset_cancelled = TRUE;
        g_cancellable_cancel(request->operation_cancel);
    }
    g_list_free(requests);
}

static void set_recovery_failure(Browser *b, const char *message)
{
    if (b->recovery_timeout) {
        g_source_remove(b->recovery_timeout);
        b->recovery_timeout = 0;
    }
    b->engine_state = ENGINE_FAILED;
    g_free(b->load_error);
    b->load_error = g_strdup(message);
    g_warning("WebKit recovery failed: %s", message);
    update_ui(b);
}

static gboolean recovery_timeout(gpointer data)
{
    Browser *b = data;
    b->recovery_timeout = 0;
    if (b->engine_state == ENGINE_READY || b->engine_state == ENGINE_FAILED)
        return G_SOURCE_REMOVE;
    if (!b->hard_reset_used) {
        g_warning("WebKit soft reset timed out; escalating to engine reset");
        hard_engine_reset(b);
    } else set_recovery_failure(b, "WebKit engine did not become ready after reset.");
    return G_SOURCE_REMOVE;
}

static void arm_recovery_timeout(Browser *b)
{
    if (b->recovery_timeout) g_source_remove(b->recovery_timeout);
    b->recovery_timeout = g_timeout_add_seconds(RECOVERY_TIMEOUT_SECONDS,
        recovery_timeout, b);
}

static void record_reset(Browser *b, const char *reason)
{
    flash_reset_warning(b);
    b->reset_count++;
    g_free(b->last_reset_at);
    b->last_reset_at = timestamp_now();
    g_free(b->last_termination_reason);
    b->last_termination_reason = g_strdup(reason);
    g_message("WebKit recovery started: reason=%s reset_count=%" G_GUINT64_FORMAT,
        reason, b->reset_count);
}

static gboolean start_reset(Browser *b, const char *source)
{
    if (b->engine_state == ENGINE_RESETTING || b->engine_state == ENGINE_RECOVERING)
        return FALSE;
    cancel_page_requests(b);
    b->recovery_burst_count = 0;
    b->last_recovery_us = 0;
    record_reset(b, source ? source : "requested");
    b->hard_reset_used = FALSE;
    if (b->engine_state == ENGINE_FAILED || !b->view) {
        b->engine_state = ENGINE_RECOVERING;
        hard_engine_reset(b);
    } else {
        b->engine_state = ENGINE_RESETTING;
        arm_recovery_timeout(b);
        update_ui(b);
        webkit_web_view_terminate_web_process(b->view);
    }
    return TRUE;
}

static void process_terminated(WebKitWebView *view, WebKitWebProcessTerminationReason reason,
                               gpointer data)
{
    Browser *b = data;
    if (view != b->view) return;
    const char *reason_name = termination_reason_name(reason);
    g_free(b->last_termination_reason);
    b->last_termination_reason = g_strdup(reason_name);
    g_warning("WebKit web process terminated: reason=%s state=%s", reason_name,
        engine_state_name(b->engine_state));

    if (b->engine_state == ENGINE_RESETTING) {
        b->engine_state = ENGINE_RECOVERING;
        g_clear_pointer(&b->load_error, g_free);
        webkit_web_view_load_uri(b->view, "about:blank");
        update_ui(b);
        return;
    }

    if (b->engine_state == ENGINE_RECOVERING) {
        if (!b->hard_reset_used) hard_engine_reset(b);
        else set_recovery_failure(b, "WebKit terminated again during engine recovery.");
        return;
    }
    if (b->engine_state == ENGINE_FAILED) return;

    gint64 now = g_get_monotonic_time();
    if (b->last_recovery_us &&
        now - b->last_recovery_us <= RECOVERY_BURST_SECONDS * G_USEC_PER_SEC)
        b->recovery_burst_count++;
    else b->recovery_burst_count = 1;
    b->last_recovery_us = now;
    cancel_page_requests(b);
    record_reset(b, reason_name);
    if (b->recovery_burst_count > RECOVERY_BURST_LIMIT) {
        set_recovery_failure(b, "WebKit stopped repeatedly; automatic recovery was bounded.");
        return;
    }
    b->engine_state = ENGINE_RECOVERING;
    b->hard_reset_used = FALSE;
    arm_recovery_timeout(b);
    g_clear_pointer(&b->load_error, g_free);
    webkit_web_view_load_uri(b->view, "about:blank");
    update_ui(b);
}

static char *normalize_uri(const char *input)
{
    if (!input || !*input) return g_strdup("about:blank");
    if (g_path_is_absolute(input)) return g_filename_to_uri(input, NULL, NULL);
    if (g_str_has_prefix(input, "http://") || g_str_has_prefix(input, "https://") ||
        g_str_has_prefix(input, "file://") || g_str_equal(input, "about:blank"))
        return g_strdup(input);
    /* Do not navigate javascript: or unhandled external protocols. */
    if (strstr(input, "://") || g_str_has_prefix(input, "javascript:") ||
        g_str_has_prefix(input, "data:") || g_str_has_prefix(input, "about:"))
        return NULL;
    return g_strconcat("https://", input, NULL);
}

static gboolean navigate(Browser *b, const char *input)
{
    if (b->engine_state != ENGINE_READY || !b->view) return FALSE;
    g_autofree char *uri = normalize_uri(input);
    if (!uri) return FALSE;
    g_clear_pointer(&b->load_error, g_free);
    webkit_web_view_load_uri(b->view, uri);
    return TRUE;
}

typedef struct {
    Browser *browser;
    char *uri;
    guint64 generation;
} PendingNavigation;

static gboolean youtube_uri(const char *uri)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GUri) parsed = g_uri_parse(uri, G_URI_FLAGS_NONE, &error);
    if (!parsed) return FALSE;
    const char *scheme = g_uri_get_scheme(parsed);
    const char *host = g_uri_get_host(parsed);
    return scheme && host &&
        (g_ascii_strcasecmp(scheme, "http") == 0 || g_ascii_strcasecmp(scheme, "https") == 0) &&
        (g_ascii_strcasecmp(host, "youtube.com") == 0 || g_str_has_suffix(host, ".youtube.com"));
}

static gboolean navigate_pending(gpointer data)
{
    PendingNavigation *pending = data;
    if (pending->browser->web_process_generation == pending->generation)
        navigate(pending->browser, pending->uri);
    return G_SOURCE_REMOVE;
}

static void pending_navigation_free(gpointer data)
{
    PendingNavigation *pending = data;
    g_free(pending->uri);
    g_free(pending);
}

static void youtube_navigation_message(WebKitUserContentManager *manager,
                                       WebKitJavascriptResult *result, gpointer data)
{
    (void)manager;
    Browser *b = data;
    JSCValue *value = webkit_javascript_result_get_js_value(result);
    if (!jsc_value_is_string(value)) return;
    g_autofree char *uri = jsc_value_to_string(value);
    if (!youtube_uri(webkit_web_view_get_uri(b->view)) || !youtube_uri(uri)) return;
    PendingNavigation *pending = g_new0(PendingNavigation, 1);
    pending->browser = b;
    pending->uri = g_steal_pointer(&uri);
    pending->generation = b->web_process_generation;
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, navigate_pending, pending,
        pending_navigation_free);
}

static void javascript_done(GObject *object, GAsyncResult *result, gpointer data)
{
    Request *r = data;
    if (r->page_bound) {
        r->browser->page_requests = g_list_remove(r->browser->page_requests, r);
        r->page_bound = FALSE;
    }
    g_autoptr(GError) error = NULL;
    g_autoptr(JSCValue) value = webkit_web_view_call_async_javascript_function_finish(
        WEBKIT_WEB_VIEW(object), result, &error);
    if (r->reset_cancelled) {
        reply_full(r, NULL, "WebKit was reset while the page operation was running",
            "webkit_reset", TRUE);
        return;
    }
    if (!value) {
        reply(r, NULL, error ? error->message : "JavaScript failed");
        return;
    }
    if (jsc_value_is_undefined(value)) {
        reply(r, NULL, NULL);
        return;
    }
    g_autofree char *json = jsc_value_to_json(value, 0);
    if (!json) {
        reply(r, NULL, "JavaScript result is not JSON serializable");
        return;
    }
    g_autoptr(JsonNode) node = json_from_string(json, &error);
    reply(r, node, error ? error->message : NULL);
}

static void dispatch(Request *r)
{
    Browser *b = r->browser;
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(GError) error = NULL;
    if (!json_parser_load_from_data(parser, r->input->str, r->input->len, &error)) {
        reply(r, NULL, "Invalid JSON request");
        return;
    }
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        reply(r, NULL, "Request must be a JSON object");
        return;
    }
    JsonObject *o = json_node_get_object(root);
    const char *cmd = string_member(o, "command");
    if (!cmd) { reply(r, NULL, "command must be a string"); return; }
    if (g_str_equal(cmd, "status")) {
        g_autoptr(JsonObject) state = json_object_new();
        const char *uri = b->view ? webkit_web_view_get_uri(b->view) : NULL;
        const char *title = b->view ? webkit_web_view_get_title(b->view) : NULL;
        json_object_set_string_member(state, "uri", uri ? uri : "");
        json_object_set_string_member(state, "title", title ? title : "");
        json_object_set_boolean_member(state, "loading",
            b->view && webkit_web_view_is_loading(b->view));
        json_object_set_boolean_member(state, "can_go_back",
            b->view && webkit_web_view_can_go_back(b->view));
        json_object_set_boolean_member(state, "can_go_forward",
            b->view && webkit_web_view_can_go_forward(b->view));
        json_object_set_boolean_member(state, "private", b->private_mode);
        json_object_set_boolean_member(state, "low_memory", b->low_memory);
        json_object_set_int_member(state, "memory_limit_mib", b->memory_limit);
        json_object_set_int_member(state, "memory_kill_threshold_mib",
            b->memory_kill_threshold);
        json_object_set_string_member(state, "download_dir", b->download_dir);
        json_object_set_int_member(state, "downloads_active", b->downloads_active);
        json_object_set_int_member(state, "pid", getpid());
        json_object_set_string_member(state, "version", LIGHTVIEW_VERSION);
        json_object_set_string_member(state, "running_mode", running_mode_name(b));
        json_object_set_string_member(state, "running_mode_short", running_mode_short_name(b));
        json_object_set_double_member(state, "ram_pss_mib",
            (double)b->memory_pss_kib / 1024.0);
        json_object_set_double_member(state, "cpu_percent", b->cpu_percent);
        json_object_set_int_member(state, "resource_processes", b->resource_processes);
        json_object_set_string_member(state, "window_title",
            gtk_window_get_title(GTK_WINDOW(b->window)));
        json_object_set_string_member(state, "toolbar_telemetry",
            gtk_label_get_text(GTK_LABEL(b->telemetry)));
        json_object_set_boolean_member(state, "version_dialog_visible",
            b->version_dialog && gtk_widget_get_visible(b->version_dialog));
        json_object_set_boolean_member(state, "mode_toggle_visible",
            b->mode_toggle && gtk_widget_get_visible(b->mode_toggle));
        json_object_set_boolean_member(state, "mode_toggle_active",
            b->mode_toggle && gtk_toggle_button_get_active(
                GTK_TOGGLE_BUTTON(b->mode_toggle)));
        json_object_set_boolean_member(state, "reset_flash_active", b->flash_timeout != 0);
        json_object_set_string_member(state, "engine_state", engine_state_name(b->engine_state));
        json_object_set_int_member(state, "web_process_generation", b->web_process_generation);
        json_object_set_int_member(state, "reset_count", b->reset_count);
        json_object_set_string_member(state, "last_committed_uri",
            b->last_committed_uri ? b->last_committed_uri : "");
        if (b->last_termination_reason)
            json_object_set_string_member(state, "last_termination_reason", b->last_termination_reason);
        else json_object_set_null_member(state, "last_termination_reason");
        if (b->last_reset_at) json_object_set_string_member(state, "last_reset_at", b->last_reset_at);
        else json_object_set_null_member(state, "last_reset_at");
        if (b->load_error) json_object_set_string_member(state, "load_error", b->load_error);
        else json_object_set_null_member(state, "load_error");
        g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
        json_node_set_object(node, state);
        reply(r, node, NULL);
        return;
    }
    if (g_str_equal(cmd, "version")) {
        JsonNode *show_node = json_object_get_member(o, "show");
        if (show_node && (!JSON_NODE_HOLDS_VALUE(show_node) ||
            json_node_get_value_type(show_node) != G_TYPE_BOOLEAN)) {
            reply(r, NULL, "show must be a boolean");
            return;
        }
        if (show_node && json_node_get_boolean(show_node))
            gtk_button_clicked(GTK_BUTTON(b->version_button));
        g_autoptr(JsonObject) details = json_object_new();
        json_object_set_string_member(details, "lightview", LIGHTVIEW_VERSION);
        g_autofree char *webkit = g_strdup_printf("%u.%u.%u", webkit_get_major_version(),
            webkit_get_minor_version(), webkit_get_micro_version());
        json_object_set_string_member(details, "webkitgtk", webkit);
        g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
        json_node_set_object(node, details);
        reply(r, node, NULL);
        return;
    }
    if (g_str_equal(cmd, "quit")) {
        r->quit = TRUE;
        reply(r, NULL, NULL);
        return;
    }
    if (g_str_equal(cmd, "reset")) {
        JsonNode *hard_node = json_object_get_member(o, "hard");
        if (hard_node && (!JSON_NODE_HOLDS_VALUE(hard_node) ||
            json_node_get_value_type(hard_node) != G_TYPE_BOOLEAN)) {
            reply(r, NULL, "hard must be a boolean");
            return;
        }
        gboolean hard = hard_node && json_node_get_boolean(hard_node);
        if (b->engine_state == ENGINE_RESETTING || b->engine_state == ENGINE_RECOVERING) {
            reply_full(r, NULL, "WebKit recovery is already in progress",
                "webkit_recovering", TRUE);
            return;
        }
        if (hard) {
            cancel_page_requests(b);
            b->recovery_burst_count = 0;
            b->last_recovery_us = 0;
            record_reset(b, "automation-hard-reset");
            b->engine_state = ENGINE_RECOVERING;
            hard_engine_reset(b);
        } else if (!start_reset(b, "automation-request")) {
            reply_full(r, NULL, "WebKit recovery is already in progress",
                "webkit_recovering", TRUE);
            return;
        }
        g_autoptr(JsonObject) operation = json_object_new();
        json_object_set_int_member(operation, "operation_id", b->reset_count);
        json_object_set_int_member(operation, "target_generation", b->web_process_generation + 1);
        g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
        json_node_set_object(node, operation);
        reply(r, node, NULL);
        return;
    }
    if (g_str_equal(cmd, "mode")) {
        JsonNode *mode_node = json_object_get_member(o, "low_memory");
        if (!mode_node || !JSON_NODE_HOLDS_VALUE(mode_node) ||
            json_node_get_value_type(mode_node) != G_TYPE_BOOLEAN) {
            reply(r, NULL, "low_memory must be a boolean");
            return;
        }
        if (b->engine_state == ENGINE_RESETTING || b->engine_state == ENGINE_RECOVERING) {
            reply_full(r, NULL, "WebKit recovery is already in progress",
                "webkit_recovering", TRUE);
            return;
        }
        gboolean enabled = json_node_get_boolean(mode_node);
        guint64 previous_generation = b->web_process_generation;
        if (b->mode_toggle)
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b->mode_toggle), enabled);
        else switch_low_memory_mode(b, enabled);
        g_autoptr(JsonObject) operation = json_object_new();
        json_object_set_string_member(operation, "running_mode", running_mode_name(b));
        json_object_set_boolean_member(operation, "changed",
            b->web_process_generation == previous_generation && b->engine_state != ENGINE_READY);
        json_object_set_int_member(operation, "operation_id", b->reset_count);
        json_object_set_int_member(operation, "target_generation",
            b->engine_state == ENGINE_READY ? b->web_process_generation : previous_generation + 1);
        g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
        json_node_set_object(node, operation);
        reply(r, node, NULL);
        return;
    }
    if (b->engine_state != ENGINE_READY || !b->view) {
        reply_engine_unavailable(r);
        return;
    }
    if (g_str_equal(cmd, "eval")) {
        const char *script = string_member(o, "script");
        if (!script) { reply(r, NULL, "script must be a JavaScript expression string"); return; }
        g_autofree char *body = g_strdup_printf("return await (\n%s\n);", script);
        r->page_bound = TRUE;
        b->page_requests = g_list_prepend(b->page_requests, r);
        webkit_web_view_call_async_javascript_function(b->view, body, -1, NULL,
            NULL, NULL, r->operation_cancel, javascript_done, r);
        return;
    }
    if (g_str_equal(cmd, "open")) {
        const char *uri = string_member(o, "uri");
        if (!uri || !*uri || !navigate(b, uri)) {
            reply(r, NULL, "uri must be an HTTP(S) URL, file URL, absolute path, or about:blank");
            return;
        }
    } else if (g_str_equal(cmd, "back")) webkit_web_view_go_back(b->view);
    else if (g_str_equal(cmd, "forward")) webkit_web_view_go_forward(b->view);
    else if (g_str_equal(cmd, "reload")) webkit_web_view_reload(b->view);
    else if (g_str_equal(cmd, "stop")) webkit_web_view_stop_loading(b->view);
    else { reply(r, NULL, "Unknown command"); return; }
    reply(r, NULL, NULL);
}

static void read_request(GObject *object, GAsyncResult *result, gpointer data)
{
    Request *r = data;
    g_autoptr(GBytes) bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(object), result, NULL);
    if (!bytes || g_bytes_get_size(bytes) == 0) { request_free(r); return; }
    gsize size;
    const char *chunk = g_bytes_get_data(bytes, &size);
    const char *newline = memchr(chunk, '\n', size);
    gsize used = newline ? (gsize)(newline - chunk) : size;
    if (r->input->len + used > MAX_REQUEST) {
        reply(r, NULL, "Request exceeds 1 MiB");
        return;
    }
    g_string_append_len(r->input, chunk, used);
    if (newline) dispatch(r);
    else g_input_stream_read_bytes_async(G_INPUT_STREAM(object), 4096,
        G_PRIORITY_DEFAULT, r->cancel, read_request, r);
}

static gboolean incoming(GSocketService *service, GSocketConnection *connection,
                          GObject *source, gpointer data)
{
    (void)service; (void)source;
    Browser *b = data;
    g_autoptr(GCredentials) credentials = g_socket_get_credentials(
        g_socket_connection_get_socket(connection), NULL);
    if (!credentials || g_credentials_get_unix_user(credentials, NULL) != getuid() ||
        b->clients >= MAX_CLIENTS) {
        g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
        return TRUE;
    }
    Request *r = g_new0(Request, 1);
    r->browser = b;
    r->connection = g_object_ref(connection);
    r->cancel = g_cancellable_new();
    r->operation_cancel = g_cancellable_new();
    r->input = g_string_new(NULL);
    r->timeout = g_timeout_add_seconds(30, request_timeout, r);
    b->clients++;
    g_input_stream_read_bytes_async(g_io_stream_get_input_stream(G_IO_STREAM(connection)),
        4096, G_PRIORITY_DEFAULT, r->cancel, read_request, r);
    return TRUE;
}

static gboolean private_directory(const char *path)
{
    struct stat st;
    if (g_mkdir_with_parents(path, 0700) != 0 || g_lstat(path, &st) != 0 ||
        !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 0077)) {
        g_printerr("Directory must be owned by you with mode 0700: %s\n", path);
        return FALSE;
    }
    return TRUE;
}

static void set_application_icon(void)
{
    gtk_window_set_default_icon_name("lightview");
    g_autoptr(GError) error = NULL;
    g_autoptr(GdkPixbuf) icon = gdk_pixbuf_new_from_resource(
        "/io/github/darkoffices/lightview/lightview.png", &error);
    if (icon) gtk_window_set_default_icon(icon);
    else g_warning("Unable to load embedded application icon: %s", error->message);
}

static gboolean start_control(Browser *b)
{
    if (strlen(b->socket_path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        g_printerr("Control socket path is too long; choose a shorter --socket path.\n");
        return FALSE;
    }
    g_autofree char *directory = g_path_get_dirname(b->socket_path);
    if (!private_directory(directory)) return FALSE;
    struct stat socket_stat;
    if (g_lstat(b->socket_path, &socket_stat) == 0) {
        if (!S_ISSOCK(socket_stat.st_mode) || socket_stat.st_uid != getuid()) {
            g_printerr("Refusing to replace unexpected control path: %s\n", b->socket_path);
            return FALSE;
        }
        int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        struct sockaddr_un endpoint = { .sun_family = AF_UNIX };
        g_strlcpy(endpoint.sun_path, b->socket_path, sizeof(endpoint.sun_path));
        int connected = probe < 0 ? -1 : connect(probe,
            (struct sockaddr *)&endpoint, sizeof(endpoint));
        int connect_error = errno;
        if (probe >= 0) close(probe);
        if (connected == 0 || (connect_error != ECONNREFUSED && connect_error != ENOENT)) {
            g_printerr("Control socket is already in use: %s\n", b->socket_path);
            return FALSE;
        }
        if (g_unlink(b->socket_path) != 0 && errno != ENOENT) {
            g_printerr("Cannot remove stale control socket %s: %s\n",
                b->socket_path, g_strerror(errno));
            return FALSE;
        }
    } else if (errno != ENOENT) {
        g_printerr("Cannot inspect control socket %s: %s\n",
            b->socket_path, g_strerror(errno));
        return FALSE;
    }
    g_autoptr(GSocketAddress) address = g_unix_socket_address_new(b->socket_path);
    g_autoptr(GError) error = NULL;
    b->service = g_socket_service_new();
    mode_t old_mask = umask(0077);
    gboolean success = g_socket_listener_add_address(G_SOCKET_LISTENER(b->service),
        address, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, &error);
    umask(old_mask);
    if (!success) {
        g_printerr("Cannot create control socket %s: %s\n", b->socket_path, error->message);
        return FALSE;
    }
    g_signal_connect(b->service, "incoming", G_CALLBACK(incoming), b);
    g_socket_service_start(b->service);
    return TRUE;
}

static void address_activate(GtkEntry *entry, gpointer data)
{
    Browser *b = data;
    if (b->engine_state == ENGINE_READY && b->view &&
        navigate(b, gtk_entry_get_text(entry))) gtk_widget_grab_focus(GTK_WIDGET(b->view));
}

static void go_back(GtkButton *button, gpointer data)
{
    (void)button; Browser *b = data;
    if (b->engine_state == ENGINE_READY && b->view) webkit_web_view_go_back(b->view);
}
static void go_forward(GtkButton *button, gpointer data)
{
    (void)button; Browser *b = data;
    if (b->engine_state == ENGINE_READY && b->view) webkit_web_view_go_forward(b->view);
}
static void reload(GtkButton *button, gpointer data)
{
    (void)button; Browser *b = data;
    if (b->engine_state == ENGINE_READY && b->view) webkit_web_view_reload(b->view);
}

static gboolean key_pressed(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
    (void)widget;
    Browser *b = data;
    guint key = gdk_keyval_to_lower(event->keyval);
    if ((event->state & GDK_CONTROL_MASK) && key == GDK_KEY_l) {
        gtk_widget_grab_focus(b->entry);
        gtk_editable_select_region(GTK_EDITABLE(b->entry), 0, -1);
    } else if (b->engine_state == ENGINE_READY && b->view &&
        (key == GDK_KEY_F5 || ((event->state & GDK_CONTROL_MASK) && key == GDK_KEY_r)))
        webkit_web_view_reload(b->view);
    else if (b->engine_state == ENGINE_READY && b->view &&
        (event->state & GDK_MOD1_MASK) && key == GDK_KEY_Left)
        webkit_web_view_go_back(b->view);
    else if (b->engine_state == ENGINE_READY && b->view &&
        (event->state & GDK_MOD1_MASK) && key == GDK_KEY_Right)
        webkit_web_view_go_forward(b->view);
    else if (b->view && key == GDK_KEY_Escape) webkit_web_view_stop_loading(b->view);
    else if ((event->state & GDK_CONTROL_MASK) && key == GDK_KEY_q) gtk_main_quit();
    else return FALSE;
    return TRUE;
}

static gboolean decide_policy(WebKitWebView *view, WebKitPolicyDecision *decision,
                               WebKitPolicyDecisionType type, gpointer data)
{
    (void)data;
    if (type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) return FALSE;
    WebKitNavigationAction *action = webkit_navigation_policy_decision_get_navigation_action(
        WEBKIT_NAVIGATION_POLICY_DECISION(decision));
    if (webkit_navigation_action_is_user_gesture(action))
        webkit_web_view_load_request(view, webkit_navigation_action_get_request(action));
    webkit_policy_decision_ignore(decision);
    return TRUE;
}

static WebKitWebContext *create_web_context(Browser *b)
{
    WebKitWebContext *context = g_object_new(WEBKIT_TYPE_WEB_CONTEXT,
        "website-data-manager", b->manager,
        "memory-pressure-settings", b->pressure,
        NULL);
    webkit_web_context_set_sandbox_enabled(context, b->sandbox_enabled);
    webkit_web_context_set_cache_model(context, b->browser_cache ?
        WEBKIT_CACHE_MODEL_WEB_BROWSER : WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER);
    webkit_web_context_set_spell_checking_enabled(context, FALSE);
    g_signal_connect(context, "download-started", G_CALLBACK(download_started), b);
    return context;
}

static WebKitWebView *create_web_view(Browser *b)
{
    g_autoptr(WebKitUserContentManager) content = webkit_user_content_manager_new();
    g_signal_connect(content, "script-message-received::navigation",
        G_CALLBACK(youtube_navigation_message), b);
    webkit_user_content_manager_register_script_message_handler(content, "navigation");
    const char *youtube_navigation_workaround =
        "document.addEventListener('click',event=>{"
        "if(!event.isTrusted||event.defaultPrevented||event.button!==0||event.ctrlKey||"
        "event.metaKey||event.shiftKey||event.altKey||!/(^|\\.)youtube\\.com$/.test(location.hostname))return;"
        "const anchor=event.target.closest?.('a[href]');"
        "if(!anchor||anchor.hasAttribute('download')||(anchor.target&&anchor.target!=='_self'))return;"
        "const url=new URL(anchor.href,location.href);"
        "if(url.protocol!=='http:'&&url.protocol!=='https:')return;"
        "event.preventDefault();event.stopImmediatePropagation();"
        "window.webkit.messageHandlers.navigation.postMessage(url.href);"
        "},true);";
    WebKitUserScript *navigation_script = webkit_user_script_new(youtube_navigation_workaround,
        WEBKIT_USER_CONTENT_INJECT_TOP_FRAME, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
        NULL, NULL);
    webkit_user_content_manager_add_script(content, navigation_script);
    webkit_user_script_unref(navigation_script);
    WebKitWebView *view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
        "web-context", b->context, "settings", b->settings,
        "user-content-manager", content, NULL));
    g_signal_connect(view, "notify::title", G_CALLBACK(notify_view), b);
    g_signal_connect(view, "notify::uri", G_CALLBACK(notify_view), b);
    g_signal_connect(view, "load-changed", G_CALLBACK(load_changed), b);
    g_signal_connect(view, "load-failed", G_CALLBACK(load_failed), b);
    g_signal_connect(view, "web-process-terminated", G_CALLBACK(process_terminated), b);
    g_signal_connect(view, "decide-policy", G_CALLBACK(decide_policy), b);
    gtk_box_pack_start(GTK_BOX(b->view_box), GTK_WIDGET(view), TRUE, TRUE, 0);
    gtk_widget_show(GTK_WIDGET(view));
    return view;
}

static void configure_memory_policy(Browser *b)
{
    b->memory_kill_threshold = MAX((guint)MINIMUM_KILL_THRESHOLD_MIB,
        b->memory_limit * 4);
    webkit_memory_pressure_settings_set_memory_limit(b->pressure, b->memory_limit);
    webkit_memory_pressure_settings_set_conservative_threshold(b->pressure, 0.25);
    webkit_memory_pressure_settings_set_strict_threshold(b->pressure, 0.50);
    webkit_memory_pressure_settings_set_kill_threshold(b->pressure,
        (double)b->memory_kill_threshold / b->memory_limit);
    webkit_memory_pressure_settings_set_poll_interval(b->pressure, 15.0);
    webkit_website_data_manager_set_memory_pressure_settings(b->pressure);
}

static void hard_engine_reset(Browser *b)
{
    b->engine_state = ENGINE_RECOVERING;
    b->hard_reset_used = TRUE;
    cancel_page_requests(b);
    if (b->view) {
        WebKitWebView *old_view = b->view;
        b->view = NULL;
        g_signal_handlers_disconnect_by_data(old_view, b);
        gtk_widget_destroy(GTK_WIDGET(old_view));
    }
    g_clear_object(&b->context);
    b->context = create_web_context(b);
    b->view = create_web_view(b);
    if (!b->context || !b->view) {
        set_recovery_failure(b, "Unable to recreate the WebKit engine.");
        return;
    }
    arm_recovery_timeout(b);
    webkit_web_view_load_uri(b->view, "about:blank");
    update_ui(b);
}

static gboolean switch_low_memory_mode(Browser *b, gboolean enabled)
{
    if (b->low_memory == enabled) return FALSE;
    if (b->engine_state == ENGINE_RESETTING || b->engine_state == ENGINE_RECOVERING)
        return FALSE;
    b->low_memory = enabled;
    if (!b->memory_limit_explicit) b->memory_limit = enabled ? 384 : 768;
    configure_memory_policy(b);
    g_object_set(b->settings,
        "enable-webrtc", !enabled,
        "enable-webgl", !enabled,
        "enable-accelerated-2d-canvas", !enabled,
        NULL);
    b->recovery_burst_count = 0;
    b->last_recovery_us = 0;
    record_reset(b, enabled ? "mode-change-low-memory" : "mode-change-normal");
    b->engine_state = ENGINE_RECOVERING;
    hard_engine_reset(b);
    return TRUE;
}

static void low_memory_toggled(GtkToggleButton *toggle, gpointer data)
{
    Browser *b = data;
    if (b->mode_toggle_syncing) return;
    gboolean enabled = gtk_toggle_button_get_active(toggle);
    if (b->low_memory == enabled) return;
    if (!switch_low_memory_mode(b, enabled)) {
        b->mode_toggle_syncing = TRUE;
        gtk_toggle_button_set_active(toggle, b->low_memory);
        b->mode_toggle_syncing = FALSE;
    }
}

static void reset_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    Browser *b = data;
    if (!start_reset(b, "toolbar-request"))
        gtk_window_present(GTK_WINDOW(b->window));
}

static void close_dialog(GtkDialog *dialog, gint response, gpointer data)
{
    (void)response; (void)data;
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void version_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    Browser *b = data;
    if (b->version_dialog) {
        gtk_window_present(GTK_WINDOW(b->version_dialog));
        return;
    }
    g_autofree char *details = version_text();
    b->version_dialog = gtk_message_dialog_new(GTK_WINDOW(b->window),
        GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE,
        "Lightview version information");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(b->version_dialog),
        "%s", details);
    gtk_window_set_title(GTK_WINDOW(b->version_dialog), "About Lightview");
    b->mode_toggle = gtk_check_button_new_with_label("Low memory mode (LM)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b->mode_toggle), b->low_memory);
    gtk_widget_set_tooltip_text(b->mode_toggle,
        "Switching mode resets WebKit to about:blank while keeping the window and control socket");
    gtk_box_pack_start(GTK_BOX(gtk_message_dialog_get_message_area(
        GTK_MESSAGE_DIALOG(b->version_dialog))), b->mode_toggle, FALSE, FALSE, 6);
    g_signal_connect(b->mode_toggle, "toggled", G_CALLBACK(low_memory_toggled), b);
    g_signal_connect(b->mode_toggle, "destroy", G_CALLBACK(gtk_widget_destroyed),
        &b->mode_toggle);
    g_signal_connect(b->version_dialog, "response", G_CALLBACK(close_dialog), NULL);
    g_signal_connect(b->version_dialog, "destroy", G_CALLBACK(gtk_widget_destroyed),
        &b->version_dialog);
    gtk_widget_show_all(b->version_dialog);
}

int main(int argc, char **argv)
{
    /* WebKitGTK 2.52 leaves its accelerated backing store without a transport
     * when this legacy switch is used, then dereferences it when media or other
     * content first requests compositing. Preserve the intended software path
     * by selecting the shared-memory renderer explicitly. */
    if (g_getenv("WEBKIT_DISABLE_DMABUF_RENDERER"))
        g_unsetenv("WEBKIT_DISABLE_DMABUF_RENDERER");
    if (!g_getenv("WEBKIT_DMABUF_RENDERER_FORCE_SHM"))
        g_setenv("WEBKIT_DMABUF_RENDERER_FORCE_SHM", "1", FALSE);
    Browser b = {0};
    b.engine_state = ENGINE_READY;
    b.web_process_generation = 1;
    gboolean no_control = FALSE, version = FALSE;
    gint memory_limit = 0;
    g_autofree char *profile = NULL;
    g_auto(GStrv) urls = NULL;
    GOptionEntry options[] = {
        {"private", 0, 0, G_OPTION_ARG_NONE, &b.private_mode, "Keep website data only for this session", NULL},
        {"profile", 0, 0, G_OPTION_ARG_FILENAME, &profile, "Persistent profile directory", "DIR"},
        {"socket", 0, 0, G_OPTION_ARG_FILENAME, &b.socket_path, "Control socket in a private directory", "PATH"},
        {"no-control", 0, 0, G_OPTION_ARG_NONE, &no_control, "Disable the automation socket", NULL},
        {"no-images", 0, 0, G_OPTION_ARG_NONE, &b.no_images, "Skip automatic image loading", NULL},
        {"memory-limit", 0, 0, G_OPTION_ARG_INT, &memory_limit, "WebKit per-process memory-pressure limit in MiB", "MIB"},
        {"low-memory", 0, 0, G_OPTION_ARG_NONE, &b.low_memory, "Reduce optional web features and use a 384 MiB limit", NULL},
        {"browser-cache", 0, 0, G_OPTION_ARG_NONE, &b.browser_cache, "Favor repeat-load speed over memory savings", NULL},
        {"version", 0, 0, G_OPTION_ARG_NONE, &version, "Show version", NULL},
        {G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_STRING_ARRAY, &urls, NULL, "[URL]"},
        {NULL, 0, 0, 0, NULL, NULL, NULL}
    };
    g_autoptr(GOptionContext) options_context = g_option_context_new("— lightweight WebKit browser");
    g_option_context_add_main_entries(options_context, options, NULL);
    g_autoptr(GError) error = NULL;
    if (!g_option_context_parse(options_context, &argc, &argv, &error)) {
        g_printerr("%s\n", error->message); return 1;
    }
    if (version) {
        g_autofree char *details = version_text();
        g_print("%s\n", details);
        return 0;
    }
    if ((urls && g_strv_length(urls) > 1) || (profile && b.private_mode)) {
        g_printerr("Use one URL; --profile and --private are mutually exclusive.\n"); return 1;
    }
    b.memory_limit_explicit = memory_limit != 0;
    if (memory_limit == 0) memory_limit = b.low_memory ? 384 : 768;
    if (memory_limit < 128 || memory_limit > 65536) {
        g_printerr("--memory-limit must be between 128 and 65536 MiB.\n"); return 1;
    }
    b.memory_limit = (guint)memory_limit;
    const char *downloads = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    b.download_dir = downloads ? g_strdup(downloads) :
        g_build_filename(g_get_home_dir(), "Downloads", NULL);
    if (g_mkdir_with_parents(b.download_dir, 0755) != 0 ||
        !g_file_test(b.download_dir, G_FILE_TEST_IS_DIR) ||
        g_access(b.download_dir, W_OK) != 0) {
        g_printerr("Download directory is not writable: %s\n", b.download_dir);
        g_free(b.download_dir);
        return 1;
    }
    if (!gtk_init_check(&argc, &argv)) {
        g_printerr("A graphical display is required (XFCE/X11 or Wayland).\n"); return 1;
    }
    set_application_icon();
    b.pressure = webkit_memory_pressure_settings_new();
    configure_memory_policy(&b);
    int profile_fd = -1;
    if (b.private_mode) b.manager = webkit_website_data_manager_new_ephemeral();
    else {
        if (!profile) profile = g_build_filename(g_get_user_data_dir(), "lightview", NULL);
        g_autofree char *absolute = g_canonicalize_filename(profile, NULL);
        if (!private_directory(absolute)) return 1;
        g_autofree char *lock = g_build_filename(absolute, ".lock", NULL);
        profile_fd = open(lock, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (profile_fd < 0 || flock(profile_fd, LOCK_EX | LOCK_NB) != 0) {
            g_printerr("Cannot lock profile (another browser may be using it): %s\n", absolute);
            if (profile_fd >= 0) close(profile_fd);
            return 1;
        }
        g_autofree char *cache = g_build_filename(absolute, "cache", NULL);
        b.manager = webkit_website_data_manager_new("base-data-directory", absolute,
            "base-cache-directory", cache, NULL);
        g_autofree char *cookies = g_build_filename(absolute, "cookies.sqlite", NULL);
        webkit_cookie_manager_set_persistent_storage(
            webkit_website_data_manager_get_cookie_manager(b.manager), cookies,
            WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
    }
    const char *disable_sandbox = g_getenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS");
    b.sandbox_enabled = !(disable_sandbox && g_str_equal(disable_sandbox, "1"));
    b.context = create_web_context(&b);
    b.settings = webkit_settings_new_with_settings(
        "enable-javascript", TRUE,
        "enable-page-cache", b.browser_cache,
        "auto-load-images", !b.no_images,
        "media-playback-requires-user-gesture", TRUE,
        "enable-webrtc", !b.low_memory,
        "enable-webgl", !b.low_memory,
        "enable-accelerated-2d-canvas", !b.low_memory,
        NULL);
    b.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(b.window), 1100, 760);
    GtkWidget *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(toolbar), 4);
    b.back = gtk_button_new_from_icon_name("go-previous-symbolic", GTK_ICON_SIZE_BUTTON);
    b.forward = gtk_button_new_from_icon_name("go-next-symbolic", GTK_ICON_SIZE_BUTTON);
    GtkWidget *refresh = gtk_button_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
    b.reset_button = gtk_button_new_from_icon_name("edit-clear-all-symbolic", GTK_ICON_SIZE_BUTTON);
    b.version_button = gtk_button_new_from_icon_name("dialog-information-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(b.back, "Back (Alt+Left)");
    gtk_widget_set_tooltip_text(b.forward, "Forward (Alt+Right)");
    gtk_widget_set_tooltip_text(refresh, "Reload (Ctrl+R)");
    gtk_widget_set_tooltip_text(b.reset_button, "Reset WebKit");
    gtk_widget_set_tooltip_text(b.version_button, "Version information");
    atk_object_set_name(gtk_widget_get_accessible(b.reset_button), "Reset WebKit");
    atk_object_set_name(gtk_widget_get_accessible(b.version_button), "Version information");
    b.entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(b.entry), "Address (Ctrl+L)");
    g_autoptr(GtkCssProvider) reset_css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(reset_css,
        ".webkit-reset-flash { background-image: none; background-color: #c62828; "
        "color: white; border-color: #8e0000; }", -1, NULL);
    gtk_style_context_add_provider(gtk_widget_get_style_context(b.entry),
        GTK_STYLE_PROVIDER(reset_css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    b.telemetry = gtk_label_new(NULL);
    gtk_label_set_ellipsize(GTK_LABEL(b.telemetry), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(b.telemetry), 30);
    gtk_widget_set_tooltip_text(b.telemetry,
        "Running mode and Lightview/WebKit process-tree resource usage (PSS)");
    atk_object_set_name(gtk_widget_get_accessible(b.telemetry),
        "Running mode and resource usage");
    gtk_box_pack_start(GTK_BOX(toolbar), b.back, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), b.forward, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), refresh, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), b.entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), b.telemetry, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(toolbar), b.reset_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), b.version_button, FALSE, FALSE, 0);
    b.view_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    b.message = gtk_label_new(NULL);
    gtk_label_set_ellipsize(GTK_LABEL(b.message), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(b.message), 0);
    gtk_box_pack_start(GTK_BOX(column), toolbar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(column), b.view_box, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(column), b.message, FALSE, FALSE, 2);
    gtk_container_add(GTK_CONTAINER(b.window), column);
    b.view = create_web_view(&b);
    g_signal_connect(b.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(b.window, "key-press-event", G_CALLBACK(key_pressed), &b);
    g_signal_connect(b.entry, "activate", G_CALLBACK(address_activate), &b);
    g_signal_connect(b.back, "clicked", G_CALLBACK(go_back), &b);
    g_signal_connect(b.forward, "clicked", G_CALLBACK(go_forward), &b);
    g_signal_connect(refresh, "clicked", G_CALLBACK(reload), &b);
    g_signal_connect(b.reset_button, "clicked", G_CALLBACK(reset_clicked), &b);
    g_signal_connect(b.version_button, "clicked", G_CALLBACK(version_clicked), &b);
    if (!no_control) {
        if (!b.socket_path) b.socket_path = g_build_filename(g_get_user_runtime_dir(),
            "lightview", "control.sock", NULL);
        if (!start_control(&b)) return 1;
        g_print("Control socket: %s\n", b.socket_path);
    }
    g_unix_signal_add(SIGINT, quit_browser, NULL);
    g_unix_signal_add(SIGTERM, quit_browser, NULL);
    gtk_widget_show_all(b.window);
    if (!navigate(&b, urls ? urls[0] : "about:blank")) {
        g_printerr("Unsupported URL\n");
        if (!no_control) g_unlink(b.socket_path);
        return 1;
    }
    sample_resource_usage(&b);
    b.resource_timer = g_timeout_add_seconds(2, sample_resource_usage, &b);
    gtk_widget_grab_focus(urls ? GTK_WIDGET(b.view) : b.entry);
    gtk_main();
    if (b.resource_timer) g_source_remove(b.resource_timer);
    if (b.flash_timeout) g_source_remove(b.flash_timeout);
    if (b.recovery_timeout) g_source_remove(b.recovery_timeout);
    if (b.service) {
        g_socket_service_stop(b.service);
        g_socket_listener_close(G_SOCKET_LISTENER(b.service));
        g_unlink(b.socket_path);
        g_object_unref(b.service);
    }
    g_clear_object(&b.context);
    g_clear_object(&b.settings);
    g_clear_object(&b.manager);
    webkit_memory_pressure_settings_free(b.pressure);
    if (profile_fd >= 0) close(profile_fd);
    g_free(b.socket_path);
    g_free(b.load_error);
    g_free(b.download_dir);
    g_free(b.download_message);
    g_free(b.last_committed_uri);
    g_free(b.last_termination_reason);
    g_free(b.last_reset_at);
    return 0;
}
