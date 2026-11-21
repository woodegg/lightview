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
#ifdef __GLIBC__
#include <malloc.h>
#endif

#define MAX_REQUEST (1024 * 1024)
#define MAX_RESPONSE (4 * 1024 * 1024)
#define MAX_CLIENTS 16
#include "lightview-version.h"
#define RECOVERY_TIMEOUT_SECONDS 10
#define RECOVERY_BURST_SECONDS 300
#define RECOVERY_BURST_LIMIT 2
#define RESET_FLASH_SECONDS 5
#define DEFAULT_KILL_THRESHOLD_MIB 3072
#define DEFAULT_NETWORK_RECYCLE_THRESHOLD_MIB 0
#define DEFAULT_NETWORK_THREAD_RECYCLE_THRESHOLD 512
#define DEFERRED_NAVIGATION_RETRY_MILLISECONDS 50
#define DEFERRED_NAVIGATION_SETTLE_MILLISECONDS 2000
#define DEFERRED_NAVIGATION_QUIET_MILLISECONDS 250
#define NAVIGATION_IN_PLACE_RETRY_LIMIT 2
#define AUTOMATION_CANCEL_GRACE_MILLISECONDS 1000
#define AUTOMATION_RELOAD_GRACE_MILLISECONDS 3000
#define DEFAULT_LEASE_SECONDS 120
#define DEFAULT_CONTROL_TIMEOUT_SECONDS 60
#define DEFAULT_IDLE_HIBERNATE_SECONDS 3600
#define MAX_IDLE_HIBERNATE_SECONDS 86400

typedef enum {
    ENGINE_READY,
    ENGINE_SUSPENDED,
    ENGINE_RESETTING,
    ENGINE_RECOVERING,
    ENGINE_FAILED
} EngineState;

typedef struct {
    GtkWidget *window, *entry, *back, *forward, *message, *view_box, *telemetry;
    GtkWidget *reset_button, *version_button, *version_dialog, *mode_toggle;
    GtkWidget *memory_protection_toggle, *memory_threshold_spin, *memory_apply_button;
    GtkWidget *idle_hibernate_spin, *idle_hibernate_apply_button;
    GtkWidget *profile_label, *profile_button;
    WebKitWebView *view;
    WebKitWebContext *context;
    WebKitWebsiteDataManager *manager;
    WebKitSettings *settings;
    WebKitMemoryPressureSettings *pressure;
    GSocketService *service;
    GList *page_requests;
    char *socket_path, *profile_path;
    char *load_error, *download_dir, *download_message;
    char *lease_token;
    char *committed_uri, *last_committed_uri, *last_termination_reason, *last_reset_at;
    char *pending_navigation_uri, *navigation_watchdog_uri;
    char *deferred_navigation_uri;
    guint clients, control_timeout_seconds;
    guint memory_limit, memory_kill_threshold, configured_kill_threshold;
    guint memory_recycle_threshold, network_recycle_threshold;
    guint network_thread_recycle_threshold, network_threads;
    guint idle_hibernate_seconds, lease_seconds, hibernate_timer;
    guint downloads_active;
    guint recovery_timeout, flash_timeout, navigation_watchdog, load_watchdog;
    guint load_probe_timeout;
    GCancellable *load_probe_cancel;
    guint deferred_navigation;
    guint navigation_retry_attempt, navigation_view_rebuild_attempt;
    guint resource_timer, resource_processes;
    guint64 web_process_generation, navigation_sequence, reset_count, proactive_recycle_count;
    guint64 automation_timeout_count, navigation_retry_count;
    guint64 navigation_view_rebuild_count;
    guint64 deferred_navigation_generation;
    guint64 network_process_recycle_count;
    guint64 load_stop_count, resource_trace_sequence;
    guint64 load_watchdog_epoch;
    guint64 memory_pss_kib, network_pss_kib, previous_cpu_ticks;
    guint recovery_burst_count;
    gint64 last_recovery_us, previous_cpu_sample_us, last_activity_us, lease_expires_us;
    gint64 deferred_navigation_deadline_us;
    double cpu_percent;
    int profile_fd;
    EngineState engine_state;
    gboolean private_mode, low_memory, memory_limit_explicit, memory_recycle_explicit;
    gboolean browser_cache, no_images, software_rendering, media_gl_sink_disabled;
    gboolean memory_protection_enabled, sandbox_enabled, hard_reset_used;
    gboolean mode_toggle_syncing, memory_controls_syncing;
    gboolean current_load_committed, trace_navigation, trace_resources;
} Browser;

typedef struct {
    guint64 id, navigation_sequence;
    gint64 started_us;
    gboolean main_resource, failed;
} ResourceTrace;

typedef struct {
    Browser *browser;
    GSocketConnection *connection;
    GCancellable *cancel;
    GCancellable *operation_cancel;
    GString *input;
    char *output, *page_error;
    guint timeout, page_error_grace;
    guint64 page_generation, page_navigation_sequence;
    gboolean quit, page_bound, reset_cancelled, operation_timed_out, client_counted;
    gboolean page_navigation_started;
} Request;

static void process_terminated(WebKitWebView *view,
    WebKitWebProcessTerminationReason reason, gpointer data);
static gboolean decide_policy(WebKitWebView *view, WebKitPolicyDecision *decision,
    WebKitPolicyDecisionType type, gpointer data);
static void youtube_navigation_message(WebKitUserContentManager *manager,
    WebKitJavascriptResult *result, gpointer data);
static void download_started(WebKitWebContext *context, WebKitDownload *download,
    gpointer data);
static gboolean rebuild_web_view_for_navigation(Browser *b, const char *uri);
static void hard_engine_reset(Browser *b);
static gboolean start_reset(Browser *b, const char *source);
static gboolean switch_low_memory_mode(Browser *b, gboolean enabled);
static gboolean switch_memory_policy(Browser *b, gboolean enabled, guint threshold);
static gboolean wake_engine(Browser *b, const char *uri);
static gboolean navigate(Browser *b, const char *input);
static void cancel_deferred_navigation(Browser *b);
static gboolean deferred_navigation_load(gpointer data);
static void set_idle_hibernate(Browser *b, guint seconds);
static gboolean switch_profile(Browser *b, const char *path, GError **error);
static void update_ui(Browser *b);

static char *trace_origin(const char *uri)
{
    if (!uri || !*uri) return g_strdup("none");
    if (g_str_equal(uri, "about:blank")) return g_strdup(uri);
    g_autoptr(GUri) parsed = g_uri_parse(uri, G_URI_FLAGS_PARSE_RELAXED, NULL);
    if (!parsed) return g_strdup("invalid");
    const char *scheme = g_uri_get_scheme(parsed);
    const char *host = g_uri_get_host(parsed);
    /* Opt-in traces must not expose URL paths, credentials, queries, or
     * fragments. Origins are enough to diagnose cross-site navigation. */
    return host ? g_strdup_printf("%s://%s", scheme ? scheme : "unknown", host) :
        g_strdup_printf("%s:", scheme ? scheme : "unknown");
}

static void trace_navigation(Browser *b, const char *phase, const char *uri)
{
    if (!b->trace_navigation) return;
    g_autofree char *origin = trace_origin(uri);
    g_autofree char *view_origin = trace_origin(
        b->view ? webkit_web_view_get_uri(b->view) : NULL);
    g_autofree char *document_origin = trace_origin(b->committed_uri);
    g_message("Navigation trace: phase=%s origin=%s view=%s document=%s "
        "sequence=%" G_GUINT64_FORMAT " generation=%" G_GUINT64_FORMAT
        " loading=%d responsive=%d page=%" G_GUINT64_FORMAT,
        phase, origin, view_origin, document_origin, b->navigation_sequence,
        b->web_process_generation,
        b->view && webkit_web_view_is_loading(b->view),
        b->view && webkit_web_view_get_is_web_process_responsive(b->view),
        b->view ? webkit_web_view_get_page_id(b->view) : 0);
}

static gboolean traced_main_resource(WebKitWebResource *resource)
{
    ResourceTrace *trace = g_object_get_data(G_OBJECT(resource), "lightview-resource-trace");
    return !trace || trace->main_resource;
}

static void trace_resource_event(Browser *b, WebKitWebResource *resource,
    const char *phase, GError *error)
{
    if (!b->trace_resources) return;
    ResourceTrace *trace = g_object_get_data(G_OBJECT(resource), "lightview-resource-trace");
    if (!trace) return;
    if (error) trace->failed = TRUE;
    g_autofree char *origin = trace_origin(webkit_web_resource_get_uri(resource));
    WebKitURIResponse *response = webkit_web_resource_get_response(resource);
    g_message("Resource trace: id=%" G_GUINT64_FORMAT " sequence=%" G_GUINT64_FORMAT
        " phase=%s main=%d origin=%s elapsed-ms=%" G_GINT64_FORMAT
        " status=%u failed=%d error-domain=%s error-code=%d",
        trace->id, trace->navigation_sequence, phase, trace->main_resource, origin,
        (g_get_monotonic_time() - trace->started_us) / 1000,
        response ? webkit_uri_response_get_status_code(response) : 0, trace->failed,
        error ? g_quark_to_string(error->domain) : "none", error ? error->code : 0);
}

static void trace_resource_finished(WebKitWebResource *resource, gpointer data)
{
    if (traced_main_resource(resource))
        trace_navigation(data, "main-resource-finished", webkit_web_resource_get_uri(resource));
    trace_resource_event(data, resource, "finished", NULL);
}

static void trace_resource_failed(WebKitWebResource *resource, GError *error, gpointer data)
{
    Browser *b = data;
    if (b->trace_navigation && traced_main_resource(resource)) {
        trace_navigation(b, "main-resource-failed", webkit_web_resource_get_uri(resource));
        g_message("Navigation trace: resource=%p error-domain=%s error-code=%d",
            (gpointer)resource, g_quark_to_string(error->domain), error->code);
    }
    trace_resource_event(b, resource, "failed", error);
}

static void trace_resource_response(WebKitWebResource *resource, GParamSpec *spec,
                                    gpointer data)
{
    (void)spec;
    Browser *b = data;
    if (b->trace_navigation && traced_main_resource(resource)) {
        trace_navigation(b, "main-resource-response", webkit_web_resource_get_uri(resource));
        WebKitURIResponse *response = webkit_web_resource_get_response(resource);
        if (response) g_message("Navigation trace: resource=%p status=%u",
            (gpointer)resource, webkit_uri_response_get_status_code(response));
    }
    trace_resource_event(b, resource, "response", NULL);
}

static void trace_resource_started(WebKitWebView *view, WebKitWebResource *resource,
                                   WebKitURIRequest *request, gpointer data)
{
    Browser *b = data;
    gboolean main_resource = resource == webkit_web_view_get_main_resource(view);
    if (!main_resource && !b->trace_resources) return;
    if (main_resource)
        trace_navigation(b, "main-resource-started", webkit_uri_request_get_uri(request));
    if (b->trace_resources) {
        ResourceTrace *trace = g_new0(ResourceTrace, 1);
        trace->id = ++b->resource_trace_sequence;
        trace->navigation_sequence = b->navigation_sequence;
        trace->started_us = g_get_monotonic_time();
        trace->main_resource = main_resource;
        g_object_set_data_full(G_OBJECT(resource), "lightview-resource-trace", trace, g_free);
        trace_resource_event(b, resource, "started", NULL);
    }
    g_signal_connect(resource, "finished", G_CALLBACK(trace_resource_finished), data);
    g_signal_connect(resource, "failed", G_CALLBACK(trace_resource_failed), data);
    g_signal_connect(resource, "notify::response", G_CALLBACK(trace_resource_response), data);
}

static gboolean trace_script_dialog(WebKitWebView *view, WebKitScriptDialog *dialog,
                                     gpointer data)
{
    trace_navigation(data, "script-dialog", webkit_web_view_get_uri(view));
    g_message("Navigation trace: script-dialog-type=%d", webkit_script_dialog_get_dialog_type(dialog));
    return FALSE;
}

static void trace_process_responsive(WebKitWebView *view, GParamSpec *spec, gpointer data)
{
    (void)spec;
    trace_navigation(data, "process-responsive-changed", webkit_web_view_get_uri(view));
}

static void note_activity(Browser *b)
{
    b->last_activity_us = g_get_monotonic_time();
}

static gboolean lease_active(Browser *b)
{
    if (b->lease_token && g_get_monotonic_time() >= b->lease_expires_us) {
        g_clear_pointer(&b->lease_token, g_free);
        b->lease_expires_us = 0;
    }
    return b->lease_token != NULL;
}

static gboolean quit_browser(gpointer unused)
{
    (void)unused;
    /* A queued control reply can outlive the top-level loop while shutdown is
     * draining WebKit callbacks. Avoid calling gtk_main_quit() a second time
     * after the loop has already returned. */
    if (gtk_main_level() > 0) gtk_main_quit();
    return G_SOURCE_REMOVE;
}

static void release_client_slot(Request *r)
{
    if (!r->client_counted) return;
    if (r->browser->clients > 0) r->browser->clients--;
    else g_warning("Control client accounting underflow prevented");
    r->client_counted = FALSE;
}

static void queue_active_page_for_recovery(Browser *b)
{
    if (b->navigation_watchdog_uri) {
        g_free(b->pending_navigation_uri);
        b->pending_navigation_uri = g_strdup(b->navigation_watchdog_uri);
        return;
    }
    if (!b->last_committed_uri ||
        (!g_str_has_prefix(b->last_committed_uri, "http://") &&
         !g_str_has_prefix(b->last_committed_uri, "https://") &&
         !g_str_has_prefix(b->last_committed_uri, "file://")))
        return;
    g_free(b->pending_navigation_uri);
    b->pending_navigation_uri = g_strdup(b->last_committed_uri);
}

static void request_free(Request *r)
{
    if (r->page_bound) {
        r->browser->page_requests = g_list_remove(r->browser->page_requests, r);
        r->page_bound = FALSE;
    }
    if (r->timeout) g_source_remove(r->timeout);
    if (r->page_error_grace) g_source_remove(r->page_error_grace);
    g_io_stream_close(G_IO_STREAM(r->connection), NULL, NULL);
    g_object_unref(r->connection);
    g_object_unref(r->cancel);
    g_object_unref(r->operation_cancel);
    g_string_free(r->input, TRUE);
    g_free(r->output);
    g_free(r->page_error);
    release_client_slot(r);
    g_free(r);
}

static gboolean request_reload_grace_timeout(gpointer data)
{
    Request *r = data;
    r->timeout = 0;
    if (!r->page_bound) return G_SOURCE_REMOVE;
    r->browser->page_requests = g_list_remove(r->browser->page_requests, r);
    r->page_bound = FALSE;
    r->reset_cancelled = TRUE;
    /* A cooperative WebKit operation completes its callback as cancelled and
     * never reaches this fallback. Escalate only when even cancellation cannot
     * release the page operation, which indicates a genuinely wedged web
     * process rather than a slow promise or transiently busy page. */
    if (r->browser->engine_state == ENGINE_READY) {
        queue_active_page_for_recovery(r->browser);
        const char *reason = r->page_navigation_started ||
            r->browser->navigation_sequence != r->page_navigation_sequence ?
            "automation-navigation-race" : "automation-timeout";
        g_warning("Timed-out page automation did not cancel; resetting WebKit "
            "(generation=%" G_GUINT64_FORMAT "->%" G_GUINT64_FORMAT
            " navigation=%" G_GUINT64_FORMAT "->%" G_GUINT64_FORMAT
            " load_started=%s loading=%s)",
            r->page_generation, r->browser->web_process_generation,
            r->page_navigation_sequence, r->browser->navigation_sequence,
            r->page_navigation_started ? "yes" : "no",
            r->browser->view && webkit_web_view_is_loading(r->browser->view) ? "yes" : "no");
        start_reset(r->browser, reason);
    }
    return G_SOURCE_REMOVE;
}

static gboolean request_cancel_grace_timeout(gpointer data)
{
    Request *r = data;
    r->timeout = 0;
    if (!r->page_bound) return G_SOURCE_REMOVE;
    if (r->browser->engine_state != ENGINE_READY || !r->browser->view) {
        r->reset_cancelled = TRUE;
        return G_SOURCE_REMOVE;
    }
    /* WebKitGTK does not always complete a cancelled async JavaScript call,
     * notably when it is awaiting a promise that never settles. Replace the
     * current document before replacing the whole web process: destroying the
     * JavaScript context releases the callback while retaining the engine,
     * generation, profile, main process, and control socket. */
    const char *current = r->browser->last_committed_uri;
    g_autofree char *uri = g_strdup(current && *current ? current : "about:blank");
    note_activity(r->browser);
    g_clear_pointer(&r->browser->load_error, g_free);
    webkit_web_view_load_uri(r->browser->view, uri);
    r->timeout = g_timeout_add(AUTOMATION_RELOAD_GRACE_MILLISECONDS,
        request_reload_grace_timeout, r);
    g_message("Timed-out page automation did not cancel; reloading the current "
        "document before considering a WebKit reset");
    return G_SOURCE_REMOVE;
}

static gboolean request_timeout(gpointer data)
{
    Request *r = data;
    r->timeout = 0;
    if (!r->page_bound) {
        g_cancellable_cancel(r->cancel);
        return G_SOURCE_REMOVE;
    }
    r->browser->automation_timeout_count++;
    if (r->browser->engine_state != ENGINE_READY) {
        r->reset_cancelled = TRUE;
        g_cancellable_cancel(r->operation_cancel);
        return G_SOURCE_REMOVE;
    }
    r->operation_timed_out = TRUE;
    g_cancellable_cancel(r->operation_cancel);
    r->timeout = g_timeout_add(AUTOMATION_CANCEL_GRACE_MILLISECONDS,
        request_cancel_grace_timeout, r);
    g_message("Page-bound automation timed out; cancellation requested "
        "(generation=%" G_GUINT64_FORMAT " navigation=%" G_GUINT64_FORMAT
        " load_started=%s loading=%s)",
        r->page_generation, r->page_navigation_sequence,
        r->page_navigation_started ? "yes" : "no",
        r->browser->view && webkit_web_view_is_loading(r->browser->view) ? "yes" : "no");
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

static void reply_engine_unavailable(Request *r, const Browser *b)
{
    if (b->engine_state == ENGINE_FAILED) {
        reply_full(r, NULL,
            b->load_error ? b->load_error : "WebKit recovery failed; reset is required",
            "webkit_failed", FALSE);
        return;
    }
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
    case ENGINE_SUSPENDED: return "suspended";
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

static gboolean environment_truthy(const char *value)
{
    return value && *value &&
        !g_str_equal(value, "0") &&
        g_ascii_strcasecmp(value, "false") != 0 &&
        g_ascii_strcasecmp(value, "no") != 0;
}

static gboolean has_accessible_drm_render_node(void)
{
    g_autoptr(GDir) directory = g_dir_open("/dev/dri", 0, NULL);
    if (!directory) return FALSE;

    const char *name;
    while ((name = g_dir_read_name(directory))) {
        if (!g_str_has_prefix(name, "renderD")) continue;
        g_autofree char *path = g_build_filename("/dev/dri", name, NULL);
        int fd = g_open(path, O_RDWR | O_CLOEXEC, 0);
        if (fd < 0) continue;
        close(fd);
        return TRUE;
    }
    return FALSE;
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

static guint process_threads(pid_t pid)
{
    g_autofree char *path = g_strdup_printf("/proc/%ld/status", (long)pid);
    g_autofree char *contents = NULL;
    if (!g_file_get_contents(path, &contents, NULL, NULL)) return 0;
    g_auto(GStrv) lines = g_strsplit(contents, "\n", -1);
    for (guint i = 0; lines[i]; i++)
        if (g_str_has_prefix(lines[i], "Threads:"))
            return (guint)g_ascii_strtoull(lines[i] + 8, NULL, 10);
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

static guint terminate_network_processes(void)
{
    g_autofree char *path = g_strdup_printf("/proc/%ld/task/%ld/children",
        (long)getpid(), (long)getpid());
    g_autofree char *contents = NULL;
    if (!g_file_get_contents(path, &contents, NULL, NULL)) return 0;

    guint terminated = 0;
    g_auto(GStrv) children = g_strsplit_set(contents, " \t\n", -1);
    for (guint i = 0; children[i]; i++) {
        if (!*children[i]) continue;
        gint64 child = g_ascii_strtoll(children[i], NULL, 10);
        if (child <= 0 || child > G_MAXINT) continue;
        g_autofree char *comm_path = g_strdup_printf("/proc/%" G_GINT64_FORMAT "/comm",
            child);
        g_autofree char *comm = NULL;
        if (!g_file_get_contents(comm_path, &comm, NULL, NULL)) continue;
        g_strchomp(comm);
        /* Linux truncates comm to TASK_COMM_LEN, so WebKitNetworkProcess is
         * normally reported as WebKitNetworkPr. Only inspect direct children
         * spawned by this Lightview process before sending the signal. */
        if (!g_str_has_prefix(comm, "WebKitNetwork")) continue;
        if (kill((pid_t)child, SIGKILL) == 0) terminated++;
        else if (errno != ESRCH)
            g_warning("Unable to terminate WebKit network process %" G_GINT64_FORMAT
                ": %s", child, g_strerror(errno));
    }
    return terminated;
}

static guint64 network_process_pss_kib(void)
{
    g_autofree char *path = g_strdup_printf("/proc/%ld/task/%ld/children",
        (long)getpid(), (long)getpid());
    g_autofree char *contents = NULL;
    if (!g_file_get_contents(path, &contents, NULL, NULL)) return 0;

    guint64 pss_kib = 0;
    g_auto(GStrv) children = g_strsplit_set(contents, " \t\n", -1);
    for (guint i = 0; children[i]; i++) {
        if (!*children[i]) continue;
        gint64 child = g_ascii_strtoll(children[i], NULL, 10);
        if (child <= 0 || child > G_MAXINT) continue;
        g_autofree char *comm_path = g_strdup_printf("/proc/%" G_GINT64_FORMAT "/comm",
            child);
        g_autofree char *comm = NULL;
        if (!g_file_get_contents(comm_path, &comm, NULL, NULL)) continue;
        g_strchomp(comm);
        if (g_str_has_prefix(comm, "WebKitNetwork"))
            pss_kib += process_pss_kib((pid_t)child);
    }
    return pss_kib;
}

static guint network_process_threads(void)
{
    g_autofree char *path = g_strdup_printf("/proc/%ld/task/%ld/children",
        (long)getpid(), (long)getpid());
    g_autofree char *contents = NULL;
    if (!g_file_get_contents(path, &contents, NULL, NULL)) return 0;

    guint threads = 0;
    g_auto(GStrv) children = g_strsplit_set(contents, " \t\n", -1);
    for (guint i = 0; children[i]; i++) {
        if (!*children[i]) continue;
        gint64 child = g_ascii_strtoll(children[i], NULL, 10);
        if (child <= 0 || child > G_MAXINT) continue;
        g_autofree char *comm_path = g_strdup_printf("/proc/%" G_GINT64_FORMAT "/comm",
            child);
        g_autofree char *comm = NULL;
        if (!g_file_get_contents(comm_path, &comm, NULL, NULL)) continue;
        g_strchomp(comm);
        if (g_str_has_prefix(comm, "WebKitNetwork"))
            threads += process_threads((pid_t)child);
    }
    return threads;
}

static void drain_main_context_for(guint milliseconds)
{
    gint64 deadline = g_get_monotonic_time() +
        (gint64)milliseconds * G_TIME_SPAN_MILLISECOND;
    while (g_get_monotonic_time() < deadline) {
        while (g_main_context_pending(NULL))
            g_main_context_iteration(NULL, FALSE);
        g_usleep(1000);
    }
}

static guint terminate_network_processes_for_reset(Browser *b)
{
    guint network_processes = terminate_network_processes();
    if (!network_processes) return 0;
    b->network_process_recycle_count++;
    g_message("Terminated %u WebKit network process%s during engine reset",
        network_processes, network_processes == 1 ? "" : "es");
    return network_processes;
}

static void terminate_web_process_for_reset(Browser *b)
{
    if (b->engine_state == ENGINE_RESETTING && b->view)
        webkit_web_view_terminate_web_process(b->view);
}

typedef struct {
    Browser *browser;
    GList *cookies;
} NetworkCookieRestore;

static void network_cookies_restored(GObject *source, GAsyncResult *result,
                                     gpointer data)
{
    NetworkCookieRestore *restore = data;
    g_autoptr(GError) error = NULL;
    webkit_cookie_manager_replace_cookies_finish(
        WEBKIT_COOKIE_MANAGER(source), result, &error);
    if (error)
        g_warning("Unable to restore cookies after WebKit network recycle: %s",
            error->message);
    g_list_free_full(restore->cookies, (GDestroyNotify)soup_cookie_free);
    terminate_web_process_for_reset(restore->browser);
    g_free(restore);
}

static void network_cookies_snapshotted(GObject *source, GAsyncResult *result,
                                        gpointer data)
{
    Browser *b = data;
    g_autoptr(GError) error = NULL;
    GList *cookies = webkit_cookie_manager_get_all_cookies_finish(
        WEBKIT_COOKIE_MANAGER(source), result, &error);
    if (b->engine_state != ENGINE_RESETTING) {
        g_list_free_full(cookies, (GDestroyNotify)soup_cookie_free);
        return;
    }
    if (error) {
        g_warning("Skipping WebKit network recycle because cookies could not be saved: %s",
            error->message);
        terminate_web_process_for_reset(b);
        return;
    }
    if (!terminate_network_processes_for_reset(b)) {
        g_list_free_full(cookies, (GDestroyNotify)soup_cookie_free);
        terminate_web_process_for_reset(b);
        return;
    }
    /* An empty profile legitimately has no cookies. WebKit's replace API
     * requires a non-NULL list, so continue the reset directly instead of
     * waiting forever for a callback that will never be scheduled. */
    if (!cookies) {
        terminate_web_process_for_reset(b);
        return;
    }

    NetworkCookieRestore *restore = g_new0(NetworkCookieRestore, 1);
    restore->browser = b;
    restore->cookies = cookies;
    webkit_cookie_manager_replace_cookies(WEBKIT_COOKIE_MANAGER(source), cookies,
        NULL, network_cookies_restored, restore);
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
    b->network_pss_kib = network_process_pss_kib();
    b->network_threads = network_process_threads();
    b->resource_processes = processes;
    b->previous_cpu_ticks = cpu_ticks;
    b->previous_cpu_sample_us = now;
    update_ui(b);
    return G_SOURCE_CONTINUE;
}

static void refresh_memory_usage(Browser *b)
{
    g_autoptr(GHashTable) seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    guint64 pss_kib = 0, ignored_cpu_ticks = 0;
    guint processes = 0;
    collect_process_tree(getpid(), seen, &pss_kib, &ignored_cpu_ticks, &processes);
    b->memory_pss_kib = pss_kib;
    b->network_pss_kib = network_process_pss_kib();
    b->network_threads = network_process_threads();
    b->resource_processes = processes;
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
    gtk_widget_set_sensitive(b->entry, ready || b->engine_state == ENGINE_SUSPENDED);
    if (b->reset_button) gtk_widget_set_sensitive(b->reset_button,
        b->engine_state != ENGINE_RESETTING && b->engine_state != ENGINE_RECOVERING);
    if (b->mode_toggle) gtk_widget_set_sensitive(b->mode_toggle,
        b->engine_state != ENGINE_RESETTING && b->engine_state != ENGINE_RECOVERING);
    gboolean policy_controls_ready = b->engine_state != ENGINE_RESETTING &&
        b->engine_state != ENGINE_RECOVERING;
    if (b->memory_protection_toggle)
        gtk_widget_set_sensitive(b->memory_protection_toggle, policy_controls_ready);
    if (b->memory_threshold_spin)
        gtk_widget_set_sensitive(b->memory_threshold_spin, policy_controls_ready &&
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(b->memory_protection_toggle)));
    if (b->memory_apply_button)
        gtk_widget_set_sensitive(b->memory_apply_button, policy_controls_ready);
    if (b->profile_button)
        gtk_widget_set_sensitive(b->profile_button, policy_controls_ready &&
            b->downloads_active == 0);
    gtk_widget_set_sensitive(b->back, ready && webkit_web_view_can_go_back(b->view));
    gtk_widget_set_sensitive(b->forward, ready && webkit_web_view_can_go_forward(b->view));
    gboolean loading = b->view && webkit_web_view_is_loading(b->view);
    const char *engine_message = NULL;
    if (b->engine_state == ENGINE_SUSPENDED) engine_message = "WebKit sleeping — open a URL to wake it.";
    else if (b->engine_state == ENGINE_RESETTING) engine_message = "Resetting WebKit…";
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

static gboolean note_view_input(GtkWidget *widget, GdkEvent *event, gpointer data)
{
    (void)widget; (void)event;
    note_activity(data);
    return FALSE;
}

static void cancel_navigation_watchdog(Browser *b)
{
    if (b->navigation_watchdog) {
        g_source_remove(b->navigation_watchdog);
        b->navigation_watchdog = 0;
    }
    g_clear_pointer(&b->navigation_watchdog_uri, g_free);
}

static void cancel_deferred_navigation(Browser *b)
{
    if (b->deferred_navigation) {
        g_source_remove(b->deferred_navigation);
        b->deferred_navigation = 0;
    }
    g_clear_pointer(&b->deferred_navigation_uri, g_free);
    b->deferred_navigation_deadline_us = 0;
}

static void cancel_load_watchdog(Browser *b)
{
    b->load_watchdog_epoch++;
    if (b->load_watchdog) g_source_remove(b->load_watchdog);
    b->load_watchdog = 0;
    if (b->load_probe_timeout) g_source_remove(b->load_probe_timeout);
    b->load_probe_timeout = 0;
    if (b->load_probe_cancel) g_cancellable_cancel(b->load_probe_cancel);
    g_clear_object(&b->load_probe_cancel);
}

static guint load_watchdog_seconds(const Browser *b)
{
    return MAX(5U, b->control_timeout_seconds / 2);
}

static void arm_load_watchdog(Browser *b);

typedef struct {
    Browser *browser;
    guint64 epoch;
} LoadProbe;

static gboolean load_probe_expired(gpointer data)
{
    Browser *b = data;
    b->load_probe_timeout = 0;
    if (b->load_probe_cancel) g_cancellable_cancel(b->load_probe_cancel);
    return G_SOURCE_REMOVE;
}

static void load_probe_done(GObject *object, GAsyncResult *result, gpointer data)
{
    LoadProbe *probe = data;
    Browser *b = probe->browser;
    guint64 epoch = probe->epoch;
    g_free(probe);
    g_autoptr(GError) error = NULL;
    g_autoptr(JSCValue) value = webkit_web_view_evaluate_javascript_finish(
        WEBKIT_WEB_VIEW(object), result, &error);
    if (epoch != b->load_watchdog_epoch || WEBKIT_WEB_VIEW(object) != b->view)
        return;
    if (b->load_probe_timeout) g_source_remove(b->load_probe_timeout);
    b->load_probe_timeout = 0;
    g_clear_object(&b->load_probe_cancel);
    if (b->engine_state != ENGINE_READY || !b->current_load_committed ||
        !webkit_web_view_is_loading(b->view))
        return;

    /* COMMITTED only means response data has started arriving. Cancelling
     * while a stylesheet or deferred script still blocks parsing can leave
     * the document permanently stuck at a head with no body. Stop remaining
     * subresources only after DOMContentLoaded, and keep an inconclusive or
     * cancelled probe bounded without using it to reset the page process. */
    if (!value || !jsc_value_is_boolean(value) || !jsc_value_to_boolean(value)) {
        arm_load_watchdog(b);
        return;
    }
    b->load_stop_count++;
    g_warning("Page load exceeded the %u-second automation budget; stopping remaining "
        "resources after DOMContentLoaded without resetting WebKit", load_watchdog_seconds(b));
    webkit_web_view_stop_loading(b->view);
    note_activity(b);
    update_ui(b);
}

static gboolean load_watchdog_expired(gpointer data)
{
    Browser *b = data;
    b->load_watchdog = 0;
    if (b->engine_state != ENGINE_READY || !b->view ||
        !webkit_web_view_is_loading(b->view))
        return G_SOURCE_REMOVE;

    /* Only stop subresources for a document that has already committed. An
     * uncommitted main-resource request is owned by the navigation watchdog;
     * stopping it here guarantees that it cannot commit and turns a slow
     * navigation into an unnecessary engine reset. */
    if (!b->current_load_committed)
        return G_SOURCE_REMOVE;

    LoadProbe *probe = g_new0(LoadProbe, 1);
    probe->browser = b;
    probe->epoch = b->load_watchdog_epoch;
    b->load_probe_cancel = g_cancellable_new();
    b->load_probe_timeout = g_timeout_add_seconds(5, load_probe_expired, b);
    webkit_web_view_evaluate_javascript(b->view,
        "document.readyState !== 'loading' && !!document.body && "
        "(document.readyState === 'complete' || "
        "performance.getEntriesByType('navigation').some(e => e.domContentLoadedEventEnd > 0))",
        -1, "lightview-load-watchdog", NULL, b->load_probe_cancel, load_probe_done, probe);
    return G_SOURCE_REMOVE;
}

static void arm_load_watchdog(Browser *b)
{
    cancel_load_watchdog(b);
    b->load_watchdog = g_timeout_add_seconds(load_watchdog_seconds(b),
        load_watchdog_expired, b);
}

static gboolean navigation_watchdog_expired(gpointer data)
{
    Browser *b = data;
    trace_navigation(b, "watchdog-expired", b->navigation_watchdog_uri);
    b->navigation_watchdog = 0;
    if (b->engine_state != ENGINE_READY || !b->navigation_watchdog_uri) {
        g_clear_pointer(&b->navigation_watchdog_uri, g_free);
        return G_SOURCE_REMOVE;
    }

    g_autofree char *uri = g_steal_pointer(&b->navigation_watchdog_uri);
    if (b->navigation_retry_attempt < NAVIGATION_IN_PLACE_RETRY_LIMIT) {
        b->navigation_retry_attempt++;
        b->navigation_retry_count++;
        g_warning("Navigation did not commit; parking on a local document and "
            "retrying in the retained WebKit process (attempt %u/%u)",
            b->navigation_retry_attempt, NAVIGATION_IN_PLACE_RETRY_LIMIT);
        cancel_deferred_navigation(b);
        cancel_load_watchdog(b);
        b->deferred_navigation_uri = g_steal_pointer(&uri);
        b->deferred_navigation_generation = b->web_process_generation;
        b->deferred_navigation_deadline_us = g_get_monotonic_time() +
            (gint64)DEFERRED_NAVIGATION_SETTLE_MILLISECONDS * G_TIME_SPAN_MILLISECOND;
        b->deferred_navigation = g_timeout_add(
            DEFERRED_NAVIGATION_RETRY_MILLISECONDS,
            deferred_navigation_load, b);
        /* Reissuing a remote URI immediately after stop_loading() can leave
         * every retry attached to the same permanently provisional load. A
         * deterministic local document supersedes that request without an
         * explicit cancellation race. LOAD_FINISHED then gives WebKit a quiet
         * main-loop turn before deferred_navigation_load() sends the retained
         * destination. This keeps the WebView, control connection, profile,
         * and WebKit process alive while still escaping the wedged load. */
        webkit_web_view_load_html(b->view,
            "<!doctype html><meta charset=\"utf-8\"><title></title>",
            "about:blank");
        note_activity(b);
        update_ui(b);
        return G_SOURCE_REMOVE;
    }

    if (!b->navigation_view_rebuild_attempt) {
        b->navigation_view_rebuild_attempt = 1;
        if (rebuild_web_view_for_navigation(b, uri))
            return G_SOURCE_REMOVE;
        g_warning("Unable to rebuild the WebView for navigation recovery; "
            "escalating to a WebKit reset");
    }

    g_free(b->pending_navigation_uri);
    b->pending_navigation_uri = g_steal_pointer(&uri);
    b->navigation_retry_attempt = 0;
    g_warning("Navigation did not commit after an in-place retry and WebView "
        "rebuild; resetting WebKit and replaying the destination");
    start_reset(b, "navigation-timeout");
    return G_SOURCE_REMOVE;
}

static void arm_navigation_watchdog(Browser *b, const char *uri)
{
    cancel_navigation_watchdog(b);
    b->navigation_watchdog_uri = g_strdup(uri);
    trace_navigation(b, "dispatch", uri);
    /* A slow but healthy remote server must receive the full configured
     * operation budget before recovery begins. Using half of that budget for
     * the first attempt caused occasional 30--60 second major-site commits to
     * be cancelled and misclassified as wedged WebProcesses. A retry gets half
     * a budget; genuinely stuck content still progresses through the bounded
     * retry/rebuild/reset sequence. */
    guint seconds = b->navigation_retry_attempt ?
        MAX(5U, b->control_timeout_seconds / 2) :
        MAX(5U, b->control_timeout_seconds);
    b->navigation_watchdog = g_timeout_add_seconds(seconds,
        navigation_watchdog_expired, b);
}

static void complete_recovery(Browser *b)
{
    if (b->recovery_timeout) {
        g_source_remove(b->recovery_timeout);
        b->recovery_timeout = 0;
    }
    b->engine_state = ENGINE_READY;
    b->hard_reset_used = FALSE;
    /* A replacement engine that reached a finished blank document recovered
     * successfully. Do not treat later, independent page failures as one
     * continuous restart loop. Failures during recovery are bounded in
     * process_terminated() before this point. */
    b->recovery_burst_count = 0;
    b->last_recovery_us = 0;
    b->web_process_generation++;
    note_activity(b);
    g_clear_pointer(&b->load_error, g_free);
    g_message("WebKit recovery ready: generation=%" G_GUINT64_FORMAT
        " reset_count=%" G_GUINT64_FORMAT, b->web_process_generation, b->reset_count);
    update_ui(b);
    if (b->pending_navigation_uri) {
        g_autofree char *uri = g_steal_pointer(&b->pending_navigation_uri);
        note_activity(b);
        g_clear_pointer(&b->load_error, g_free);
        arm_navigation_watchdog(b, uri);
        webkit_web_view_load_uri(b->view, uri);
    }
}

static const char *skip_www(const char *host)
{
    return host && g_ascii_strncasecmp(host, "www.", 4) == 0 ? host + 4 : host;
}

static gboolean navigation_target_matches(const char *expected, const char *actual)
{
    if (!expected || !actual) return FALSE;
    if (g_str_equal(expected, actual)) return TRUE;
    g_autoptr(GError) expected_error = NULL;
    g_autoptr(GError) actual_error = NULL;
    g_autoptr(GUri) expected_uri = g_uri_parse(expected, G_URI_FLAGS_NONE, &expected_error);
    g_autoptr(GUri) actual_uri = g_uri_parse(actual, G_URI_FLAGS_NONE, &actual_error);
    if (!expected_uri || !actual_uri) return FALSE;
    const char *expected_scheme = g_uri_get_scheme(expected_uri);
    const char *actual_scheme = g_uri_get_scheme(actual_uri);
    const char *expected_host = skip_www(g_uri_get_host(expected_uri));
    const char *actual_host = skip_www(g_uri_get_host(actual_uri));
    if (!expected_scheme || !actual_scheme || !expected_host || !actual_host ||
        (g_ascii_strcasecmp(expected_scheme, "http") != 0 &&
         g_ascii_strcasecmp(expected_scheme, "https") != 0) ||
        (g_ascii_strcasecmp(actual_scheme, "http") != 0 &&
         g_ascii_strcasecmp(actual_scheme, "https") != 0))
        return FALSE;
    if (g_ascii_strcasecmp(expected_host, actual_host) == 0) return TRUE;
    g_autofree char *expected_suffix = g_strconcat(".", expected_host, NULL);
    g_autofree char *actual_suffix = g_strconcat(".", actual_host, NULL);
    return g_str_has_suffix(actual_host, expected_suffix) ||
           g_str_has_suffix(expected_host, actual_suffix);
}

static void load_changed(WebKitWebView *view, WebKitLoadEvent event, gpointer data)
{
    Browser *b = data;
    const char *phase = event == WEBKIT_LOAD_STARTED ? "load-started" :
        event == WEBKIT_LOAD_REDIRECTED ? "load-redirected" :
        event == WEBKIT_LOAD_COMMITTED ? "load-committed" : "load-finished";
    trace_navigation(b, phase, webkit_web_view_get_uri(view));
    if (event == WEBKIT_LOAD_STARTED || event == WEBKIT_LOAD_FINISHED) note_activity(b);
    if (event == WEBKIT_LOAD_STARTED) {
        b->current_load_committed = FALSE;
        if (b->engine_state == ENGINE_READY) arm_load_watchdog(b);
        g_clear_pointer(&b->load_error, g_free);
        for (GList *item = b->page_requests; item; item = item->next) {
            Request *request = item->data;
            request->page_navigation_started = TRUE;
        }
    }
    if (event == WEBKIT_LOAD_FINISHED && !webkit_web_view_is_loading(view))
        cancel_load_watchdog(b);
    if (event == WEBKIT_LOAD_COMMITTED) {
        b->current_load_committed = TRUE;
        const char *uri = webkit_web_view_get_uri(view);
        g_free(b->committed_uri);
        b->committed_uri = g_strdup(uri);
        /* A cancelled page can emit a late commit after a newer open request.
         * Do not let that stale cross-site event disarm the watchdog for the
         * destination that automation is actually waiting for. */
        if (b->navigation_watchdog_uri &&
            navigation_target_matches(b->navigation_watchdog_uri, uri)) {
            cancel_navigation_watchdog(b);
            b->navigation_retry_attempt = 0;
            b->navigation_view_rebuild_attempt = 0;
        }
        b->navigation_sequence++;
    }
    if (event == WEBKIT_LOAD_COMMITTED && b->engine_state == ENGINE_READY) {
        const char *uri = webkit_web_view_get_uri(view);
        if (uri && *uri) {
            g_free(b->last_committed_uri);
            b->last_committed_uri = g_strdup(uri);
        }
    }
    if (event == WEBKIT_LOAD_FINISHED && b->engine_state == ENGINE_RECOVERING)
        complete_recovery(b);
    if (event == WEBKIT_LOAD_FINISHED && b->engine_state == ENGINE_READY &&
        b->deferred_navigation_uri) {
        /* Dispatch as soon as the cancelled document is actually idle. This
         * is more reliable than assuming a fixed delay is long enough for a
         * busy page's late cancellation callbacks to drain. */
        if (b->deferred_navigation) g_source_remove(b->deferred_navigation);
        /* LOAD_FINISHED means WebKit is idle, but late cancellation callbacks
         * can still be queued in the same main-loop turn. Give them a short
         * quiet window before dispatching the replacement document. */
        b->deferred_navigation = g_timeout_add(
            DEFERRED_NAVIGATION_QUIET_MILLISECONDS,
            deferred_navigation_load, b);
    }
    update_ui(b);
}

static gboolean load_failed(WebKitWebView *view, WebKitLoadEvent event,
                            const char *uri, GError *error, gpointer data)
{
    Browser *b = data;
    trace_navigation(b, "load-failed", uri);
    if (b->trace_navigation) g_message("Navigation trace: load-event=%d error-domain=%s error-code=%d",
        event, g_quark_to_string(error->domain), error->code);
    /* Standalone media first replaces its preliminary resource load with a
     * media document. Keep supervising that navigation until the media
     * document commits; this handoff is not a page failure. */
    if (g_error_matches(error, WEBKIT_PLUGIN_ERROR, WEBKIT_PLUGIN_ERROR_WILL_HANDLE_LOAD))
        return TRUE;
    /* A cancelled old navigation may report failure after a replacement load
     * has already started. Do not let that stale callback disarm the new
     * load's budget. */
    if (!webkit_web_view_is_loading(view)) cancel_load_watchdog(b);
    if (g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED))
        return TRUE;
    cancel_navigation_watchdog(b);
    b->navigation_retry_attempt = 0;
    b->navigation_view_rebuild_attempt = 0;
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
    note_activity(b);
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
    note_activity(b);
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
    g_clear_pointer(&b->pending_navigation_uri, g_free);
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
    cancel_navigation_watchdog(b);
    cancel_deferred_navigation(b);
    b->navigation_retry_attempt = 0;
    b->navigation_view_rebuild_attempt = 0;
    cancel_load_watchdog(b);
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
    g_clear_pointer(&b->committed_uri, g_free);
    const char *reason_name = termination_reason_name(reason);
    /* Keep the explicit reset source for operator/automation diagnostics. An
     * expected terminate-by-API callback otherwise hides memory-maintenance or
     * toolbar-request behind a generic termination reason. */
    if (b->engine_state != ENGINE_RESETTING) {
        g_free(b->last_termination_reason);
        b->last_termination_reason = g_strdup(reason_name);
    }
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
    /* Unexpected WebKit termination must not silently discard the active
     * page. Recover through a known-good blank document, then replay the last
     * committed top-level URL while retaining the Lightview process/socket. */
    queue_active_page_for_recovery(b);
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

static gboolean deferred_navigation_load(gpointer data)
{
    Browser *b = data;
    b->deferred_navigation = 0;
    if (!b->deferred_navigation_uri || b->engine_state != ENGINE_READY || !b->view ||
        b->web_process_generation != b->deferred_navigation_generation) {
        g_clear_pointer(&b->deferred_navigation_uri, g_free);
        b->deferred_navigation_deadline_us = 0;
        return G_SOURCE_REMOVE;
    }
    if (webkit_web_view_is_loading(b->view) &&
        b->deferred_navigation_deadline_us > g_get_monotonic_time()) {
        b->deferred_navigation = g_timeout_add(
            DEFERRED_NAVIGATION_RETRY_MILLISECONDS,
            deferred_navigation_load, b);
        return G_SOURCE_REMOVE;
    }
    g_autofree char *uri = g_steal_pointer(&b->deferred_navigation_uri);
    b->deferred_navigation_deadline_us = 0;
    note_activity(b);
    g_clear_pointer(&b->load_error, g_free);
    arm_navigation_watchdog(b, uri);
    webkit_web_view_load_uri(b->view, uri);
    return G_SOURCE_REMOVE;
}

static gboolean navigate(Browser *b, const char *input)
{
    if (b->engine_state != ENGINE_READY || !b->view) return FALSE;
    g_autofree char *uri = normalize_uri(input);
    if (!uri) return FALSE;
    cancel_navigation_watchdog(b);
    cancel_deferred_navigation(b);
    /* Recovery attempts belong to one explicit top-level navigation. A fresh
     * command gets its own retry/rebuild budget; a rebuilt view dispatches the
     * retained destination directly and therefore keeps these flags set. */
    b->navigation_retry_attempt = 0;
    b->navigation_view_rebuild_attempt = 0;
    /* A committed page can keep its subresource load alive indefinitely. Its
     * watchdog must not survive an explicit open request: otherwise the old
     * timer can expire while the replacement main resource is provisional
     * and stop the new navigation. WEBKIT_LOAD_STARTED arms a fresh budget
     * for the replacement. */
    cancel_load_watchdog(b);
    /* The UI sampler runs every two seconds. Heavy pages can grow by hundreds
     * of MiB between its last tick and the next automation navigation, so a
     * cached value can miss the maintenance threshold or retain a pre-reset
     * high-water mark. Take a current process-tree sample at the decision
     * point; this keeps proactive recycle ahead of WebKit's last-resort kill. */
    if (b->memory_recycle_threshold || b->network_recycle_threshold ||
        b->network_thread_recycle_threshold)
        refresh_memory_usage(b);
    gboolean network_memory_limit_reached = b->network_recycle_threshold &&
        b->network_pss_kib >= (guint64)b->network_recycle_threshold * 1024;
    gboolean network_thread_limit_reached = b->network_thread_recycle_threshold &&
        b->network_threads >= b->network_thread_recycle_threshold;
    if ((network_memory_limit_reached || network_thread_limit_reached) &&
        !b->downloads_active && !b->page_requests &&
        !g_str_equal(uri, "about:blank")) {
        guint recycled = terminate_network_processes();
        if (recycled) {
            b->network_process_recycle_count++;
            g_message("Proactive WebKit network-only recycle at %.2f MiB PSS and "
                "%u threads (limits: %u MiB, %u threads); preserving page "
                "process and generation",
                (double)b->network_pss_kib / 1024.0,
                b->network_threads, b->network_recycle_threshold,
                b->network_thread_recycle_threshold);
            /* Let WebKit observe the helper's exit before asking the retained
             * WebProcess to start a new load. Issuing both in the same main
             * loop dispatch can race the network crash notification and make
             * the navigation fail with an internal network error. */
            drain_main_context_for(250);
            b->network_pss_kib = network_process_pss_kib();
            b->network_threads = network_process_threads();
        }
    }
    if (b->memory_recycle_threshold &&
        b->memory_pss_kib >= (guint64)b->memory_recycle_threshold * 1024 &&
        !b->downloads_active && !b->page_requests &&
        !g_str_equal(uri, "about:blank")) {
        g_free(b->pending_navigation_uri);
        b->pending_navigation_uri = g_steal_pointer(&uri);
        b->recovery_burst_count = 0;
        b->last_recovery_us = 0;
        b->proactive_recycle_count++;
        record_reset(b, "memory-maintenance");
        g_message("Proactive WebKit recycle at %.2f MiB PSS (threshold %u MiB)",
            (double)b->memory_pss_kib / 1024.0, b->memory_recycle_threshold);
        b->engine_state = ENGINE_RESETTING;
        arm_recovery_timeout(b);
        update_ui(b);
        /* Snapshot cookies before the network process is terminated, then
         * restore them through the retained data manager before replacing the
         * page process. This preserves both persistent and private sessions. */
        webkit_cookie_manager_get_all_cookies(
            webkit_website_data_manager_get_cookie_manager(b->manager),
            NULL, network_cookies_snapshotted, b);
        return TRUE;
    }
    note_activity(b);
    g_clear_pointer(&b->load_error, g_free);
    if (webkit_web_view_is_loading(b->view)) {
        /* A newly created WebView begins an implicit blank-document load. The
         * control socket can become reachable before that first document has
         * committed. Issuing an external URI during that small window can race
         * WebProcess initialization and leave the first request provisional
         * forever. Retain only the newest requested destination and dispatch
         * it when the blank document finishes, with a short timer fallback in
         * case that bootstrap document never emits LOAD_FINISHED. */
        if (b->navigation_sequence == 0 && !b->last_committed_uri) {
            b->deferred_navigation_uri = g_steal_pointer(&uri);
            b->deferred_navigation_generation = b->web_process_generation;
            b->deferred_navigation_deadline_us = g_get_monotonic_time() +
                (gint64)DEFERRED_NAVIGATION_SETTLE_MILLISECONDS *
                    G_TIME_SPAN_MILLISECOND;
            b->deferred_navigation = g_timeout_add(
                DEFERRED_NAVIGATION_RETRY_MILLISECONDS,
                deferred_navigation_load, b);
            return TRUE;
        }
    }
    /* Let WebKit replace the previous navigation itself, including a committed
     * document that still has active subresources. An explicit stop followed
     * by a separate remote load can leave WebKit's per-page network state
     * permanently provisional even after the cancellation callbacks drain.
     * The previous load's watchdog has already been cancelled above. */
    arm_navigation_watchdog(b, uri);
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

static gboolean javascript_error_after_grace(gpointer data)
{
    Request *r = data;
    r->page_error_grace = 0;
    if (r->page_bound) {
        r->browser->page_requests = g_list_remove(r->browser->page_requests, r);
        r->page_bound = FALSE;
    }
    if (r->reset_cancelled || r->browser->engine_state != ENGINE_READY ||
        r->browser->web_process_generation != r->page_generation) {
        reply_full(r, NULL, "WebKit was reset while the page operation was running",
            "webkit_reset", TRUE);
    } else if (r->operation_timed_out) {
        reply_full(r, NULL, "JavaScript operation timed out and was cancelled",
            "operation_timeout", TRUE);
    } else {
        reply(r, NULL, r->page_error ? r->page_error : "JavaScript failed");
    }
    return G_SOURCE_REMOVE;
}

static void javascript_done(GObject *object, GAsyncResult *result, gpointer data)
{
    Request *r = data;
    note_activity(r->browser);
    g_autoptr(GError) error = NULL;
    g_autoptr(JSCValue) value = webkit_web_view_call_async_javascript_function_finish(
        WEBKIT_WEB_VIEW(object), result, &error);
    /* WebKit can complete an in-flight JS call with INVALID_RESULT just before
     * emitting web-process-terminated. Keep the request page-bound for one
     * short main-loop grace period so the termination handler can mark it as
     * reset-cancelled. Without this ordering guard, automation observes a
     * non-retryable "Unsupported result type" instead of webkit_reset. */
    if (!value && !r->reset_cancelled && error &&
        g_error_matches(error, WEBKIT_JAVASCRIPT_ERROR,
                        WEBKIT_JAVASCRIPT_ERROR_INVALID_RESULT)) {
        g_free(r->page_error);
        r->page_error = g_strdup(error->message);
        r->page_error_grace = g_timeout_add(100, javascript_error_after_grace, r);
        return;
    }
    if (r->page_bound) {
        r->browser->page_requests = g_list_remove(r->browser->page_requests, r);
        r->page_bound = FALSE;
    }
    if (r->reset_cancelled) {
        reply_full(r, NULL, "WebKit was reset while the page operation was running",
            "webkit_reset", TRUE);
        return;
    }
    if (r->operation_timed_out) {
        reply_full(r, NULL, "JavaScript operation timed out and was cancelled",
            "operation_timeout", TRUE);
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
        gboolean leased = lease_active(b);
        g_autoptr(JsonObject) state = json_object_new();
        const char *uri = b->view ? webkit_web_view_get_uri(b->view) : NULL;
        const char *title = b->view ? webkit_web_view_get_title(b->view) : NULL;
        json_object_set_string_member(state, "uri", uri ? uri : "");
        json_object_set_string_member(state, "title", title ? title : "");
        json_object_set_boolean_member(state, "loading",
            b->view && webkit_web_view_is_loading(b->view));
        json_object_set_boolean_member(state, "web_process_responsive",
            b->view && webkit_web_view_get_is_web_process_responsive(b->view));
        json_object_set_boolean_member(state, "can_go_back",
            b->view && webkit_web_view_can_go_back(b->view));
        json_object_set_boolean_member(state, "can_go_forward",
            b->view && webkit_web_view_can_go_forward(b->view));
        json_object_set_boolean_member(state, "private", b->private_mode);
        if (b->profile_path) json_object_set_string_member(state, "profile_path", b->profile_path);
        else json_object_set_null_member(state, "profile_path");
        json_object_set_boolean_member(state, "low_memory", b->low_memory);
        json_object_set_int_member(state, "memory_limit_mib", b->memory_limit);
        json_object_set_int_member(state, "memory_kill_threshold_mib",
            b->memory_kill_threshold);
        json_object_set_boolean_member(state, "memory_protection_enabled",
            b->memory_protection_enabled);
        json_object_set_int_member(state, "configured_memory_kill_threshold_mib",
            b->configured_kill_threshold);
        json_object_set_int_member(state, "memory_recycle_threshold_mib",
            b->memory_recycle_threshold);
        json_object_set_int_member(state, "network_recycle_threshold_mib",
            b->network_recycle_threshold);
        json_object_set_int_member(state, "network_thread_recycle_threshold",
            b->network_thread_recycle_threshold);
        json_object_set_double_member(state, "network_pss_mib",
            (double)b->network_pss_kib / 1024.0);
        json_object_set_int_member(state, "network_threads", b->network_threads);
        json_object_set_int_member(state, "proactive_recycle_count",
            b->proactive_recycle_count);
        json_object_set_int_member(state, "network_process_recycle_count",
            b->network_process_recycle_count);
        json_object_set_string_member(state, "download_dir", b->download_dir);
        json_object_set_int_member(state, "downloads_active", b->downloads_active);
        json_object_set_int_member(state, "control_clients", b->clients);
        json_object_set_int_member(state, "page_requests_active",
            g_list_length(b->page_requests));
        json_object_set_int_member(state, "automation_timeout_count",
            b->automation_timeout_count);
        json_object_set_int_member(state, "navigation_retry_count",
            b->navigation_retry_count);
        json_object_set_int_member(state, "navigation_view_rebuild_count",
            b->navigation_view_rebuild_count);
        json_object_set_int_member(state, "control_timeout_seconds",
            b->control_timeout_seconds);
        json_object_set_int_member(state, "pid", getpid());
        json_object_set_string_member(state, "version", LIGHTVIEW_VERSION);
        json_object_set_string_member(state, "running_mode", running_mode_name(b));
        json_object_set_string_member(state, "running_mode_short", running_mode_short_name(b));
        json_object_set_boolean_member(state, "software_rendering", b->software_rendering);
        json_object_set_boolean_member(state, "media_gl_sink_disabled",
            b->media_gl_sink_disabled);
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
        json_object_set_boolean_member(state, "memory_protection_toggle_visible",
            b->memory_protection_toggle && gtk_widget_get_visible(
                b->memory_protection_toggle));
        json_object_set_boolean_member(state, "memory_protection_toggle_active",
            b->memory_protection_toggle && gtk_toggle_button_get_active(
                GTK_TOGGLE_BUTTON(b->memory_protection_toggle)));
        json_object_set_int_member(state, "memory_threshold_control_mib",
            b->memory_threshold_spin ? gtk_spin_button_get_value_as_int(
                GTK_SPIN_BUTTON(b->memory_threshold_spin)) : 0);
        json_object_set_boolean_member(state, "idle_hibernate_control_visible",
            b->idle_hibernate_spin && gtk_widget_get_visible(b->idle_hibernate_spin));
        json_object_set_int_member(state, "idle_hibernate_control_seconds",
            b->idle_hibernate_spin ? gtk_spin_button_get_value_as_int(
                GTK_SPIN_BUTTON(b->idle_hibernate_spin)) : 0);
        json_object_set_boolean_member(state, "profile_button_visible",
            b->profile_button && gtk_widget_get_visible(b->profile_button));
        json_object_set_string_member(state, "profile_control_path",
            b->profile_label ? gtk_label_get_text(GTK_LABEL(b->profile_label)) : "");
        json_object_set_boolean_member(state, "reset_flash_active", b->flash_timeout != 0);
        json_object_set_string_member(state, "engine_state", engine_state_name(b->engine_state));
        json_object_set_int_member(state, "idle_hibernate_seconds", b->idle_hibernate_seconds);
        json_object_set_boolean_member(state, "lease_active", leased);
        json_object_set_int_member(state, "lease_expires_in_seconds", leased ?
            (b->lease_expires_us - g_get_monotonic_time() + G_USEC_PER_SEC - 1) /
                G_USEC_PER_SEC : 0);
        json_object_set_int_member(state, "web_process_generation", b->web_process_generation);
        json_object_set_int_member(state, "navigation_sequence", b->navigation_sequence);
        json_object_set_boolean_member(state, "navigation_watchdog_active",
            b->navigation_watchdog != 0);
        json_object_set_boolean_member(state, "load_watchdog_active",
            b->load_watchdog != 0 || b->load_probe_cancel != NULL);
        json_object_set_int_member(state, "load_watchdog_seconds",
            load_watchdog_seconds(b));
        json_object_set_int_member(state, "load_stop_count", b->load_stop_count);
        json_object_set_int_member(state, "reset_count", b->reset_count);
        json_object_set_string_member(state, "committed_uri",
            b->committed_uri ? b->committed_uri : "");
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
        json_object_set_boolean_member(details, "software_rendering", b->software_rendering);
        json_object_set_boolean_member(details, "media_gl_sink_disabled",
            b->media_gl_sink_disabled);
        g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
        json_node_set_object(node, details);
        reply(r, node, NULL);
        return;
    }
    if (g_str_equal(cmd, "lease")) {
        const char *action = string_member(o, "action");
        if (!action) { reply(r, NULL, "action must be acquire, renew, or release"); return; }
        gboolean active = lease_active(b);
        if (g_str_equal(action, "acquire")) {
            if (active) {
                reply_full(r, NULL, "Browser is reserved by another agent", "lease_locked", TRUE);
                return;
            }
            b->lease_token = g_uuid_string_random();
            b->lease_expires_us = g_get_monotonic_time() +
                (gint64)b->lease_seconds * G_USEC_PER_SEC;
        } else if (g_str_equal(action, "renew") || g_str_equal(action, "release")) {
            const char *token = string_member(o, "lease");
            if (!active || !token || !g_str_equal(token, b->lease_token)) {
                reply_full(r, NULL, "A valid lease token is required", "lease_required", FALSE);
                return;
            }
            if (g_str_equal(action, "release")) {
                g_clear_pointer(&b->lease_token, g_free);
                b->lease_expires_us = 0;
            } else b->lease_expires_us = g_get_monotonic_time() +
                (gint64)b->lease_seconds * G_USEC_PER_SEC;
        } else { reply(r, NULL, "action must be acquire, renew, or release"); return; }
        g_autoptr(JsonObject) result = json_object_new();
        json_object_set_boolean_member(result, "active", b->lease_token != NULL);
        if (b->lease_token) json_object_set_string_member(result, "token", b->lease_token);
        json_object_set_int_member(result, "ttl_seconds", b->lease_seconds);
        g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
        json_node_set_object(node, result);
        reply(r, node, NULL);
        return;
    }
    const char *token = string_member(o, "lease");
    if (lease_active(b)) {
        if (!token || !g_str_equal(token, b->lease_token)) {
            reply_full(r, NULL, "Browser is reserved by another agent", "lease_locked", TRUE);
            return;
        }
    } else if (token) {
        reply_full(r, NULL, "Agent lease has expired or was released", "lease_expired", FALSE);
        return;
    }
    if (g_str_equal(cmd, "hibernate-after")) {
        JsonNode *seconds_node = json_object_get_member(o, "seconds");
        if (!seconds_node || !JSON_NODE_HOLDS_VALUE(seconds_node) ||
            json_node_get_value_type(seconds_node) != G_TYPE_INT64 ||
            json_node_get_int(seconds_node) < 0 ||
            json_node_get_int(seconds_node) > MAX_IDLE_HIBERNATE_SECONDS) {
            reply(r, NULL, "seconds must be an integer from 0 to 86400");
            return;
        }
        set_idle_hibernate(b, (guint)json_node_get_int(seconds_node));
        g_autoptr(JsonObject) result = json_object_new();
        json_object_set_int_member(result, "idle_hibernate_seconds",
            b->idle_hibernate_seconds);
        g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
        json_node_set_object(node, result);
        reply(r, node, NULL);
        return;
    }
    if (g_str_equal(cmd, "quit")) {
        r->quit = TRUE;
        reply(r, NULL, NULL);
        return;
    }
    if (g_str_equal(cmd, "profile")) {
        const char *path = string_member(o, "path");
        if (!path || !*path) {
            reply(r, NULL, "path must be a non-empty profile folder");
            return;
        }
        guint64 previous_generation = b->web_process_generation;
        g_clear_error(&error);
        gboolean changed = switch_profile(b, path, &error);
        if (error) {
            reply(r, NULL, error->message);
            return;
        }
        g_autoptr(JsonObject) operation = json_object_new();
        json_object_set_boolean_member(operation, "changed", changed);
        json_object_set_string_member(operation, "profile_path", b->profile_path);
        json_object_set_int_member(operation, "operation_id", b->reset_count);
        json_object_set_int_member(operation, "target_generation",
            changed ? previous_generation + 1 : previous_generation);
        g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
        json_node_set_object(node, operation);
        reply(r, node, NULL);
        return;
    }
    if (g_str_equal(cmd, "wake")) {
        if (b->engine_state == ENGINE_RESETTING || b->engine_state == ENGINE_RECOVERING ||
            b->engine_state == ENGINE_FAILED) {
            reply_engine_unavailable(r, b);
            return;
        }
        gboolean changed = b->engine_state == ENGINE_SUSPENDED;
        guint64 target = b->web_process_generation + (changed ? 1 : 0);
        if (changed && !wake_engine(b, "about:blank")) {
            reply(r, NULL, "Unable to wake WebKit");
            return;
        }
        note_activity(b);
        g_autoptr(JsonObject) operation = json_object_new();
        json_object_set_boolean_member(operation, "changed", changed);
        json_object_set_int_member(operation, "target_generation", target);
        g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
        json_node_set_object(node, operation);
        reply(r, node, NULL);
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
    if (g_str_equal(cmd, "memory-protection")) {
        JsonNode *enabled_node = json_object_get_member(o, "enabled");
        if (!enabled_node || !JSON_NODE_HOLDS_VALUE(enabled_node) ||
            json_node_get_value_type(enabled_node) != G_TYPE_BOOLEAN) {
            reply(r, NULL, "enabled must be a boolean");
            return;
        }
        JsonNode *threshold_node = json_object_get_member(o, "kill_threshold_mib");
        if (threshold_node && (!JSON_NODE_HOLDS_VALUE(threshold_node) ||
            json_node_get_value_type(threshold_node) != G_TYPE_INT64)) {
            reply(r, NULL, "kill_threshold_mib must be an integer");
            return;
        }
        gint64 requested = threshold_node ? json_node_get_int(threshold_node) :
            b->configured_kill_threshold;
        if (requested < 512 || requested > 65536 ||
            requested * 2 <= b->memory_limit) {
            reply(r, NULL, "kill_threshold_mib must be between 512 and 65536 MiB and greater than half the memory-pressure limit");
            return;
        }
        if (b->engine_state == ENGINE_RESETTING || b->engine_state == ENGINE_RECOVERING) {
            reply_full(r, NULL, "WebKit recovery is already in progress",
                "webkit_recovering", TRUE);
            return;
        }
        gboolean enabled = json_node_get_boolean(enabled_node);
        guint64 previous_generation = b->web_process_generation;
        gboolean changed = switch_memory_policy(b, enabled, (guint)requested);
        g_autoptr(JsonObject) operation = json_object_new();
        json_object_set_boolean_member(operation, "memory_protection_enabled",
            b->memory_protection_enabled);
        json_object_set_int_member(operation, "memory_kill_threshold_mib",
            b->memory_kill_threshold);
        json_object_set_int_member(operation, "configured_memory_kill_threshold_mib",
            b->configured_kill_threshold);
        json_object_set_boolean_member(operation, "changed", changed);
        json_object_set_int_member(operation, "operation_id", b->reset_count);
        json_object_set_int_member(operation, "target_generation",
            changed ? previous_generation + 1 : previous_generation);
        g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
        json_node_set_object(node, operation);
        reply(r, node, NULL);
        return;
    }
    if (b->engine_state == ENGINE_SUSPENDED && g_str_equal(cmd, "open")) {
        const char *uri = string_member(o, "uri");
        if (!uri || !*uri || !wake_engine(b, uri)) {
            reply(r, NULL, "uri must be an HTTP(S) URL, file URL, absolute path, or about:blank");
            return;
        }
        reply(r, NULL, NULL);
        return;
    }
    if (b->engine_state == ENGINE_SUSPENDED) {
        if (!g_str_equal(cmd, "eval") && !g_str_equal(cmd, "back") &&
            !g_str_equal(cmd, "forward") && !g_str_equal(cmd, "reload") &&
            !g_str_equal(cmd, "stop")) {
            reply(r, NULL, "Unknown command");
            return;
        }
        wake_engine(b, "about:blank");
        reply_full(r, NULL, "WebKit was sleeping and is waking; retry the page command",
            "webkit_waking", TRUE);
        return;
    }
    if (b->engine_state != ENGINE_READY || !b->view) {
        reply_engine_unavailable(r, b);
        return;
    }
    if (g_str_equal(cmd, "eval")) {
        const char *script = string_member(o, "script");
        if (!script) { reply(r, NULL, "script must be a JavaScript expression string"); return; }
        g_autofree char *body = g_strdup_printf("return await (\n%s\n);", script);
        r->page_bound = TRUE;
        r->page_generation = b->web_process_generation;
        r->page_navigation_sequence = b->navigation_sequence;
        note_activity(b);
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
    note_activity(b);
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
    b->clients++;
    r->client_counted = TRUE;
    r->timeout = g_timeout_add_seconds(b->control_timeout_seconds, request_timeout, r);
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

static gboolean profile_directory(const char *path, gboolean allow_empty_chmod,
                                   GError **error)
{
    struct stat st;
    gboolean created = FALSE;
    if (g_lstat(path, &st) != 0) {
        if (errno != ENOENT) {
            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                "Cannot inspect profile folder %s: %s", path, g_strerror(errno));
            return FALSE;
        }
        if (g_mkdir_with_parents(path, 0700) != 0) {
            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                "Cannot create profile folder %s: %s", path, g_strerror(errno));
            return FALSE;
        }
        created = TRUE;
        if (g_lstat(path, &st) != 0) {
            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                "Cannot inspect profile folder %s: %s", path, g_strerror(errno));
            return FALSE;
        }
    }
    if (!S_ISDIR(st.st_mode) || st.st_uid != getuid()) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_ACCES,
            "Profile folder must be a directory owned by you: %s", path);
        return FALSE;
    }
    if (st.st_mode & 0077) {
        gboolean empty = created;
        if (!empty && allow_empty_chmod) {
            g_autoptr(GError) directory_error = NULL;
            GDir *directory = g_dir_open(path, 0, &directory_error);
            empty = directory && g_dir_read_name(directory) == NULL;
            if (directory) g_dir_close(directory);
        }
        if (!allow_empty_chmod || !empty || g_chmod(path, 0700) != 0) {
            g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_ACCES,
                "Profile folder must have mode 0700. Create or select an empty folder: %s",
                path);
            return FALSE;
        }
    }
    return TRUE;
}

static WebKitWebsiteDataManager *open_profile_manager(const char *path,
    gboolean allow_empty_chmod, int *profile_fd, GError **error)
{
    *profile_fd = -1;
    if (!profile_directory(path, allow_empty_chmod, error)) return NULL;
    g_autofree char *lock = g_build_filename(path, ".lock", NULL);
    int fd = open(lock, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
            "Cannot open profile lock %s: %s", lock, g_strerror(errno));
        return NULL;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int lock_error = errno;
        if (lock_error == EWOULDBLOCK || lock_error == EAGAIN)
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                "Cannot lock profile (another browser may be using it): %s", path);
        else
            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(lock_error),
                "Cannot lock profile %s: %s", path, g_strerror(lock_error));
        close(fd);
        return NULL;
    }
    g_autofree char *cache = g_build_filename(path, "cache", NULL);
    WebKitWebsiteDataManager *manager = webkit_website_data_manager_new(
        "base-data-directory", path, "base-cache-directory", cache, NULL);
    g_autofree char *cookies = g_build_filename(path, "cookies.sqlite", NULL);
    webkit_cookie_manager_set_persistent_storage(
        webkit_website_data_manager_get_cookie_manager(manager), cookies,
        WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
    *profile_fd = fd;
    return manager;
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
    if (b->engine_state == ENGINE_SUSPENDED) {
        if (wake_engine(b, gtk_entry_get_text(entry)))
            gtk_widget_grab_focus(GTK_WIDGET(b->view));
        return;
    }
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
    note_activity(b);
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
    Browser *b = data;
    if (b->trace_navigation) {
        const char *uri = NULL;
        if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE)
            uri = webkit_uri_request_get_uri(webkit_response_policy_decision_get_request(
                WEBKIT_RESPONSE_POLICY_DECISION(decision)));
        else {
            WebKitNavigationAction *action = webkit_navigation_policy_decision_get_navigation_action(
                WEBKIT_NAVIGATION_POLICY_DECISION(decision));
            uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(action));
        }
        trace_navigation(b, type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE ?
            "response-policy" : "navigation-policy", uri);
    }
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
    g_clear_pointer(&b->committed_uri, g_free);
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
    if (b->trace_navigation || b->trace_resources)
        g_signal_connect(view, "resource-load-started", G_CALLBACK(trace_resource_started), b);
    if (b->trace_navigation) {
        g_signal_connect(view, "script-dialog", G_CALLBACK(trace_script_dialog), b);
        g_signal_connect(view, "notify::is-web-process-responsive",
            G_CALLBACK(trace_process_responsive), b);
    }
    g_signal_connect(view, "web-process-terminated", G_CALLBACK(process_terminated), b);
    g_signal_connect(view, "decide-policy", G_CALLBACK(decide_policy), b);
    g_signal_connect(view, "button-press-event", G_CALLBACK(note_view_input), b);
    g_signal_connect(view, "scroll-event", G_CALLBACK(note_view_input), b);
    g_signal_connect(view, "key-press-event", G_CALLBACK(note_view_input), b);
    g_signal_connect(view, "touch-event", G_CALLBACK(note_view_input), b);
    gtk_box_pack_start(GTK_BOX(b->view_box), GTK_WIDGET(view), TRUE, TRUE, 0);
    gtk_widget_show(GTK_WIDGET(view));
    return view;
}

static gboolean rebuild_web_view_for_navigation(Browser *b, const char *uri)
{
    if (!b->context || !b->view || !uri || !*uri) return FALSE;

    /* This is deliberately narrower than hard_engine_reset(): the shared
     * context, data manager, network process, profile, control socket, lease,
     * and Lightview PID all remain alive. Only the WebView and its page process
     * are replaced, which is enough to escape a permanently provisional or
     * script-wedged page without reporting a full browser reset. */
    cancel_page_requests(b);
    cancel_navigation_watchdog(b);
    cancel_deferred_navigation(b);
    cancel_load_watchdog(b);

    WebKitWebView *old_view = b->view;
    b->view = NULL;
    g_signal_handlers_disconnect_by_data(old_view, b);
    webkit_web_view_terminate_web_process(old_view);
    gtk_widget_destroy(GTK_WIDGET(old_view));

    b->view = create_web_view(b);
    if (!b->view) return FALSE;

    b->web_process_generation++;
    b->navigation_view_rebuild_count++;
    b->current_load_committed = FALSE;
    note_activity(b);
    g_clear_pointer(&b->load_error, g_free);

    /* Seed a deterministic local document before dispatching the destination.
     * This avoids the same implicit-initial-blank race handled at startup. The
     * generation and rebuild-attempt flag stay changed so a second failure is
     * bounded and escalates to the ordinary reset path instead of looping. */
    b->deferred_navigation_uri = g_strdup(uri);
    b->deferred_navigation_generation = b->web_process_generation;
    b->deferred_navigation_deadline_us = g_get_monotonic_time() +
        (gint64)DEFERRED_NAVIGATION_SETTLE_MILLISECONDS * G_TIME_SPAN_MILLISECOND;
    b->deferred_navigation = g_timeout_add(
        DEFERRED_NAVIGATION_RETRY_MILLISECONDS,
        deferred_navigation_load, b);
    webkit_web_view_load_html(b->view,
        "<!doctype html><meta charset=\"utf-8\"><title></title>",
        "about:blank");
    update_ui(b);
    g_message("Navigation maintenance rebuilt the WebView: generation=%"
        G_GUINT64_FORMAT " rebuild_count=%" G_GUINT64_FORMAT,
        b->web_process_generation, b->navigation_view_rebuild_count);
    return TRUE;
}

static void configure_memory_policy(Browser *b)
{
    b->memory_kill_threshold = b->memory_protection_enabled ?
        b->configured_kill_threshold : 0;
    /* WebKit applies MemoryPressureSettings independently to each web and
     * network process. A process-tree PSS total is therefore not comparable
     * to its per-process kill threshold: summing several healthy processes
     * made the old derived 75% threshold rebuild the engine every few
     * navigations. Keep total-PSS maintenance available as an explicit
     * operator policy, but never derive or enable it implicitly. */
    if (!b->memory_recycle_explicit)
        b->memory_recycle_threshold = 0;
    webkit_memory_pressure_settings_set_memory_limit(b->pressure, b->memory_limit);
    webkit_memory_pressure_settings_set_conservative_threshold(b->pressure, 0.25);
    webkit_memory_pressure_settings_set_strict_threshold(b->pressure, 0.50);
    webkit_memory_pressure_settings_set_kill_threshold(b->pressure,
        b->memory_protection_enabled ?
            (double)b->configured_kill_threshold / b->memory_limit : 0.0);
    webkit_memory_pressure_settings_set_poll_interval(b->pressure, 15.0);
    webkit_website_data_manager_set_memory_pressure_settings(b->pressure);
}

static void destroy_engine(Browser *b, gboolean terminate_page)
{
    g_clear_pointer(&b->committed_uri, g_free);
    cancel_navigation_watchdog(b);
    cancel_deferred_navigation(b);
    cancel_load_watchdog(b);
    if (b->view) {
        WebKitWebView *old_view = b->view;
        b->view = NULL;
        g_signal_handlers_disconnect_by_data(old_view, b);
        /* A queued JavaScript probe can retain the view even after GTK
         * destroys its widget. Force resets must also stop a wedged worker
         * so those callbacks can finish and release the retired engine. */
        if (terminate_page) webkit_web_view_terminate_web_process(old_view);
        gtk_widget_destroy(GTK_WIDGET(old_view));
    }
    g_clear_object(&b->context);
}

static gboolean wake_engine(Browser *b, const char *input)
{
    if (b->engine_state != ENGINE_SUSPENDED) return FALSE;
    g_autofree char *uri = normalize_uri(input);
    if (!uri) return FALSE;
    b->engine_state = ENGINE_RECOVERING;
    b->hard_reset_used = TRUE;
    b->context = create_web_context(b);
    b->view = create_web_view(b);
    if (!b->context || !b->view) {
        set_recovery_failure(b, "Unable to wake WebKit.");
        return FALSE;
    }
    note_activity(b);
    arm_recovery_timeout(b);
    /* Wake through the same deterministic local bootstrap used by recovery.
     * A slow remote destination must not consume the engine-startup budget or
     * commit while ENGINE_RECOVERING hides its last-visited metadata. */
    g_clear_pointer(&b->pending_navigation_uri, g_free);
    if (!g_str_equal(uri, "about:blank"))
        b->pending_navigation_uri = g_steal_pointer(&uri);
    webkit_web_view_load_html(b->view,
        "<!doctype html><meta charset=\"utf-8\"><title></title>", "about:blank");
    update_ui(b);
    g_message("WebKit waking after idle hibernation");
    return TRUE;
}

static gboolean hibernate_tick(gpointer data)
{
    Browser *b = data;
    lease_active(b);
    if (b->engine_state != ENGINE_READY || !b->view ||
        webkit_web_view_is_loading(b->view) ||
        webkit_web_view_is_playing_audio(b->view) ||
        b->downloads_active || b->page_requests ||
        g_get_monotonic_time() - b->last_activity_us <
            (gint64)b->idle_hibernate_seconds * G_USEC_PER_SEC)
        return G_SOURCE_CONTINUE;
    b->engine_state = ENGINE_SUSPENDED;
    b->web_process_generation++;
    destroy_engine(b, FALSE);
#ifdef __GLIBC__
    malloc_trim(0);
#endif
    gtk_widget_grab_focus(b->entry);
    update_ui(b);
    g_message("WebKit hibernated after %u seconds idle; generation=%" G_GUINT64_FORMAT,
        b->idle_hibernate_seconds, b->web_process_generation);
    return G_SOURCE_CONTINUE;
}

static void set_idle_hibernate(Browser *b, guint seconds)
{
    if (b->hibernate_timer) {
        g_source_remove(b->hibernate_timer);
        b->hibernate_timer = 0;
    }
    b->idle_hibernate_seconds = seconds;
    note_activity(b);
    if (seconds) b->hibernate_timer = g_timeout_add_seconds(1, hibernate_tick, b);
    if (b->idle_hibernate_spin)
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(b->idle_hibernate_spin), seconds);
}

static void hard_engine_reset(Browser *b)
{
    b->engine_state = ENGINE_RECOVERING;
    b->hard_reset_used = TRUE;
    cancel_page_requests(b);
    destroy_engine(b, TRUE);
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

static void sync_profile_control(Browser *b)
{
    if (!b->profile_label) return;
    const char *text = b->profile_path ? b->profile_path : "Private (ephemeral)";
    gtk_label_set_text(GTK_LABEL(b->profile_label), text);
    gtk_widget_set_tooltip_text(b->profile_label, text);
}

static gboolean switch_profile(Browser *b, const char *path, GError **error)
{
    if (b->engine_state == ENGINE_RESETTING || b->engine_state == ENGINE_RECOVERING) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY,
            "WebKit recovery is already in progress");
        return FALSE;
    }
    if (b->downloads_active) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY,
            "Wait for the active download to finish before changing profiles");
        return FALSE;
    }
    if (!path || !*path) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Choose a profile folder");
        return FALSE;
    }

    g_autofree char *absolute = g_canonicalize_filename(path, NULL);
    if (b->profile_path && g_str_equal(b->profile_path, absolute)) return FALSE;

    int new_fd = -1;
    WebKitWebsiteDataManager *new_manager = open_profile_manager(
        absolute, TRUE, &new_fd, error);
    if (!new_manager) return FALSE;

    cancel_page_requests(b);
    b->recovery_burst_count = 0;
    b->last_recovery_us = 0;
    record_reset(b, "profile-change");
    b->engine_state = ENGINE_RECOVERING;
    b->hard_reset_used = TRUE;

    WebKitWebsiteDataManager *old_manager = b->manager;
    int old_fd = b->profile_fd;
    destroy_engine(b, TRUE);
    b->manager = new_manager;
    b->profile_fd = new_fd;
    b->private_mode = FALSE;
    g_free(b->profile_path);
    b->profile_path = g_strdup(absolute);
    sync_profile_control(b);
    g_clear_object(&old_manager);
    if (old_fd >= 0) close(old_fd);

    b->context = create_web_context(b);
    b->view = create_web_view(b);
    if (!b->context || !b->view) {
        set_recovery_failure(b, "Unable to recreate WebKit with the selected profile.");
        return TRUE;
    }
    note_activity(b);
    arm_recovery_timeout(b);
    webkit_web_view_load_uri(b->view, "about:blank");
    update_ui(b);
    g_message("Profile changed to %s", b->profile_path);
    return TRUE;
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

static void sync_memory_controls(Browser *b)
{
    if (!b->memory_protection_toggle || !b->memory_threshold_spin) return;
    b->memory_controls_syncing = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b->memory_protection_toggle),
        b->memory_protection_enabled);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(b->memory_threshold_spin),
        b->configured_kill_threshold);
    gtk_widget_set_sensitive(b->memory_threshold_spin, b->memory_protection_enabled &&
        b->engine_state != ENGINE_RESETTING && b->engine_state != ENGINE_RECOVERING);
    b->memory_controls_syncing = FALSE;
}

static gboolean switch_memory_policy(Browser *b, gboolean enabled, guint threshold)
{
    gboolean active_change = b->memory_protection_enabled != enabled;
    gboolean threshold_change = b->configured_kill_threshold != threshold;
    if (!active_change && !threshold_change) {
        sync_memory_controls(b);
        return FALSE;
    }
    if (b->engine_state == ENGINE_RESETTING || b->engine_state == ENGINE_RECOVERING)
        return FALSE;

    b->memory_protection_enabled = enabled;
    b->configured_kill_threshold = threshold;
    configure_memory_policy(b);
    sync_memory_controls(b);

    b->recovery_burst_count = 0;
    b->last_recovery_us = 0;
    record_reset(b, enabled ? "memory-protection-change" :
        "memory-protection-disabled");
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

static void memory_protection_draft_toggled(GtkToggleButton *toggle, gpointer data)
{
    Browser *b = data;
    if (b->memory_controls_syncing || !b->memory_threshold_spin) return;
    gtk_widget_set_sensitive(b->memory_threshold_spin,
        gtk_toggle_button_get_active(toggle));
}

static void memory_policy_apply_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    Browser *b = data;
    gboolean enabled = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(b->memory_protection_toggle));
    guint threshold = (guint)gtk_spin_button_get_value_as_int(
        GTK_SPIN_BUTTON(b->memory_threshold_spin));
    if (!switch_memory_policy(b, enabled, threshold)) sync_memory_controls(b);
}

static void idle_hibernate_apply_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    Browser *b = data;
    gtk_spin_button_update(GTK_SPIN_BUTTON(b->idle_hibernate_spin));
    set_idle_hibernate(b, (guint)gtk_spin_button_get_value_as_int(
        GTK_SPIN_BUTTON(b->idle_hibernate_spin)));
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

static void profile_chooser_error(GtkWidget *chooser, const char *message)
{
    GtkWidget *label = g_object_get_data(G_OBJECT(chooser), "profile-error-label");
    gtk_label_set_text(GTK_LABEL(label), message ? message : "");
}

static GtkWidget *create_profile_chooser(GtkWindow *parent, const char *current_folder,
                                         const char *message)
{
    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        "Choose Lightview profile folder", parent,
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Select", GTK_RESPONSE_ACCEPT,
        NULL);
    gtk_window_set_modal(GTK_WINDOW(chooser), TRUE);
    gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(chooser), TRUE);
    gtk_file_chooser_set_create_folders(GTK_FILE_CHOOSER(chooser), TRUE);
    if (current_folder)
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser), current_folder);
    GtkWidget *error_label = gtk_label_new(message ? message : "");
    gtk_label_set_xalign(GTK_LABEL(error_label), 0);
    gtk_label_set_line_wrap(GTK_LABEL(error_label), TRUE);
    gtk_widget_set_size_request(error_label, 400, -1);
    gtk_style_context_add_class(gtk_widget_get_style_context(error_label), "error");
    gtk_file_chooser_set_extra_widget(GTK_FILE_CHOOSER(chooser), error_label);
    g_object_set_data(G_OBJECT(chooser), "profile-error-label", error_label);
    return chooser;
}

static void profile_chooser_response(GtkDialog *dialog, gint response, gpointer data)
{
    Browser *b = data;
    if (response == GTK_RESPONSE_ACCEPT) {
        g_autofree char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        g_autoptr(GError) error = NULL;
        switch_profile(b, path, &error);
        if (error) {
            profile_chooser_error(GTK_WIDGET(dialog), error->message);
            return;
        }
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void profile_choose_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    Browser *b = data;
    GtkWidget *chooser = create_profile_chooser(
        b->version_dialog ? GTK_WINDOW(b->version_dialog) : GTK_WINDOW(b->window),
        b->profile_path ? b->profile_path : g_get_user_data_dir(), NULL);
    g_signal_connect(chooser, "response", G_CALLBACK(profile_chooser_response), b);
    gtk_widget_show_all(chooser);
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

    GtkWidget *memory_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(memory_grid), 6);
    gtk_grid_set_row_spacing(GTK_GRID(memory_grid), 6);
    b->memory_protection_toggle = gtk_check_button_new_with_label(
        "WebKit memory kill protection");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b->memory_protection_toggle),
        b->memory_protection_enabled);
    GtkAdjustment *threshold_adjustment = gtk_adjustment_new(
        b->configured_kill_threshold,
        MAX(512.0, (double)b->memory_limit / 2.0 + 1.0), 65536.0,
        256.0, 1024.0, 0.0);
    b->memory_threshold_spin = gtk_spin_button_new(threshold_adjustment, 1.0, 0);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(b->memory_threshold_spin), TRUE);
    gtk_widget_set_sensitive(b->memory_threshold_spin, b->memory_protection_enabled);
    GtkWidget *threshold_label = gtk_label_new("Kill threshold (MiB)");
    gtk_label_set_xalign(GTK_LABEL(threshold_label), 0);
    b->memory_apply_button = gtk_button_new_with_label("Apply memory policy");
    GtkWidget *memory_note = gtk_label_new(
        "When disabled, WebKit will not kill a page for excessive memory.\n"
        "The operating system or container may still terminate it.");
    gtk_label_set_xalign(GTK_LABEL(memory_note), 0);
    gtk_label_set_line_wrap(GTK_LABEL(memory_note), TRUE);
    gtk_grid_attach(GTK_GRID(memory_grid), b->memory_protection_toggle, 0, 0, 2, 1);
    gtk_grid_attach(GTK_GRID(memory_grid), threshold_label, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(memory_grid), b->memory_threshold_spin, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(memory_grid), b->memory_apply_button, 0, 2, 2, 1);
    gtk_grid_attach(GTK_GRID(memory_grid), memory_note, 0, 3, 2, 1);
    gtk_box_pack_start(GTK_BOX(gtk_message_dialog_get_message_area(
        GTK_MESSAGE_DIALOG(b->version_dialog))), memory_grid, FALSE, FALSE, 6);
    g_signal_connect(b->memory_protection_toggle, "toggled",
        G_CALLBACK(memory_protection_draft_toggled), b);
    g_signal_connect(b->memory_apply_button, "clicked",
        G_CALLBACK(memory_policy_apply_clicked), b);
    g_signal_connect(b->memory_protection_toggle, "destroy",
        G_CALLBACK(gtk_widget_destroyed), &b->memory_protection_toggle);
    g_signal_connect(b->memory_threshold_spin, "destroy",
        G_CALLBACK(gtk_widget_destroyed), &b->memory_threshold_spin);
    g_signal_connect(b->memory_apply_button, "destroy",
        G_CALLBACK(gtk_widget_destroyed), &b->memory_apply_button);

    GtkWidget *idle_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(idle_grid), 6);
    gtk_grid_set_row_spacing(GTK_GRID(idle_grid), 6);
    GtkWidget *idle_label = gtk_label_new("Hibernate after idle (seconds)");
    gtk_label_set_xalign(GTK_LABEL(idle_label), 0);
    GtkAdjustment *idle_adjustment = gtk_adjustment_new(
        b->idle_hibernate_seconds, 0, MAX_IDLE_HIBERNATE_SECONDS, 60, 300, 0);
    b->idle_hibernate_spin = gtk_spin_button_new(idle_adjustment, 1.0, 0);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(b->idle_hibernate_spin), TRUE);
    atk_object_set_name(gtk_widget_get_accessible(b->idle_hibernate_spin),
        "Hibernate after idle (seconds)");
    b->idle_hibernate_apply_button = gtk_button_new_with_label("Apply idle time");
    GtkWidget *idle_note = gtk_label_new(
        "0 disables automatic hibernation. Applying starts a new idle countdown.");
    gtk_label_set_xalign(GTK_LABEL(idle_note), 0);
    gtk_label_set_line_wrap(GTK_LABEL(idle_note), TRUE);
    gtk_grid_attach(GTK_GRID(idle_grid), idle_label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(idle_grid), b->idle_hibernate_spin, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(idle_grid), b->idle_hibernate_apply_button, 0, 1, 2, 1);
    gtk_grid_attach(GTK_GRID(idle_grid), idle_note, 0, 2, 2, 1);
    gtk_box_pack_start(GTK_BOX(gtk_message_dialog_get_message_area(
        GTK_MESSAGE_DIALOG(b->version_dialog))), idle_grid, FALSE, FALSE, 6);
    g_signal_connect(b->idle_hibernate_apply_button, "clicked",
        G_CALLBACK(idle_hibernate_apply_clicked), b);
    g_signal_connect(b->idle_hibernate_spin, "destroy",
        G_CALLBACK(gtk_widget_destroyed), &b->idle_hibernate_spin);
    g_signal_connect(b->idle_hibernate_apply_button, "destroy",
        G_CALLBACK(gtk_widget_destroyed), &b->idle_hibernate_apply_button);

    GtkWidget *profile_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(profile_grid), 6);
    gtk_grid_set_row_spacing(GTK_GRID(profile_grid), 6);
    GtkWidget *profile_title = gtk_label_new("Profile folder");
    gtk_label_set_xalign(GTK_LABEL(profile_title), 0);
    b->profile_label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(b->profile_label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(b->profile_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(b->profile_label), 48);
    gtk_label_set_selectable(GTK_LABEL(b->profile_label), TRUE);
    sync_profile_control(b);
    b->profile_button = gtk_button_new_with_label("Choose profile folder…");
    gtk_widget_set_tooltip_text(b->profile_button,
        "Rebuild WebKit with another persistent profile while keeping the control connection");
    gtk_grid_attach(GTK_GRID(profile_grid), profile_title, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(profile_grid), b->profile_label, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(profile_grid), b->profile_button, 0, 1, 2, 1);
    gtk_box_pack_start(GTK_BOX(gtk_message_dialog_get_message_area(
        GTK_MESSAGE_DIALOG(b->version_dialog))), profile_grid, FALSE, FALSE, 6);
    g_signal_connect(b->profile_button, "clicked",
        G_CALLBACK(profile_choose_clicked), b);
    g_signal_connect(b->profile_label, "destroy",
        G_CALLBACK(gtk_widget_destroyed), &b->profile_label);
    g_signal_connect(b->profile_button, "destroy",
        G_CALLBACK(gtk_widget_destroyed), &b->profile_button);
    g_signal_connect(b->version_dialog, "response", G_CALLBACK(close_dialog), NULL);
    g_signal_connect(b->version_dialog, "destroy", G_CALLBACK(gtk_widget_destroyed),
        &b->version_dialog);
    gtk_widget_show_all(b->version_dialog);
    update_ui(b);
}

static gboolean open_startup_profile(Browser *b, const char *requested_path)
{
    g_autofree char *absolute = g_canonicalize_filename(requested_path, NULL);
    g_autoptr(GError) error = NULL;
    b->manager = open_profile_manager(absolute, FALSE, &b->profile_fd, &error);
    if (!b->manager && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_BUSY)) {
        g_printerr("%s\n", error->message);
        return FALSE;
    }
    if (!b->manager) {
        GtkWidget *chooser = create_profile_chooser(NULL, absolute, error->message);
        gtk_widget_show_all(chooser);
        while (!b->manager) {
            if (gtk_dialog_run(GTK_DIALOG(chooser)) != GTK_RESPONSE_ACCEPT) {
                gtk_widget_destroy(chooser);
                g_printerr("Profile selection cancelled.\n");
                return FALSE;
            }
            g_autofree char *selected = gtk_file_chooser_get_filename(
                GTK_FILE_CHOOSER(chooser));
            g_autofree char *selected_absolute = selected ?
                g_canonicalize_filename(selected, NULL) : NULL;
            g_clear_error(&error);
            if (selected_absolute)
                b->manager = open_profile_manager(selected_absolute, TRUE,
                    &b->profile_fd, &error);
            else
                g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Choose a profile folder");
            if (b->manager) {
                g_free(absolute);
                absolute = g_steal_pointer(&selected_absolute);
            } else profile_chooser_error(chooser, error->message);
        }
        gtk_widget_destroy(chooser);
    }
    b->profile_path = g_steal_pointer(&absolute);
    return TRUE;
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
    b.trace_navigation = environment_truthy(g_getenv("LIGHTVIEW_TRACE_NAVIGATION"));
    b.trace_resources = environment_truthy(g_getenv("LIGHTVIEW_TRACE_RESOURCES"));
    gboolean drm_accessible = has_accessible_drm_render_node();
    const char *software_override = g_getenv("LIBGL_ALWAYS_SOFTWARE");
    if (!software_override && !drm_accessible) {
        g_setenv("LIBGL_ALWAYS_SOFTWARE", "1", FALSE);
        software_override = "1";
        g_message("No accessible DRM render node; using Mesa software rendering");
    }
    b.software_rendering = environment_truthy(software_override);
    const char *media_gl_override = g_getenv("WEBKIT_GST_DISABLE_GL_SINK");
    if (!media_gl_override && !drm_accessible) {
        g_setenv("WEBKIT_GST_DISABLE_GL_SINK", "1", FALSE);
        media_gl_override = "1";
        g_message("No accessible DRM render node; disabling GStreamer GL video sink");
    }
    b.media_gl_sink_disabled = environment_truthy(media_gl_override);
    b.profile_fd = -1;
    b.engine_state = ENGINE_READY;
    b.web_process_generation = 1;
    gboolean no_control = FALSE, version = FALSE, disable_memory_kill = FALSE;
    gint memory_limit = 0, memory_kill_threshold = 0, memory_recycle_threshold = -1;
    gint network_recycle_threshold = DEFAULT_NETWORK_RECYCLE_THRESHOLD_MIB;
    gint network_thread_recycle_threshold = DEFAULT_NETWORK_THREAD_RECYCLE_THRESHOLD;
    gint idle_hibernate = DEFAULT_IDLE_HIBERNATE_SECONDS;
    gint control_timeout = DEFAULT_CONTROL_TIMEOUT_SECONDS;
    gint lease_ttl = DEFAULT_LEASE_SECONDS;
    g_autofree char *profile = NULL;
    g_auto(GStrv) urls = NULL;
    GOptionEntry options[] = {
        {"private", 0, 0, G_OPTION_ARG_NONE, &b.private_mode, "Keep website data only for this session", NULL},
        {"profile", 0, 0, G_OPTION_ARG_FILENAME, &profile, "Persistent profile directory", "DIR"},
        {"socket", 0, 0, G_OPTION_ARG_FILENAME, &b.socket_path, "Control socket in a private directory", "PATH"},
        {"no-control", 0, 0, G_OPTION_ARG_NONE, &no_control, "Disable the automation socket", NULL},
        {"no-images", 0, 0, G_OPTION_ARG_NONE, &b.no_images, "Skip automatic image loading", NULL},
        {"memory-limit", 0, 0, G_OPTION_ARG_INT, &memory_limit, "WebKit per-process memory-pressure limit in MiB", "MIB"},
        {"memory-kill-threshold", 0, 0, G_OPTION_ARG_INT, &memory_kill_threshold, "Terminate a WebKit process above this threshold", "MIB"},
        {"memory-recycle-threshold", 0, 0, G_OPTION_ARG_INT, &memory_recycle_threshold, "Rebuild WebKit before navigation when process-tree PSS reaches MiB (0 disables)", "MIB"},
        {"network-recycle-threshold", 0, 0, G_OPTION_ARG_INT, &network_recycle_threshold, "Replace only WebKit's network process before navigation when its PSS reaches MiB (0 disables)", "MIB"},
        {"network-thread-recycle-threshold", 0, 0, G_OPTION_ARG_INT, &network_thread_recycle_threshold, "Replace only WebKit's network process before navigation when its thread count reaches this value (0 disables)", "COUNT"},
        {"disable-memory-kill", 0, 0, G_OPTION_ARG_NONE, &disable_memory_kill, "Never terminate WebKit for exceeding the memory threshold", NULL},
        {"idle-hibernate", 0, 0, G_OPTION_ARG_INT, &idle_hibernate, "Suspend WebKit after idle seconds (default 3600; 0 disables)", "SECONDS"},
        {"control-timeout", 0, 0, G_OPTION_ARG_INT, &control_timeout, "Cancel a WebKit page operation after this many seconds", "SECONDS"},
        {"lease-ttl", 0, 0, G_OPTION_ARG_INT, &lease_ttl, "Agent lease lifetime in seconds", "SECONDS"},
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
    if (idle_hibernate < 0 || idle_hibernate > MAX_IDLE_HIBERNATE_SECONDS ||
        control_timeout < 1 || control_timeout > 300 ||
        lease_ttl < 1 || lease_ttl > 3600) {
        g_printerr("--idle-hibernate must be 0–86400 seconds, --control-timeout must be 1–300 seconds, and --lease-ttl must be 1–3600 seconds.\n");
        return 1;
    }
    b.idle_hibernate_seconds = (guint)idle_hibernate;
    b.control_timeout_seconds = (guint)control_timeout;
    b.lease_seconds = (guint)lease_ttl;
    b.memory_limit_explicit = memory_limit != 0;
    if (memory_limit == 0) memory_limit = b.low_memory ? 384 : 768;
    if (memory_limit < 128 || memory_limit > 65536) {
        g_printerr("--memory-limit must be between 128 and 65536 MiB.\n"); return 1;
    }
    b.memory_limit = (guint)memory_limit;
    if (memory_kill_threshold == 0)
        memory_kill_threshold = MIN(65536,
            MAX(DEFAULT_KILL_THRESHOLD_MIB, memory_limit * 4));
    if (memory_kill_threshold < 512 || memory_kill_threshold > 65536 ||
        memory_kill_threshold * 2 <= memory_limit) {
        g_printerr("--memory-kill-threshold must be between 512 and 65536 MiB and greater than half the memory-pressure limit.\n");
        return 1;
    }
    b.memory_protection_enabled = !disable_memory_kill;
    b.configured_kill_threshold = (guint)memory_kill_threshold;
    b.memory_recycle_explicit = memory_recycle_threshold >= 0;
    if (memory_recycle_threshold > 65536 ||
        (memory_recycle_threshold > 0 && memory_recycle_threshold < 256) ||
        (b.memory_protection_enabled && memory_recycle_threshold > 0 &&
         memory_recycle_threshold >= memory_kill_threshold)) {
        g_printerr("--memory-recycle-threshold must be 0 or 256–65536 MiB and lower than the enabled kill threshold.\n");
        return 1;
    }
    if (b.memory_recycle_explicit)
        b.memory_recycle_threshold = (guint)memory_recycle_threshold;
    if (network_recycle_threshold < 0 || network_recycle_threshold > 65536) {
        g_printerr("--network-recycle-threshold must be 0–65536 MiB.\n");
        return 1;
    }
    b.network_recycle_threshold = (guint)network_recycle_threshold;
    if (network_thread_recycle_threshold < 0 ||
        network_thread_recycle_threshold > 65536) {
        g_printerr("--network-thread-recycle-threshold must be 0–65536.\n");
        return 1;
    }
    b.network_thread_recycle_threshold = (guint)network_thread_recycle_threshold;
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
    if (b.private_mode) b.manager = webkit_website_data_manager_new_ephemeral();
    else {
        g_autofree char *default_profile = NULL;
        if (!profile) default_profile = g_build_filename(g_get_user_data_dir(),
            "lightview", NULL);
        if (!open_startup_profile(&b, profile ? profile : default_profile)) return 1;
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
    g_autofree char *initial_uri = normalize_uri(urls ? urls[0] : "about:blank");
    if (!initial_uri) {
        g_printerr("Unsupported URL\n");
        return 1;
    }
    /* Queue a real initial document before exposing the control socket. A new
     * WebView can report itself as loading even though its implicit blank state
     * will not emit a load-finished event. A local HTML document gives startup
     * a deterministic first commit; navigate() can safely queue an agent's
     * immediate first URL behind it. */
    note_activity(&b);
    arm_navigation_watchdog(&b, initial_uri);
    if (g_str_equal(initial_uri, "about:blank"))
        webkit_web_view_load_html(b.view, "<!doctype html><title></title>",
            "about:blank");
    else
        webkit_web_view_load_uri(b.view, initial_uri);
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
    sample_resource_usage(&b);
    b.resource_timer = g_timeout_add_seconds(2, sample_resource_usage, &b);
    set_idle_hibernate(&b, b.idle_hibernate_seconds);
    gtk_widget_grab_focus(urls ? GTK_WIDGET(b.view) : b.entry);
    gtk_main();
    gboolean terminate_on_exit = b.load_probe_cancel != NULL || b.page_requests != NULL;
    if (b.resource_timer) g_source_remove(b.resource_timer);
    if (b.hibernate_timer) g_source_remove(b.hibernate_timer);
    if (b.flash_timeout) g_source_remove(b.flash_timeout);
    if (b.recovery_timeout) g_source_remove(b.recovery_timeout);
    cancel_load_watchdog(&b);
    if (b.service) {
        g_socket_service_stop(b.service);
        g_socket_listener_close(G_SOCKET_LISTENER(b.service));
        g_unlink(b.socket_path);
        g_object_unref(b.service);
    }
    /* A control-socket quit used to leave the still-live WebView owned by the
     * GTK window and then let process exit tear down WebKit abruptly. In
     * particular, a just-completed localStorage write could remain in the
     * WebProcess journal and be lost intermittently. Destroy the view while
     * retaining its context, briefly service the WebProcess shutdown, and
     * only then release the context/network process. */
    cancel_page_requests(&b);
    cancel_navigation_watchdog(&b);
    cancel_deferred_navigation(&b);
    cancel_load_watchdog(&b);
    if (b.view) {
        WebKitWebView *old_view = b.view;
        b.view = NULL;
        g_signal_handlers_disconnect_by_data(old_view, &b);
        if (terminate_on_exit) webkit_web_view_terminate_web_process(old_view);
        gtk_widget_destroy(GTK_WIDGET(old_view));
    }
    drain_main_context_for(100);
    g_clear_object(&b.context);
    drain_main_context_for(100);
    g_clear_object(&b.settings);
    g_clear_object(&b.manager);
    webkit_memory_pressure_settings_free(b.pressure);
    if (b.profile_fd >= 0) close(b.profile_fd);
    g_free(b.socket_path);
    g_free(b.profile_path);
    g_free(b.load_error);
    g_free(b.download_dir);
    g_free(b.download_message);
    g_free(b.committed_uri);
    g_free(b.last_committed_uri);
    g_free(b.last_termination_reason);
    g_free(b.last_reset_at);
    g_free(b.pending_navigation_uri);
    g_free(b.navigation_watchdog_uri);
    g_free(b.deferred_navigation_uri);
    g_free(b.lease_token);
    return 0;
}
