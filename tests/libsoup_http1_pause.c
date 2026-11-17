/* Native regression for libsoup's private asynchronous pause interface.
 * Linked only into a test executable, following upstream's static test build. */
#include "libsoup/soup.h"
#include "libsoup/soup-session-private.h"
#include <string.h>

static SoupSession *session;
static SoupMessage *message;
static GTlsCertificate *certificate;
static GMainLoop *loop;
static const char *base;
static gint64 started;
static double paused_at, resumed_at;
static guint writes, completions, expected_writes;
static gboolean ordinary, cancel_paused, passed, timed_out;

static double seconds(void)
{
    return (g_get_monotonic_time() - started) / 1000000.0;
}

static gboolean accept_certificate(SoupMessage *msg, GTlsCertificate *peer,
                                   GTlsCertificateFlags errors, gpointer data)
{
    return g_tls_certificate_is_same(peer, certificate);
}

static gboolean resume_message(gpointer data)
{
    resumed_at = seconds();
    soup_session_unpause_message(session, message);
    return G_SOURCE_REMOVE;
}

static gboolean cancel_message(gpointer data)
{
    soup_session_cancel_message(session, message);
    return G_SOURCE_REMOVE;
}

static gboolean pause_message(gpointer data)
{
    soup_session_pause_message(session, message);
    paused_at = seconds();
    g_timeout_add(cancel_paused ? 1000 : 25000,
                  cancel_paused ? cancel_message : resume_message, NULL);
    return G_SOURCE_REMOVE;
}

static void wrote_headers(SoupMessage *msg, gpointer data)
{
    if (++writes == expected_writes)
        g_timeout_add(1000, pause_message, NULL);
}

static void finished(GObject *source, GAsyncResult *result, gpointer data)
{
    GError *error = NULL;
    GBytes *bytes = soup_session_send_and_read_finish(session, result, &error);
    gboolean cancelled = error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    if (error) {
        g_printerr("request-error: %s\n", error->message);
        g_clear_error(&error);
    }
    if (!GPOINTER_TO_INT(data)) {
        gsize size = 0;
        const char *payload = bytes ? g_bytes_get_data(bytes, &size) : NULL;
        gboolean warm = soup_message_get_status(message) == 200 && size == 4 &&
                        payload && memcmp(payload, "warm", 4) == 0;
        g_clear_pointer(&bytes, g_bytes_unref);
        if (!warm) {
            g_main_loop_quit(loop);
            return;
        }
        g_clear_object(&message);
        char *uri = g_strconcat(base, "/first", NULL);
        message = soup_message_new("GET", uri);
        g_free(uri);
        g_signal_connect(message, "accept-certificate", G_CALLBACK(accept_certificate), NULL);
        g_signal_connect(message, "wrote-headers", G_CALLBACK(wrote_headers), NULL);
        soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL,
                                         finished, GINT_TO_POINTER(1));
        return;
    }
    completions++;
    if (cancel_paused) {
        passed = cancelled && paused_at > 20 && seconds() < 25;
    } else if (bytes) {
        gsize size;
        const char *payload = g_bytes_get_data(bytes, &size);
        passed = soup_message_get_status(message) == 200 &&
                 soup_message_get_http_version(message) == SOUP_HTTP_1_1 &&
                 size == 8 && memcmp(payload, "guard-ok", 8) == 0 &&
                 paused_at > (ordinary ? 0 : 20) && resumed_at - paused_at >= 24.9 &&
                 seconds() < (ordinary ? 31 : 51);
    }
    passed = passed && writes == expected_writes && completions == 1;
    g_clear_pointer(&bytes, g_bytes_unref);
    g_main_loop_quit(loop);
}

static gboolean deadline(gpointer data)
{
    timed_out = TRUE;
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
    if (argc != 4 || (strcmp(argv[3], "ordinary") && strcmp(argv[3], "fallback") &&
                      strcmp(argv[3], "cancel")))
        return 2;
    ordinary = strcmp(argv[3], "ordinary") == 0;
    cancel_paused = strcmp(argv[3], "cancel") == 0;
    expected_writes = ordinary ? 1 : 2;
    if (ordinary)
        g_setenv("SOUP_FORCE_HTTP1", "1", TRUE);
    else
        g_unsetenv("SOUP_FORCE_HTTP1");
    base = argv[1];
    GError *error = NULL;
    certificate = g_tls_certificate_new_from_file(argv[2], &error);
    if (!certificate) {
        g_printerr("certificate-error: %s\n", error->message);
        g_clear_error(&error);
        return 2;
    }
    session = soup_session_new();
    GProxyResolver *proxy = g_simple_proxy_resolver_new(NULL, NULL);
    g_object_set(session, "timeout", 0, "proxy-resolver", proxy, NULL);
    g_object_unref(proxy);
    loop = g_main_loop_new(NULL, FALSE);
    started = g_get_monotonic_time();
    char *uri = g_strconcat(base, "/warm", NULL);
    message = soup_message_new("GET", uri);
    g_free(uri);
    g_signal_connect(message, "accept-certificate", G_CALLBACK(accept_certificate), NULL);
    soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL,
                                     finished, GINT_TO_POINTER(0));
    g_timeout_add_seconds(ordinary ? 35 : 55, deadline, NULL);
    g_main_loop_run(loop);
    g_print("{\"passed\":%s,\"timeout\":%s,\"paused_at\":%.3f,"
            "\"resumed_at\":%.3f,\"seconds\":%.3f,\"writes\":%u,\"completions\":%u}\n",
            passed ? "true" : "false", timed_out ? "true" : "false",
            paused_at, resumed_at, seconds(), writes, completions);
    soup_session_abort(session);
    gint64 until = g_get_monotonic_time() + G_USEC_PER_SEC;
    while (g_get_monotonic_time() < until) {
        while (g_main_context_iteration(NULL, FALSE));
        g_usleep(1000);
    }
    g_clear_object(&message);
    g_object_unref(session);
    g_object_unref(certificate);
    g_main_loop_unref(loop);
    return passed && !timed_out ? 0 : 1;
}
