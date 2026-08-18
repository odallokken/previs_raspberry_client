// =============================================================================
//  previs-client — headless Pexip Infinity video client for Raspberry Pi
// -----------------------------------------------------------------------------
//
//  Reads config.yaml, connects to the configured VMR, and stays connected.
//  When the call drops it waits reconnect_delay_seconds then dials again.
//
//  Build requirements (Ubuntu 24.04 / Ubuntu 22.04 on ARM64):
//    - Pexip Pulse SDK runtime (the 'pexninja' .deb) plus the pexpulse headers,
//      which scripts/install.sh takes from the public doppler repository
//      because the runtime package ships none
//    - libyaml-cpp-dev
//    - libglfw3-dev, libgl1-mesa-dev  (Pulse needs OpenGL headers at link time)
//
//  The application is designed to be run as a systemd service via
//  systemd/previs-client.service and managed by scripts/install.sh.
// =============================================================================

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include <yaml-cpp/yaml.h>

#include <pexpulse/pulse.h>

// ---------------------------------------------------------------------------
//  Configuration
// ---------------------------------------------------------------------------

struct Config {
    std::string server;
    std::string vmr;
    std::string display_name  = "Raspberry Pi Client";
    std::string pin;
    int         reconnect_delay_seconds = 10;
};

static Config load_config(const char * path)
{
    Config cfg;
    try {
        YAML::Node doc = YAML::LoadFile(path);

        if (doc["server"])       cfg.server       = doc["server"].as<std::string>();
        if (doc["vmr"])          cfg.vmr          = doc["vmr"].as<std::string>();
        if (doc["display_name"]) cfg.display_name = doc["display_name"].as<std::string>();
        if (doc["pin"])          cfg.pin          = doc["pin"].as<std::string>();
        if (doc["reconnect_delay_seconds"])
            cfg.reconnect_delay_seconds = doc["reconnect_delay_seconds"].as<int>();
    } catch (const YAML::Exception & e) {
        std::fprintf(stderr, "[config] Failed to parse %s: %s\n", path, e.what());
        std::exit(1);
    }

    if (cfg.server.empty()) {
        std::fprintf(stderr, "[config] 'server' is required in %s\n", path);
        std::exit(1);
    }
    if (cfg.vmr.empty()) {
        std::fprintf(stderr, "[config] 'vmr' is required in %s\n", path);
        std::exit(1);
    }
    if (cfg.reconnect_delay_seconds < 5) cfg.reconnect_delay_seconds = 5;

    return cfg;
}

// ---------------------------------------------------------------------------
//  Application state
// ---------------------------------------------------------------------------

struct AppState {
    Pulse * pulse = nullptr;

    // Connection status written from Pulse callback threads.
    std::atomic<int> connection_status{PULSE_CONNECTION_STATUS_DISCONNECTED};

    // Async operation result — PULSE_SUCCESS means completed OK.
    std::atomic<int> last_async_error{PULSE_SUCCESS};

    // Signals the main loop that an async op has finished.
    std::mutex              async_mutex;
    std::condition_variable async_cv;
    std::atomic<bool>       async_done{false};

    // Set true by the SIGTERM/SIGINT handler so the loop exits cleanly.
    std::atomic<bool> shutdown_requested{false};
};

// ---------------------------------------------------------------------------
//  Signal handling
// ---------------------------------------------------------------------------

static AppState * g_app = nullptr;

static void signal_handler(int /*sig*/)
{
    if (g_app) g_app->shutdown_requested.store(true);
}

// ---------------------------------------------------------------------------
//  Pulse callbacks
// ---------------------------------------------------------------------------

static void on_conference_status(const PulseConferenceStatusInfo * info,
                                 void * user_context)
{
    auto * app = static_cast<AppState *>(user_context);
    int status = static_cast<int>(info->status);
    app->connection_status.store(status);

    const char * names[] = {
        "Disconnected", "Connecting", "Reconnecting",
        "Connected",    "Disconnecting"
    };
    const char * name = (status >= 0 && status < 5) ? names[status] : "Unknown";
    std::printf("[pulse] Conference status: %s\n", name);
    std::fflush(stdout);
}

static void on_async_result(const PulseError err, void * user_context)
{
    auto * app = static_cast<AppState *>(user_context);
    app->last_async_error.store(static_cast<int>(err));

    if (err != PULSE_SUCCESS) {
        std::fprintf(stderr, "[pulse] Async operation failed: %s\n",
                     pulse_strerror(err));
    }

    {
        std::lock_guard<std::mutex> lock(app->async_mutex);
        app->async_done.store(true);
    }
    app->async_cv.notify_all();
}

static void on_progress(const PulseOperationProgressInfo * info, void * /*ctx*/)
{
    std::printf("[pulse] %3d%%  %s\n",
                static_cast<int>(info->progress * 100.0f),
                info->desc ? info->desc : "");
    std::fflush(stdout);
}

static void on_pulse_log(void * /*ctx*/, PulseDebugLevel level,
                         const char * category, int64_t /*wall_time_us*/,
                         int64_t /*elapsed_nano*/, unsigned int /*pid*/,
                         const char * /*file*/, const char * /*function*/,
                         int /*line*/, const char * /*object_debug_str*/,
                         const char * message)
{
    if (level > PULSE_LEVEL_WARNING) return;
    std::fprintf(stderr, "[pulse:%s] %s\n",
                 category ? category : "?", message ? message : "");
}

// ---------------------------------------------------------------------------
//  Pulse helpers
// ---------------------------------------------------------------------------

static void install_callbacks(AppState & app)
{
    PulseConferenceStatusCallbackConfig conf_cb{on_conference_status, &app};
    pulse_options_set_conference_state_callback(app.pulse, &conf_cb);
    pulse_options_set_application_user_agent_string(app.pulse, "previs-client/1.0");

    // Headless: disable Pulse's auto-spawned video windows.  Each of these must
    // be set to NULL before connecting, or Pulse spawns its own window.
    pulse_options_set_self_view_window_handle(app.pulse, nullptr);
    pulse_options_set_remote_video_window_handle(app.pulse, nullptr);
    pulse_options_set_presentation_video_window_handle(app.pulse, nullptr);
    pulse_options_set_preflight_video_window_handle(app.pulse, nullptr);
}

static void connect_default_devices(AppState & app)
{
    struct Binding {
        const char *        name;
        PulseMediaType      type;
        PulseMediaDirection direction;
    };
    const Binding bindings[] = {
        { "camera",      PULSE_MEDIA_VIDEO, PULSE_MEDIA_INPUT  },
        { "microphone",  PULSE_MEDIA_AUDIO, PULSE_MEDIA_INPUT  },
        { "speaker",     PULSE_MEDIA_AUDIO, PULSE_MEDIA_OUTPUT },
    };

    for (const auto & b : bindings) {
        PulseError err = pulse_device_session_connect_system_default(
            app.pulse, PULSE_MEDIA_CONTENT_MAIN, b.type, b.direction);
        if (err != PULSE_SUCCESS) {
            std::fprintf(stderr, "[pulse] Failed to attach default %s: %s\n",
                         b.name, pulse_strerror(err));
        }
    }
}

// Wait (with timeout) for the async operation to complete.
// Returns true if the operation completed (check last_async_error for the result).
static bool wait_for_async(AppState & app, int timeout_seconds = 60)
{
    std::unique_lock<std::mutex> lock(app.async_mutex);
    return app.async_cv.wait_for(
        lock,
        std::chrono::seconds(timeout_seconds),
        [&app]() { return app.async_done.load(); });
}

// ---------------------------------------------------------------------------
//  One call attempt
// ---------------------------------------------------------------------------

static bool do_connect(AppState & app, const Config & cfg)
{
    PulseRestConnectionConfig rest{};
    rest.server_address  = cfg.server.c_str();
    rest.conference_name = cfg.vmr.c_str();
    rest.display_name    = cfg.display_name.c_str();
    rest.pin_code        = cfg.pin.empty() ? nullptr : cfg.pin.c_str();

    PulseAsyncOperationResultCallbackConfig result_cb{on_async_result, &app};
    PulseOperationProgressCallbackConfig    progress_cb{on_progress, &app};

    std::printf("[client] Connecting to %s / %s as \"%s\" ...\n",
                cfg.server.c_str(), cfg.vmr.c_str(), cfg.display_name.c_str());
    std::fflush(stdout);

    app.async_done.store(false);
    PulseError err = pulse_connect_with_rest_async(app.pulse, &rest,
                                                   &result_cb, &progress_cb);
    if (err != PULSE_SUCCESS) {
        std::fprintf(stderr, "[client] pulse_connect_with_rest_async: %s\n",
                     pulse_strerror(err));
        return false;
    }

    if (!wait_for_async(app)) {
        std::fprintf(stderr, "[client] Connect timed out.\n");
        return false;
    }
    return app.last_async_error.load() == PULSE_SUCCESS;
}

static void do_disconnect(AppState & app)
{
    PulseAsyncOperationResultCallbackConfig result_cb{on_async_result, &app};
    PulseOperationProgressCallbackConfig    progress_cb{on_progress, &app};

    app.async_done.store(false);
    PulseError err = pulse_disconnect_async(app.pulse, &result_cb, &progress_cb);
    if (err != PULSE_SUCCESS) {
        std::fprintf(stderr, "[client] pulse_disconnect_async: %s\n",
                     pulse_strerror(err));
        return;
    }
    wait_for_async(app, 30);
}

// ---------------------------------------------------------------------------
//  Main
// ---------------------------------------------------------------------------

int main(int argc, char * argv[])
{
    const char * config_path = "/etc/previs-client/config.yaml";
    if (argc > 1) config_path = argv[1];

    std::printf("[client] Loading config from %s\n", config_path);
    Config cfg = load_config(config_path);

    // Install signal handlers for graceful shutdown.
    AppState app;
    g_app = &app;
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT,  signal_handler);

    // Global Pulse logging hook (optional — keeps the console readable).
    pulse_global_logger_callback(on_pulse_log, nullptr);

    // Create and configure the Pulse instance.
    app.pulse = pulse_new();
    if (!app.pulse) {
        std::fprintf(stderr, "[client] pulse_new() failed\n");
        return 1;
    }

    install_callbacks(app);
    connect_default_devices(app);

    // -----------------------------------------------------------------------
    //  Main loop: connect → stay connected → reconnect on drop
    // -----------------------------------------------------------------------
    while (!app.shutdown_requested.load()) {
        bool connected = do_connect(app, cfg);

        if (!connected) {
            std::fprintf(stderr, "[client] Connect failed. Retrying in %ds ...\n",
                         cfg.reconnect_delay_seconds);
        } else {
            std::printf("[client] In call. Waiting for disconnect...\n");
            std::fflush(stdout);

            // Wait until we are no longer in a connected state (call dropped
            // or remote side hung up) or a shutdown is requested.
            while (!app.shutdown_requested.load()) {
                int status = app.connection_status.load();
                if (status == PULSE_CONNECTION_STATUS_DISCONNECTED) break;
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }

        if (app.shutdown_requested.load()) break;

        std::printf("[client] Waiting %ds before reconnect...\n",
                    cfg.reconnect_delay_seconds);
        std::fflush(stdout);

        // Interruptible sleep so SIGTERM is handled promptly.
        for (int i = 0; i < cfg.reconnect_delay_seconds * 10; ++i) {
            if (app.shutdown_requested.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    std::printf("[client] Shutting down...\n");
    std::fflush(stdout);

    int status = app.connection_status.load();
    if (status != PULSE_CONNECTION_STATUS_DISCONNECTED) {
        do_disconnect(app);
    }

    pulse_options_set_conference_state_callback(app.pulse, nullptr);
    pulse_free(app.pulse);
    app.pulse = nullptr;

    std::printf("[client] Done.\n");
    return 0;
}
