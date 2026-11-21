/* Diagnostic for GStreamer 1.28 asynchronous provider-start cancellation.
 * Default: reproduce the old immediate enumerate/stop sequence (may crash).
 * "wait": wait for the documented startup message before enumerate/stop.
 * Run only in a disposable test environment with native GStreamer >= 1.28.
 */
#include <gst/gst.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    gboolean wait = argc > 1 && !strcmp(argv[1], "wait");
    for (int i = 0; i < 200; i++) {
        GstDeviceMonitor *monitor = gst_device_monitor_new();
        GstCaps *caps = gst_caps_new_empty_simple("audio/x-raw");
        gst_device_monitor_add_filter(monitor, "Audio/Sink", caps);
        gst_caps_unref(caps);
        gboolean started = gst_device_monitor_start(monitor);
        if (started && wait) {
            GstBus *bus = gst_device_monitor_get_bus(monitor);
            GstMessage *message = gst_bus_timed_pop_filtered(bus,
                10 * GST_SECOND, GST_MESSAGE_DEVICE_MONITOR_STARTED);
            if (!message) {
                fprintf(stderr, "Device-monitor startup did not complete\n");
                gst_object_unref(bus);
                gst_device_monitor_stop(monitor);
                gst_object_unref(monitor);
                return 3;
            }
            gst_message_unref(message);
            gst_object_unref(bus);
        }
        GList *devices = gst_device_monitor_get_devices(monitor);
        g_list_free_full(devices, (GDestroyNotify)gst_object_unref);
        if (started)
            gst_device_monitor_stop(monitor);
        gst_object_unref(monitor);
        printf("iteration %d\n", i + 1);
        fflush(stdout);
    }
    return 0;
}
