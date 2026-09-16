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

typedef struct {
    GtkWidget *window, *entry, *back, *forward, *message;
    WebKitWebView *view;
    GSocketService *service;
    char *socket_path, *load_error;
    guint clients, memory_limit;
    gboolean private_mode, low_memory, resetting;
} Browser;

typedef struct {
    Browser *browser;
    GSocketConnection *connection;
    GCancellable *cancel;
    GString *input;
    char *output;
    guint timeout;
    gboolean quit;
} Request;

static gboolean quit_browser(gpointer unused)
{
    (void)unused;
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}

static void request_free(Request *r)
{
    if (r->timeout) g_source_remove(r->timeout);
    g_io_stream_close(G_IO_STREAM(r->connection), NULL, NULL);
    g_object_unref(r->connection);
    g_object_unref(r->cancel);
    g_string_free(r->input, TRUE);
    g_free(r->output);
    r->browser->clients--;
    g_free(r);
}

static gboolean request_timeout(gpointer data)
{
    Request *r = data;
    r->timeout = 0;
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

static void reply(Request *r, JsonNode *value, const char *error)
{
    g_autoptr(JsonBuilder) b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "ok");
    json_builder_add_boolean_value(b, error == NULL);
    json_builder_set_member_name(b, error ? "error" : "result");
    if (error) json_builder_add_string_value(b, error);
    else if (value) json_builder_add_value(b, json_node_copy(value));
    else json_builder_add_null_value(b);
    json_builder_end_object(b);
    g_autoptr(JsonNode) root = json_builder_get_root(b);
    g_autofree char *json = json_to_string(root, FALSE);
    if (strlen(json) > MAX_RESPONSE) {
        reply(r, NULL, "Response exceeds 4 MiB; extract a smaller result");
        return;
    }
    r->output = g_strconcat(json, "\n", NULL);
    g_output_stream_write_all_async(
        g_io_stream_get_output_stream(G_IO_STREAM(r->connection)),
        r->output, strlen(r->output), G_PRIORITY_DEFAULT, r->cancel, written, r);
}

static const char *string_member(JsonObject *o, const char *name)
{
    JsonNode *n = json_object_get_member(o, name);
    return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING
        ? json_node_get_string(n) : NULL;
}

static void update_ui(Browser *b)
{
    const char *title = webkit_web_view_get_title(b->view);
    const char *uri = webkit_web_view_get_uri(b->view);
    g_autofree char *caption = g_strdup_printf("%s — Lightview%s",
        title && *title ? title : "Browser", b->private_mode ? " (private)" : "");
    gtk_window_set_title(GTK_WINDOW(b->window), caption);
    if (!gtk_widget_has_focus(b->entry))
        gtk_entry_set_text(GTK_ENTRY(b->entry), uri ? uri : "");
    gtk_widget_set_sensitive(b->back, webkit_web_view_can_go_back(b->view));
    gtk_widget_set_sensitive(b->forward, webkit_web_view_can_go_forward(b->view));
    gtk_label_set_text(GTK_LABEL(b->message), b->load_error ? b->load_error :
        (webkit_web_view_is_loading(b->view) ? "Loading…" : ""));
    gtk_widget_set_visible(b->message,
        b->load_error != NULL || webkit_web_view_is_loading(b->view));
}

static void notify_view(GObject *object, GParamSpec *pspec, gpointer data)
{
    (void)object; (void)pspec;
    update_ui(data);
}

static void load_changed(WebKitWebView *view, WebKitLoadEvent event, gpointer data)
{
    (void)view;
    Browser *b = data;
    if (event == WEBKIT_LOAD_STARTED) g_clear_pointer(&b->load_error, g_free);
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

static void process_terminated(WebKitWebView *view, WebKitWebProcessTerminationReason reason,
                               gpointer data)
{
    (void)view; (void)reason;
    Browser *b = data;
    if (b->resetting) {
        b->resetting = FALSE;
        g_clear_pointer(&b->load_error, g_free);
        webkit_web_view_load_uri(b->view, "about:blank");
        return;
    }
    g_free(b->load_error);
    b->load_error = g_strdup("Web process stopped. Reload to recover.");
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
    g_autofree char *uri = normalize_uri(input);
    if (!uri) return FALSE;
    g_clear_pointer(&b->load_error, g_free);
    webkit_web_view_load_uri(b->view, uri);
    return TRUE;
}

static void javascript_done(GObject *object, GAsyncResult *result, gpointer data)
{
    Request *r = data;
    g_autoptr(GError) error = NULL;
    g_autoptr(JSCValue) value = webkit_web_view_call_async_javascript_function_finish(
        WEBKIT_WEB_VIEW(object), result, &error);
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
        const char *uri = webkit_web_view_get_uri(b->view);
        const char *title = webkit_web_view_get_title(b->view);
        json_object_set_string_member(state, "uri", uri ? uri : "");
        json_object_set_string_member(state, "title", title ? title : "");
        json_object_set_boolean_member(state, "loading", webkit_web_view_is_loading(b->view));
        json_object_set_boolean_member(state, "can_go_back", webkit_web_view_can_go_back(b->view));
        json_object_set_boolean_member(state, "can_go_forward", webkit_web_view_can_go_forward(b->view));
        json_object_set_boolean_member(state, "private", b->private_mode);
        json_object_set_boolean_member(state, "low_memory", b->low_memory);
        json_object_set_int_member(state, "memory_limit_mib", b->memory_limit);
        json_object_set_int_member(state, "pid", getpid());
        if (b->load_error) json_object_set_string_member(state, "load_error", b->load_error);
        else json_object_set_null_member(state, "load_error");
        g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
        json_node_set_object(node, state);
        reply(r, node, NULL);
        return;
    }
    if (g_str_equal(cmd, "eval")) {
        const char *script = string_member(o, "script");
        if (!script) { reply(r, NULL, "script must be a JavaScript expression string"); return; }
        g_autofree char *body = g_strdup_printf("return await (\n%s\n);", script);
        webkit_web_view_call_async_javascript_function(b->view, body, -1, NULL,
            NULL, NULL, r->cancel, javascript_done, r);
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
    else if (g_str_equal(cmd, "reset")) {
        if (b->resetting) { reply(r, NULL, "Web process reset already in progress"); return; }
        b->resetting = TRUE;
        webkit_web_view_terminate_web_process(b->view);
    }
    else if (g_str_equal(cmd, "quit")) r->quit = TRUE;
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
    if (navigate(b, gtk_entry_get_text(entry))) gtk_widget_grab_focus(GTK_WIDGET(b->view));
}

static void go_back(GtkButton *button, gpointer data)
{ (void)button; webkit_web_view_go_back(((Browser *)data)->view); }
static void go_forward(GtkButton *button, gpointer data)
{ (void)button; webkit_web_view_go_forward(((Browser *)data)->view); }
static void reload(GtkButton *button, gpointer data)
{ (void)button; webkit_web_view_reload(((Browser *)data)->view); }

static gboolean key_pressed(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
    (void)widget;
    Browser *b = data;
    guint key = gdk_keyval_to_lower(event->keyval);
    if ((event->state & GDK_CONTROL_MASK) && key == GDK_KEY_l) {
        gtk_widget_grab_focus(b->entry);
        gtk_editable_select_region(GTK_EDITABLE(b->entry), 0, -1);
    } else if (key == GDK_KEY_F5 || ((event->state & GDK_CONTROL_MASK) && key == GDK_KEY_r))
        webkit_web_view_reload(b->view);
    else if ((event->state & GDK_MOD1_MASK) && key == GDK_KEY_Left)
        webkit_web_view_go_back(b->view);
    else if ((event->state & GDK_MOD1_MASK) && key == GDK_KEY_Right)
        webkit_web_view_go_forward(b->view);
    else if (key == GDK_KEY_Escape) webkit_web_view_stop_loading(b->view);
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

int main(int argc, char **argv)
{
    Browser b = {0};
    gboolean no_control = FALSE, no_images = FALSE, browser_cache = FALSE, version = FALSE;
    gint memory_limit = 0;
    g_autofree char *profile = NULL;
    g_auto(GStrv) urls = NULL;
    GOptionEntry options[] = {
        {"private", 0, 0, G_OPTION_ARG_NONE, &b.private_mode, "Keep website data only for this session", NULL},
        {"profile", 0, 0, G_OPTION_ARG_FILENAME, &profile, "Persistent profile directory", "DIR"},
        {"socket", 0, 0, G_OPTION_ARG_FILENAME, &b.socket_path, "Control socket in a private directory", "PATH"},
        {"no-control", 0, 0, G_OPTION_ARG_NONE, &no_control, "Disable the automation socket", NULL},
        {"no-images", 0, 0, G_OPTION_ARG_NONE, &no_images, "Skip automatic image loading", NULL},
        {"memory-limit", 0, 0, G_OPTION_ARG_INT, &memory_limit, "WebKit per-process memory-pressure limit in MiB", "MIB"},
        {"low-memory", 0, 0, G_OPTION_ARG_NONE, &b.low_memory, "Reduce optional web features and use a 384 MiB limit", NULL},
        {"browser-cache", 0, 0, G_OPTION_ARG_NONE, &browser_cache, "Favor repeat-load speed over memory savings", NULL},
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
        g_print("Lightview 0.1.0 (WebKitGTK %u.%u.%u)\n", webkit_get_major_version(),
            webkit_get_minor_version(), webkit_get_micro_version());
        return 0;
    }
    if ((urls && g_strv_length(urls) > 1) || (profile && b.private_mode)) {
        g_printerr("Use one URL; --profile and --private are mutually exclusive.\n"); return 1;
    }
    if (memory_limit == 0) memory_limit = b.low_memory ? 384 : 768;
    if (memory_limit < 128 || memory_limit > 65536) {
        g_printerr("--memory-limit must be between 128 and 65536 MiB.\n"); return 1;
    }
    b.memory_limit = (guint)memory_limit;
    if (b.low_memory) no_images = TRUE;
    if (!gtk_init_check(&argc, &argv)) {
        g_printerr("A graphical display is required (XFCE/X11 or Wayland).\n"); return 1;
    }
    set_application_icon();
    g_autoptr(WebKitMemoryPressureSettings) pressure = webkit_memory_pressure_settings_new();
    webkit_memory_pressure_settings_set_memory_limit(pressure, b.memory_limit);
    webkit_memory_pressure_settings_set_conservative_threshold(pressure, 0.25);
    webkit_memory_pressure_settings_set_strict_threshold(pressure, 0.50);
    webkit_memory_pressure_settings_set_poll_interval(pressure, 15.0);
    webkit_website_data_manager_set_memory_pressure_settings(pressure);
    int profile_fd = -1;
    g_autoptr(WebKitWebsiteDataManager) manager = NULL;
    if (b.private_mode) manager = webkit_website_data_manager_new_ephemeral();
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
        manager = webkit_website_data_manager_new("base-data-directory", absolute,
            "base-cache-directory", cache, NULL);
        g_autofree char *cookies = g_build_filename(absolute, "cookies.sqlite", NULL);
        webkit_cookie_manager_set_persistent_storage(
            webkit_website_data_manager_get_cookie_manager(manager), cookies,
            WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
    }
    g_autoptr(WebKitWebContext) context = g_object_new(WEBKIT_TYPE_WEB_CONTEXT,
        "website-data-manager", manager,
        "memory-pressure-settings", pressure,
        NULL);
    webkit_web_context_set_sandbox_enabled(context, TRUE);
    webkit_web_context_set_cache_model(context, browser_cache ?
        WEBKIT_CACHE_MODEL_WEB_BROWSER : WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER);
    webkit_web_context_set_spell_checking_enabled(context, FALSE);
    g_autoptr(WebKitSettings) settings = webkit_settings_new_with_settings(
        "enable-javascript", TRUE,
        "enable-page-cache", browser_cache,
        "auto-load-images", !no_images,
        "media-playback-requires-user-gesture", TRUE,
        "enable-media", !b.low_memory,
        "enable-mediasource", !b.low_memory,
        "enable-encrypted-media", !b.low_memory,
        "enable-webaudio", !b.low_memory,
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
    gtk_widget_set_tooltip_text(b.back, "Back (Alt+Left)");
    gtk_widget_set_tooltip_text(b.forward, "Forward (Alt+Right)");
    gtk_widget_set_tooltip_text(refresh, "Reload (Ctrl+R)");
    b.entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(b.entry), "Address (Ctrl+L)");
    gtk_box_pack_start(GTK_BOX(toolbar), b.back, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), b.forward, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), refresh, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), b.entry, TRUE, TRUE, 0);
    b.view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
        "web-context", context, "settings", settings, NULL));
    b.message = gtk_label_new(NULL);
    gtk_label_set_ellipsize(GTK_LABEL(b.message), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(b.message), 0);
    gtk_box_pack_start(GTK_BOX(column), toolbar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(column), GTK_WIDGET(b.view), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(column), b.message, FALSE, FALSE, 2);
    gtk_container_add(GTK_CONTAINER(b.window), column);
    g_signal_connect(b.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(b.window, "key-press-event", G_CALLBACK(key_pressed), &b);
    g_signal_connect(b.entry, "activate", G_CALLBACK(address_activate), &b);
    g_signal_connect(b.back, "clicked", G_CALLBACK(go_back), &b);
    g_signal_connect(b.forward, "clicked", G_CALLBACK(go_forward), &b);
    g_signal_connect(refresh, "clicked", G_CALLBACK(reload), &b);
    g_signal_connect(b.view, "notify::title", G_CALLBACK(notify_view), &b);
    g_signal_connect(b.view, "notify::uri", G_CALLBACK(notify_view), &b);
    g_signal_connect(b.view, "load-changed", G_CALLBACK(load_changed), &b);
    g_signal_connect(b.view, "load-failed", G_CALLBACK(load_failed), &b);
    g_signal_connect(b.view, "web-process-terminated", G_CALLBACK(process_terminated), &b);
    g_signal_connect(b.view, "decide-policy", G_CALLBACK(decide_policy), &b);
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
    update_ui(&b);
    gtk_widget_grab_focus(urls ? GTK_WIDGET(b.view) : b.entry);
    gtk_main();
    if (b.service) {
        g_socket_service_stop(b.service);
        g_socket_listener_close(G_SOCKET_LISTENER(b.service));
        g_unlink(b.socket_path);
        g_object_unref(b.service);
    }
    if (profile_fd >= 0) close(profile_fd);
    g_free(b.socket_path);
    g_free(b.load_error);
    return 0;
}
