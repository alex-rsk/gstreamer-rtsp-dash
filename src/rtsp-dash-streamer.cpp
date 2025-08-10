#include <gst/gst.h>
#include <glib.h>
#include <iostream>
#include <string>

class RTSPDashStreamer {
private:
    GstElement *pipeline;
    GstElement *rtsp_src;
    GstElement *dummy_src;
    GstElement *input_selector;
    GstElement *tee;
    GstElement *dash_sink_fullhd;
    GstElement *dash_sink_hd;
    GstBus *bus;
    GMainLoop *loop;
    guint bus_watch_id;
    guint reconnect_timeout_id;
    std::string rtsp_uri;
    std::string output_path;
    bool is_rtsp_connected;
    bool tee_linked;
    
public:
    RTSPDashStreamer(const std::string& uri, const std::string& output) 
        : pipeline(nullptr), rtsp_src(nullptr), dummy_src(nullptr),
          input_selector(nullptr), tee(nullptr), dash_sink_fullhd(nullptr),
          dash_sink_hd(nullptr), bus(nullptr), loop(nullptr),
          bus_watch_id(0), reconnect_timeout_id(0),
          rtsp_uri(uri), output_path(output), is_rtsp_connected(false), tee_linked(false) {}
    
    ~RTSPDashStreamer() {
        cleanup();
    }
    
    bool initialize() {
        std::cout << "Initialize" << std::endl;
        pipeline = gst_pipeline_new("rtsp-dash-pipeline");
        if (!pipeline) {
            g_printerr("Failed to create pipeline\n");
            return false;
        }
        
        // Create RTSP source
        rtsp_src = gst_element_factory_make("rtspsrc", "rtsp-source");
        if (!rtsp_src) {
            g_printerr("Failed to create rtspsrc element\n");
            return false;
        }
        
        // Configure RTSP source for reliability
        g_object_set(rtsp_src,
            "location", rtsp_uri.c_str(),
            "retry", 999,
            "protocols", 4,
            "timeout", G_GUINT64_CONSTANT(5000000), // 5 seconds
            "tcp-timeout", G_GUINT64_CONSTANT(5000000),
            "do-retransmission", TRUE,
            "drop-on-latency", TRUE, 
            "latency", 200, // 200ms latency
            NULL);
        
        // Create dummy video source (test pattern)
        dummy_src = gst_element_factory_make("videotestsrc", "dummy-source");
        if (!dummy_src) {
            g_printerr("Failed to create videotestsrc element\n");
            return false;
        }        
        
        // Configure dummy source
        g_object_set(dummy_src,
            "pattern", 18,
            "is-live", TRUE,
            NULL);
        
        // Create input selector to switch between RTSP and dummy
        input_selector = gst_element_factory_make("input-selector", "input-selector");
        if (!input_selector) {
            g_printerr("Failed to create input-selector element\n");
            return false;
        }
        
        // Create tee for splitting stream
        tee = gst_element_factory_make("tee", "tee");
        if (!tee) {
            g_printerr("Failed to create tee element\n");
            return false;
        }
        
        // Add core elements to pipeline first
        gst_bin_add_many(GST_BIN(pipeline), 
            rtsp_src, dummy_src, input_selector, tee, NULL);
        
        // Connect dummy source to input selector first
        if (!connect_dummy_source()) {
            return false;
        }
        
        // Link input selector to tee BEFORE creating DASH pipelines
        if (!gst_element_link(input_selector, tee)) {
            g_printerr("Failed to link input selector to tee\n");
            return false;
        }
        
        // Now create DASH pipelines - tee is ready to provide source pads
        if (!create_dash_pipeline("fullhd", 1920, 1080, 5000) ||
            !create_dash_pipeline("hd", 1280, 720, 3000)) {
            return false;
        }
        
        // Set up bus monitoring
        setup_bus_monitoring();
        
        // Connect RTSP source dynamically (when pads are available)
        g_signal_connect(rtsp_src, "pad-added", G_CALLBACK(on_rtsp_pad_added), this);
        g_signal_connect(rtsp_src, "no-more-pads", G_CALLBACK(on_rtsp_no_more_pads), this);
        
        return true;
    }
    
    bool start() {
        if (!pipeline) {
            g_printerr("Pipeline not initialized\n");
            return false;
        }
        
        // Start with dummy source active
        switch_to_dummy_source();
        
        // Set pipeline to playing state
        GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
        if (ret == GST_STATE_CHANGE_FAILURE) {
            g_printerr("Failed to start pipeline\n");
            return false;
        }
        
        // Create and run main loop
        loop = g_main_loop_new(NULL, FALSE);
        
        g_print("Starting RTSP to DASH streaming...\n");
        g_print("RTSP URI: %s\n", rtsp_uri.c_str());
        g_print("Output path: %s\n", output_path.c_str());
        g_print("Press Ctrl+C to stop\n");
        
        g_main_loop_run(loop);
        
        return true;
    }
    
    void stop() {
        if (loop) {
            g_main_loop_quit(loop);
        }
    }

private:
    bool create_dash_pipeline(const std::string& quality, int width, int height, int bitrate) {
        std::string sink_name = "dash-sink-" + quality;
        std::string enc_name = "encoder-" + quality;
        std::string scale_name = "scale-" + quality;
        std::string rate_name = "rate-" + quality;
        std::string parse_name = "parse-" + quality;
        std::string queue_name = "queue-" + quality;
        
        // Create elements for this quality
        GstElement *queue = gst_element_factory_make("queue", queue_name.c_str());
        GstElement *videoconvert = gst_element_factory_make("videoconvert", NULL);
        GstElement *videoscale = gst_element_factory_make("videoscale", scale_name.c_str());
        GstElement *videorate = gst_element_factory_make("videorate", rate_name.c_str());
        GstElement *capsfilter = gst_element_factory_make("capsfilter", NULL);
        GstElement *encoder = gst_element_factory_make("openh264enc", enc_name.c_str());
        GstElement *h264parse = gst_element_factory_make("h264parse", parse_name.c_str());
        GstElement *dash_sink = gst_element_factory_make("dashsink", sink_name.c_str());
        
        if (!queue || !videoconvert || !videoscale || !videorate || 
            !capsfilter || !encoder || !h264parse || !dash_sink) {
            g_printerr("Failed to create elements for %s quality\n", quality.c_str());
            return false;
        }
        
        // Configure caps for resolution and framerate
        GstCaps *caps = gst_caps_new_simple("video/x-raw",
            "format", G_TYPE_STRING, "I420",
            "width", G_TYPE_INT, width,
            "height", G_TYPE_INT, height,
            "framerate", GST_TYPE_FRACTION, 25, 1,
            NULL);
        g_object_set(capsfilter, "caps", caps, NULL);
        gst_caps_unref(caps);
        
        // Configure encoder
        g_object_set(encoder,
            "bitrate", bitrate,
        NULL);
        
        // Configure DASH sink
        std::string manifest_path = "./manifest.mpd";
        
        g_object_set(dash_sink,
            "mpd-filename", manifest_path.c_str(),
            "muxer", 0,
            "target-duration", 4, // 4 second segments        
            "use-segment-list", TRUE,
            "mpd-baseurl" , "./",
            "mpd-root-path", output_path.c_str(),        
            "send-keyframe-requests", TRUE,
        NULL);

        // Add elements to pipeline
        gst_bin_add_many(GST_BIN(pipeline),
            queue, videoconvert, videoscale, videorate, 
            capsfilter, encoder, h264parse, dash_sink, NULL);                

        // Link elements in the encoding chain
        if (!gst_element_link_many(queue, videoconvert, videoscale, 
                                   videorate, capsfilter, encoder, 
                                   h264parse, dash_sink, NULL)) {
            g_printerr("Failed to link %s pipeline elements\n", quality.c_str());
            return false;
        }

        // Request tee pad and link to queue
        GstPad *tee_pad = gst_element_get_request_pad(tee, "src_%u");
        if (!tee_pad) {
            g_printerr("Failed to get tee source pad for %s\n", quality.c_str());
            return false;
        }
        
        GstPad *queue_pad = gst_element_get_static_pad(queue, "sink");
        if (!queue_pad) {
            g_printerr("Failed to get queue sink pad for %s\n", quality.c_str());
            gst_object_unref(tee_pad);
            return false;
        }
        
        // Sync state of new elements with pipeline
        gst_element_sync_state_with_parent(queue);
        gst_element_sync_state_with_parent(videoconvert);
        gst_element_sync_state_with_parent(videoscale);
        gst_element_sync_state_with_parent(videorate);
        gst_element_sync_state_with_parent(capsfilter);
        gst_element_sync_state_with_parent(encoder);
        gst_element_sync_state_with_parent(h264parse);
        gst_element_sync_state_with_parent(dash_sink);
        
        if (gst_pad_link(tee_pad, queue_pad) != GST_PAD_LINK_OK) {
            g_printerr("Failed to link tee to %s queue\n", quality.c_str());
            gst_object_unref(tee_pad);
            gst_object_unref(queue_pad);
            return false;
        }
        
        g_print("Successfully linked tee to %s queue\n", quality.c_str());
        
        gst_object_unref(tee_pad);
        gst_object_unref(queue_pad);
        
        // Store reference to dash sink
        if (quality == "fullhd") {
            dash_sink_fullhd = dash_sink;
        } else {
            dash_sink_hd = dash_sink;
        }
        
        return true;
    }
    
    bool connect_dummy_source() {
        // Create caps filter for dummy source
        GstElement *dummy_convert = gst_element_factory_make("videoconvert", "dummy-convert");
        GstElement *dummy_caps = gst_element_factory_make("capsfilter", "dummy-caps");
        
        if (!dummy_caps || !dummy_convert) {
            g_printerr("Failed to create dummy source elements\n");
            return false;
        }
        
        // Set caps for dummy source to match expected format
        GstCaps *caps = gst_caps_new_simple("video/x-raw",
            "format", G_TYPE_STRING, "I420",
            "width", G_TYPE_INT, 1920,
            "height", G_TYPE_INT, 1080,
            "framerate", GST_TYPE_FRACTION, 25, 1,
            NULL);
        g_object_set(dummy_caps, "caps", caps, NULL);
        gst_caps_unref(caps);
        
        gst_bin_add_many(GST_BIN(pipeline), dummy_convert, dummy_caps, NULL);
        
        // Link dummy source chain
        if (!gst_element_link_many(dummy_src, dummy_convert, dummy_caps, NULL)) {
            g_printerr("Failed to link dummy source elements\n");
            return false;
        }
        
        // Connect to input selector
        GstPad *dummy_pad = gst_element_get_static_pad(dummy_caps, "src");
        GstPad *selector_pad = gst_element_get_request_pad(input_selector, "sink_%u");
        
        if (gst_pad_link(dummy_pad, selector_pad) != GST_PAD_LINK_OK) {
            g_printerr("Failed to link dummy source to input selector\n");
            gst_object_unref(dummy_pad);
            return false;
        }
        
        // Store selector pad for later activation
        g_object_set_data(G_OBJECT(input_selector), "dummy-pad", selector_pad);
        
        gst_object_unref(dummy_pad);
        
        return true;
    }
    
    void setup_bus_monitoring() {
        bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
        bus_watch_id = gst_bus_add_watch(bus, (GstBusFunc)bus_message_handler, this);
    }
    
    static gboolean bus_message_handler(GstBus *bus, GstMessage *msg, gpointer user_data) {
        RTSPDashStreamer *streamer = static_cast<RTSPDashStreamer*>(user_data);
        return streamer->handle_bus_message(msg);
    }
    
    gboolean handle_bus_message(GstMessage *msg) {
        switch (GST_MESSAGE_TYPE(msg)) {
            case GST_MESSAGE_ERROR: {
                GError *err;
                gchar *debug;
                gst_message_parse_error(msg, &err, &debug);
                
                // Check if error is from RTSP source
                if (GST_MESSAGE_SRC(msg) == GST_OBJECT(rtsp_src)) {
                    g_printerr("RTSP Error: %s\n", err->message);
                    g_printerr("Debug info: %s\n", debug ? debug : "none");
                    
                    // Switch to dummy source and try to reconnect
                    switch_to_dummy_source();
                    schedule_rtsp_reconnect();
                } else {
                    g_printerr("Pipeline Error: %s\n", err->message);
                    g_printerr("Debug info: %s\n", debug ? debug : "none");
                    stop();
                }
                
                g_error_free(err);
                g_free(debug);
                break;
            }
            case GST_MESSAGE_EOS:
                g_print("End of stream\n");
                stop();
                break;
                
            case GST_MESSAGE_STATE_CHANGED: {
                if (GST_MESSAGE_SRC(msg) == GST_OBJECT(rtsp_src)) {
                    GstState old_state, new_state;
                    gst_message_parse_state_changed(msg, &old_state, &new_state, NULL);
                    
                    if (new_state == GST_STATE_PLAYING) {
                        g_print("RTSP source connected successfully\n");
                        is_rtsp_connected = true;
                        switch_to_rtsp_source();
                    } else if (old_state == GST_STATE_PLAYING && new_state < GST_STATE_PLAYING) {
                        g_print("RTSP source disconnected\n");
                        is_rtsp_connected = false;
                        switch_to_dummy_source();
                        schedule_rtsp_reconnect();
                    }
                }
                break;
            }
            default:
                break;
        }
        return TRUE;
    }
    
    static void on_rtsp_pad_added(GstElement *src, GstPad *pad, gpointer user_data) {
        RTSPDashStreamer *streamer = static_cast<RTSPDashStreamer*>(user_data);
        streamer->connect_rtsp_pad(pad);
    }
    
    static void on_rtsp_no_more_pads(GstElement *src, gpointer user_data) {
        g_print("RTSP: No more pads\n");
    }
    
    void connect_rtsp_pad(GstPad *pad) {
        GstCaps *caps = gst_pad_get_current_caps(pad);
        if (!caps) {
            caps = gst_pad_query_caps(pad, NULL);
        }
        
        if (caps) {
            GstStructure *structure = gst_caps_get_structure(caps, 0);
            const gchar *name = gst_structure_get_name(structure);
            
            g_print("RTSP pad added: %s\n", name);
            
            // We're interested in video streams
            if (g_str_has_prefix(name, "application/x-rtp") && 
                gst_structure_has_field(structure, "media")) {
                const gchar *media = gst_structure_get_string(structure, "media");
                std::cout << "Media: " << media << std::endl;
                if (g_strcmp0(media, "video") == 0) {
                    // Create RTP depayloader and decoder chain
                    create_rtsp_decode_chain(pad);
                }
            }
            
            gst_caps_unref(caps);
        }
    }
    
    void create_rtsp_decode_chain(GstPad *pad) {
        // Create decode chain elements
        GstElement *depay = gst_element_factory_make("rtph264depay", "rtsp-depay");
        GstElement *parse = gst_element_factory_make("h264parse", "rtsp-parse");
        GstElement *decode = gst_element_factory_make("avdec_h264", "rtsp-decode");
        GstElement *convert = gst_element_factory_make("videoconvert", "rtsp-convert");
        GstElement *rtsp_caps = gst_element_factory_make("capsfilter", "rtsp-caps");
        
        if (!depay || !parse || !decode || !convert || !rtsp_caps) {
            g_printerr("Failed to create RTSP decode chain elements\n");
            return;
        }
        
        // Set caps for RTSP output to match dummy source format
        GstCaps *caps = gst_caps_new_simple("video/x-raw",
            "format", G_TYPE_STRING, "I420",
            "width", G_TYPE_INT, 1920,
            "height", G_TYPE_INT, 1080,
            "framerate", GST_TYPE_FRACTION, 25, 1,
            NULL);
        g_object_set(rtsp_caps, "caps", caps, NULL);
        gst_caps_unref(caps);
        
        gst_bin_add_many(GST_BIN(pipeline), depay, parse, decode, convert, rtsp_caps, NULL);
        
        // Link decode chain
        if (!gst_element_link_many(depay, parse, decode, convert, rtsp_caps, NULL)) {
            g_printerr("Failed to link RTSP decode chain\n");
            return;
        }
        
        // Connect RTSP pad to depayloader
        GstPad *depay_sink = gst_element_get_static_pad(depay, "sink");
        if (gst_pad_link(pad, depay_sink) != GST_PAD_LINK_OK) {
            g_printerr("Failed to link RTSP pad to depayloader\n");
        }
        gst_object_unref(depay_sink);
        
        // Connect convert output to input selector
        GstPad *convert_src = gst_element_get_static_pad(rtsp_caps, "src");
        GstPad *selector_pad = gst_element_get_request_pad(input_selector, "sink_%u");
        
        if (gst_pad_link(convert_src, selector_pad) != GST_PAD_LINK_OK) {
            g_printerr("Failed to link RTSP chain to input selector\n");
        }
        
        // Store selector pad for later activation
        g_object_set_data(G_OBJECT(input_selector), "rtsp-pad", selector_pad);
        
        gst_object_unref(convert_src);
        
        // Sync states
        gst_element_sync_state_with_parent(depay);
        gst_element_sync_state_with_parent(parse);
        gst_element_sync_state_with_parent(decode);
        gst_element_sync_state_with_parent(convert);
        gst_element_sync_state_with_parent(rtsp_caps);
        
         // Wait for elements to reach PLAYING state
        GstStateChangeReturn ret;
        ret = gst_element_get_state(rtsp_caps, NULL, NULL, GST_CLOCK_TIME_NONE);
        if (ret == GST_STATE_CHANGE_SUCCESS) {
            g_print("RTSP decode chain ready, switching input\n");
            // Now it's safe to switch
            g_timeout_add_seconds(1, switch_to_rtsp_delayed, this);
        }
        g_print("RTSP decode chain created and linked\n");
    }


    static gboolean switch_to_rtsp_delayed(gpointer user_data) {
        RTSPDashStreamer *streamer = static_cast<RTSPDashStreamer*>(user_data);
        streamer->switch_to_rtsp_source();
        return FALSE;
    }
    
    void switch_to_dummy_source() {
        GstPad *dummy_pad = (GstPad*)g_object_get_data(G_OBJECT(input_selector), "dummy-pad");
        if (dummy_pad) {
            g_object_set(input_selector, "active-pad", dummy_pad, NULL);
            g_print("Switched to dummy source (blank frames)\n");
        }
    }
    
    void switch_to_rtsp_source() {
        GstPad *rtsp_pad = (GstPad*)g_object_get_data(G_OBJECT(input_selector), "rtsp-pad");
        if (rtsp_pad) {
            g_object_set(input_selector, "active-pad", rtsp_pad, NULL);
            g_print("Switched to RTSP source\n");
        }
    }
    
    void schedule_rtsp_reconnect() {
        if (reconnect_timeout_id > 0) {
            g_source_remove(reconnect_timeout_id);
        }
        
        // Try to reconnect after 5 seconds
        reconnect_timeout_id = g_timeout_add_seconds(5, 
            (GSourceFunc)reconnect_rtsp_source, this);
    }
    
    static gboolean reconnect_rtsp_source(gpointer user_data) {
        RTSPDashStreamer *streamer = static_cast<RTSPDashStreamer*>(user_data);
        
        g_print("Attempting RTSP reconnection...\n");
        
        // Reset RTSP source state
        gst_element_set_state(streamer->rtsp_src, GST_STATE_NULL);
        gst_element_set_state(streamer->rtsp_src, GST_STATE_PLAYING);
        
        streamer->reconnect_timeout_id = 0;
        return FALSE; // Remove timeout
    }
    
    void cleanup() {
        if (reconnect_timeout_id > 0) {
            g_source_remove(reconnect_timeout_id);
            reconnect_timeout_id = 0;
        }
        
        if (bus_watch_id > 0) {
            g_source_remove(bus_watch_id);
            bus_watch_id = 0;
        }
        
        if (pipeline) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
            pipeline = nullptr;
        }
        
        if (bus) {
            gst_object_unref(bus);
            bus = nullptr;
        }
        
        if (loop) {
            g_main_loop_unref(loop);
            loop = nullptr;
        }
    }
};

// Signal handler for graceful shutdown
RTSPDashStreamer *g_streamer = nullptr;

void signal_handler(int signal) {
    g_print("\nReceived signal %d, shutting down...\n", signal);
    if (g_streamer) {
        g_streamer->stop();
    }
}

int main(int argc, char *argv[]) {
    // Initialize GStreamer
    gst_init(&argc, &argv);
    
    if (argc < 3) {
        g_print("Usage: %s <rtsp-uri> <output-directory>\n", argv[0]);
        g_print("Example: %s rtsp://192.168.1.100:554/stream /var/www/html/dash\n", argv[0]);
        return 1;
    }
    
    std::string rtsp_uri = argv[1];
    std::string output_path = argv[2];
    
    // Set up signal handlers for graceful shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Create and initialize streamer
    RTSPDashStreamer streamer(rtsp_uri, output_path);
    g_streamer = &streamer;
    
    if (!streamer.initialize()) {
        g_printerr("Failed to initialize streamer\n");
        return 1;
    }
    
    // Start streaming
    if (!streamer.start()) {
        g_printerr("Failed to start streaming\n");
        return 1;
    }
    
    g_print("Streaming stopped\n");
    g_streamer = nullptr;
    
    return 0;
}