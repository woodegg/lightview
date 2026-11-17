#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <stdio.h>

static GstPadQueryFunction original_query;
static guint allocations;
static gint padded_allocations;
static gboolean inject_padding;
static gboolean strict_meta = TRUE;
static const gchar *fault_mode;
static gboolean expected_decode_error;

typedef struct { GstVideoBufferPool parent; } PaddedMetaPool;
typedef struct { GstVideoBufferPoolClass parent; } PaddedMetaPoolClass;
G_DEFINE_TYPE(PaddedMetaPool, padded_meta_pool, GST_TYPE_VIDEO_BUFFER_POOL)

static GstFlowReturn
padded_alloc(GstBufferPool *pool, GstBuffer **buffer, GstBufferPoolAcquireParams *params)
{
    GstFlowReturn ret = GST_BUFFER_POOL_CLASS(padded_meta_pool_parent_class)->alloc_buffer(pool, buffer, params);
    if (ret == GST_FLOW_OK) {
        GstVideoMeta *meta = gst_buffer_get_video_meta(*buffer);
        /* Change only the descriptor: preserve the allocation and decoded
         * picture while exercising the production invariant and error paths. */
        if (meta && meta->width == 640) {
            if (g_str_equal(fault_mode, "height"))
                meta->height += 2;
            else if (g_str_equal(fault_mode, "undersized"))
                meta->width -= 2;
            else {
                meta->width += 2;
                if (g_str_equal(fault_mode, "incompatible"))
                    meta->format = GST_VIDEO_FORMAT_Y42B;
            }
            g_atomic_int_inc(&padded_allocations);
        }
    }
    return ret;
}

static void padded_meta_pool_class_init(PaddedMetaPoolClass *klass)
{
    GST_BUFFER_POOL_CLASS(klass)->alloc_buffer = padded_alloc;
}

static void padded_meta_pool_init(PaddedMetaPool *pool) { (void) pool; }

/* Advertise the video-meta buffer pool used by a zero-copy video consumer. */
static gboolean
sink_query(GstPad *pad, GstObject *parent, GstQuery *query)
{
    if (GST_QUERY_TYPE(query) != GST_QUERY_ALLOCATION)
        return original_query(pad, parent, query);
    GstCaps *caps = NULL;
    gboolean need_pool;
    gst_query_parse_allocation(query, &caps, &need_pool);
    GstVideoInfo info;
    if (!caps || !gst_video_info_from_caps(&info, caps))
        return FALSE;
    GstBufferPool *pool = inject_padding ? g_object_new(padded_meta_pool_get_type(), NULL) : gst_video_buffer_pool_new();
    GstStructure *config = gst_buffer_pool_get_config(pool);
    gst_buffer_pool_config_set_params(config, caps, info.size, 0, 0);
    gst_buffer_pool_config_add_option(config, GST_BUFFER_POOL_OPTION_VIDEO_META);
    if (!gst_buffer_pool_set_config(pool, config)) {
        gst_object_unref(pool);
        return FALSE;
    }
    gst_query_add_allocation_pool(query, pool, info.size, 0, 0);
    gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, NULL);
    gst_object_unref(pool);
    allocations++;
    return TRUE;
}

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    if (argc != 2 && argc != 3) return 2;
    fault_mode = argc == 3 ? argv[2] : "normal";
    inject_padding = argc == 3;
    expected_decode_error = g_str_equal(fault_mode, "undersized") ||
                            g_str_equal(fault_mode, "incompatible");
    strict_meta = argc != 3 || !g_str_equal(argv[2], "padded-abort-control");
    GError *error = NULL;
    GstElement *pipeline = gst_parse_launch(
        "filesrc name=source ! h264parse ! avdec_h264 name=decoder max-threads=4 ! "
        "appsink name=sink sync=false enable-last-sample=false", &error);
    if (!pipeline || error) {
        fprintf(stderr, "pipeline: %s\n", error ? error->message : "unknown error");
        return 2;
    }
    GstElement *source = gst_bin_get_by_name(GST_BIN(pipeline), "source");
    g_object_set(source, "location", argv[1], NULL);
    gst_object_unref(source);
    GstElement *decoder = gst_bin_get_by_name(GST_BIN(pipeline), "decoder");
    GstElementFactory *factory = gst_element_get_factory(decoder);
    GstPlugin *plugin = gst_plugin_feature_get_plugin(GST_PLUGIN_FEATURE(factory));
    fprintf(stderr, "Selected decoder plugin: %s\n", gst_plugin_get_filename(plugin));
    gst_object_unref(plugin);
    gst_object_unref(decoder);
    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    GstPad *pad = gst_element_get_static_pad(sink, "sink");
    original_query = GST_PAD_QUERYFUNC(pad);
    gst_pad_set_query_function(pad, GST_DEBUG_FUNCPTR(sink_query));
    gst_object_unref(pad);
    GstBus *bus = gst_element_get_bus(pipeline);
    guint frames = 0, red = 0, blue = 0, meta_frames = 0;
    gboolean failed = FALSE, eos = FALSE, rejected_layout = FALSE;
    gint64 deadline = g_get_monotonic_time() + 30 * G_USEC_PER_SEC;
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    while (g_get_monotonic_time() < deadline) {
        GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), GST_SECOND);
        if (sample) {
            GstBuffer *buffer = gst_sample_get_buffer(sample);
            GstVideoInfo info;
            GstVideoFrame frame;
            GstVideoMeta *meta = gst_buffer_get_video_meta(buffer);
            if (!gst_video_info_from_caps(&info, gst_sample_get_caps(sample))) {
                failed = TRUE;
            } else if (strict_meta && meta && (meta->width != (guint) info.width || meta->height != (guint) info.height)) {
                fprintf(stderr, "output metadata still differs from negotiated picture: %ux%u vs %dx%d\n",
                        meta->width, meta->height, info.width, info.height);
                failed = TRUE;
            }
            if (failed ||
                !gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) {
                fprintf(stderr, "could not map decoded sample\n");
                failed = TRUE;
            } else {
                guint8 y = *((guint8 *) GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
                guint8 u = *((guint8 *) GST_VIDEO_FRAME_PLANE_DATA(&frame, 1));
                guint8 v = *((guint8 *) GST_VIDEO_FRAME_PLANE_DATA(&frame, 2));
                gboolean red_picture = info.width == 318;
                for (guint plane = 0; plane < 3; plane++) {
                    guint width = plane ? (info.width+1)/2 : info.width;
                    guint height = plane ? (info.height+1)/2 : info.height;
                    guint8 *data = GST_VIDEO_FRAME_PLANE_DATA(&frame, plane);
                    gint stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, plane);
                    for (guint iy = 0; iy < 3; iy++) for (guint ix = 0; ix < 3; ix++) {
                        guint x = ix == 0 ? 0 : ix == 1 ? width/2 : width-1;
                        guint y_index = iy == 0 ? 0 : iy == 1 ? height/2 : height-1;
                        guint8 pixel = data[y_index*stride+x];
                        gboolean good = plane == 0 ?
                            (red_picture ? pixel > 65 && pixel < 100 : pixel > 25 && pixel < 65) :
                            plane == 1 ? (red_picture ? pixel < 120 : pixel > 200) :
                            (red_picture ? pixel > 200 : pixel < 140);
                        if (!good) { fprintf(stderr, "invalid frame pixel plane%u (%u,%u): %u\n", plane, x, y_index, pixel); failed = TRUE; }
                    }
                }
                if (info.width == 318 && info.height == 242 && y > 65 && y < 100 && u < 120 && v > 200)
                    red++;
                else if (info.width == 640 && (info.height == 360 || info.height == 384) && y > 25 && y < 65 && u > 200 && v < 140)
                    blue++;
                else {
                    fprintf(stderr, "invalid decoded shape/pixels: %dx%d YUV %u %u %u\n",
                            info.width, info.height, y, u, v);
                    failed = TRUE;
                }
                meta_frames += gst_buffer_get_video_meta(buffer) != NULL;
                gst_video_frame_unmap(&frame);
            }
            frames++;
            gst_sample_unref(sample);
            if (failed) break;
        }
        GstMessage *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        if (message) {
            gchar *debug = NULL;
            gst_message_parse_error(message, &error, &debug);
            fprintf(stderr, "decode error: %s %s\n", error->message, debug ? debug : "");
            rejected_layout = expected_decode_error &&
                g_str_has_prefix(error->message, "Output buffer cannot hold the decoded picture");
            g_clear_error(&error);
            g_free(debug);
            gst_message_unref(message);
            failed = TRUE;
            break;
        }
        if (!sample && gst_app_sink_is_eos(GST_APP_SINK(sink))) {
            eos = TRUE;
            break;
        }
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(bus);
    gst_object_unref(sink);
    gst_object_unref(pipeline);
    printf("{\"frames\":%u,\"red_frames\":%u,\"blue_frames\":%u,\"meta_frames\":%u,\"allocations\":%u,\"padded_allocations\":%d,\"eos\":%s,\"failed\":%s,\"rejected_layout\":%s}\n",
        frames, red, blue, meta_frames, allocations, g_atomic_int_get(&padded_allocations), eos ? "true" : "false", failed ? "true" : "false", rejected_layout ? "true" : "false");
    if (expected_decode_error)
        return !rejected_layout || frames >= 288 || !padded_allocations;
    return failed || !eos || frames != 288 || red != 144 || blue != 144 || (inject_padding && !padded_allocations);
}
