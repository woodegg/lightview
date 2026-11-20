#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RUNTIME "/usr/local/lib/lightview-diagnostics/"
#define LIBRARIES RUNTIME "webkit-preload-timeout-v6-candidate-2.52.6-20261117/lib:" \
    RUNTIME "webkit-source-loop-2.52.6/lib:" \
    RUNTIME "libsoup-redirect-budget-3.8.1-20261118/lib"
#define PLUGINS RUNTIME "libav-width-guard-v3-1.24.1/lib/gstreamer-1.0"
#define BROWSER "/usr/local/libexec/lightview-bin"

int main(int argc, char **argv)
{
    (void)argc;
    if (setenv("LD_LIBRARY_PATH", LIBRARIES, 1) != 0 ||
        setenv("GST_PLUGIN_PATH", PLUGINS, 1) != 0) {
        perror("lightview: configure private runtime");
        return EXIT_FAILURE;
    }
    const char *cache = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    char *registry = NULL;
    if (cache && cache[0] == '/') {
        if (asprintf(&registry, "%s/lightview-gstreamer-0.1.12.bin", cache) < 0)
            registry = NULL;
    } else if (home && home[0] == '/') {
        if (asprintf(&registry, "%s/.cache/lightview-gstreamer-0.1.12.bin", home) < 0)
            registry = NULL;
    }
    if (!registry || setenv("GST_REGISTRY", registry, 1) != 0) {
        free(registry);
        fprintf(stderr, "lightview: cannot configure the private plugin registry\n");
        return EXIT_FAILURE;
    }
    free(registry);
    /* Retain the browser's GTK/AT-SPI application identity through both launchers. */
    argv[0] = (char *)"lightview";
    execv(BROWSER, argv);
    fprintf(stderr, "lightview: execute %s: %s\n", BROWSER, strerror(errno));
    return EXIT_FAILURE;
}
