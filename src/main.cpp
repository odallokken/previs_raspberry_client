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
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include <sys/stat.h>

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
//  Sound server
// ---------------------------------------------------------------------------

// Where the PulseAudio socket should be, according to the environment.
// Returns an empty string when it cannot be derived.
static std::string pulse_socket_path()
{
    const char * server = std::getenv("PULSE_SERVER");
    if (server && std::strncmp(server, "unix:", 5) == 0) return server + 5;
    if (server && server[0] == '/')                      return server;

    const char * runtime = std::getenv("PULSE_RUNTIME_PATH");
    if (runtime && runtime[0]) return std::string(runtime) + "/native";

    const char * xdg = std::getenv("XDG_RUNTIME_DIR");
    if (xdg && xdg[0]) return std::string(xdg) + "/pulse/native";

    return {};
}

// The Pulse SDK opens its audio backend while the instance is being created,
// so the sound server has to be up first: without it the SDK falls back to raw
// ALSA, floods the journal with "Failed to connect"/"Got no caps from device"
// and can die with SIGSEGV.  systemd orders us after previs-pulseaudio.service,
// but wait here too so a manually started client behaves the same.
static bool wait_for_sound_server(const AppState & app, int timeout_seconds = 30)
{
    const std::string path = pulse_socket_path();
    if (path.empty()) {
        std::fprintf(stderr,
                     "[client] PULSE_SERVER is not set — the Pulse SDK will look"
                     " for a sound server on its own.\n");
        return false;
    }

    struct stat st{};
    for (int waited = 0; waited < timeout_seconds * 10; ++waited) {
        if (stat(path.c_str(), &st) == 0) {
            if (waited > 0)
                std::printf("[client] Sound server socket appeared after %.1fs\n",
                            waited / 10.0);
            return true;
        }
        if (app.shutdown_requested.load()) return false;
        if (waited == 0)
            std::printf("[client] Waiting for the sound server at %s ...\n",
                        path.c_str());
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::fprintf(stderr,
                 "[client] No sound server at %s after %ds.\n"
                 "[client] Check it with:  systemctl status previs-pulseaudio\n",
                 path.c_str(), timeout_seconds);
    return false;
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

// Report how many devices the SDK found, and log their names.  Returns the
// device count, or -1 when the list could not be read at all.
static int list_devices(AppState & app, const char * label,
                        PulseMediaType type, PulseMediaDirection direction)
{
    PulseDeviceIterator * it = nullptr;
    PulseError err = pulse_device_iterator_new(app.pulse, type, direction, &it);
    if (err != PULSE_SUCCESS || !it) {
        std::fprintf(stderr, "[client] Could not list %s devices: %s\n",
                     label, pulse_strerror(err));
        if (it) pulse_device_iterator_free(it);
        return -1;
    }

    int count = pulse_device_iterator_item_count(it);
    for (const PulseDevice * dev = pulse_device_iterator_first(it);
         dev != nullptr;
         dev = pulse_device_iterator_next(it)) {
        const char * name = pulse_device_get_name(dev);
        std::printf("[client]   %s: %s%s\n", label, name ? name : "(unnamed)",
                    pulse_device_is_system_default(dev) ? "  (default)" : "");
    }
    std::fflush(stdout);

    pulse_device_iterator_free(it);
    return count;
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

    int audio_failures = 0;
    for (const auto & b : bindings) {
        // Only bind a device class the SDK actually enumerated.  Asking it for
        // the "system default" of an empty class makes the media backend walk
        // a device it never opened, which has been seen to abort the process
        // with SIGSEGV on machines without a working sound server.
        int count = list_devices(app, b.name, b.type, b.direction);
        if (count <= 0) {
            std::fprintf(stderr, "[client] No %s device available — skipping.\n",
                         b.name);
            if (b.type == PULSE_MEDIA_AUDIO) ++audio_failures;
            continue;
        }

        PulseError err = pulse_device_session_connect_system_default(
            app.pulse, PULSE_MEDIA_CONTENT_MAIN, b.type, b.direction);
        if (err != PULSE_SUCCESS) {
            std::fprintf(stderr, "[client] Failed to attach default %s: %s\n",
                         b.name, pulse_strerror(err));
            if (b.type == PULSE_MEDIA_AUDIO) ++audio_failures;
        }
    }

    // The Pulse SDK talks to a PulseAudio server.  When the client runs as a
    // headless system user there is no automatically started daemon, so
    // previs-pulseaudio.service must be running and PULSE_SERVER must point at
    // its socket — otherwise the SDK logs "Failed to connect: Connection
    // refused" and falls back to raw ALSA, which normally cannot open the
    // devices either.
    if (audio_failures > 0) {
        const char * server = std::getenv("PULSE_SERVER");
        std::fprintf(stderr,
                     "[client] No usable audio device (PULSE_SERVER=%s).\n"
                     "[client] Check that the sound server is running:\n"
                     "[client]   systemctl status previs-pulseaudio\n",
                     server ? server : "<unset>");
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
    // systemd captures stdout through a pipe, which makes it fully buffered:
    // without this the client's own messages only reach the journal in large
    // chunks (or not at all if the process is killed), leaving just the Pulse
    // SDK's stderr output visible.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

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

    // Must happen before pulse_new(): the SDK initialises its audio backend
    // while creating the instance.
    wait_for_sound_server(app);

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
