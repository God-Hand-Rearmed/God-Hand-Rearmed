// Keeping the display awake on Linux (issue #12): runtime/keep_awake.h says what blanks a Linux screen and how each is
// stopped. Windows' counterpart is host_window_win32.cpp (SetThreadExecutionState).
#if defined(__linux__) && !defined(__ANDROID__)
#include "runtime/keep_awake.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

namespace ps2x::keepawake
{
    const char *name(Inhibitor inhibitor)
    {
        switch (inhibitor)
        {
        case Inhibitor::FreedesktopScreenSaver:
            return "org.freedesktop.ScreenSaver";
        case Inhibitor::GnomeSessionManager:
            return "org.gnome.SessionManager";
        case Inhibitor::None:
            break;
        }
        return "none";
    }

    std::vector<Call> calls()
    {
        return {
            {Inhibitor::FreedesktopScreenSaver, "org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver",
             "org.freedesktop.ScreenSaver", "Inhibit"},
            {Inhibitor::GnomeSessionManager, "org.gnome.SessionManager", "/org/gnome/SessionManager",
             "org.gnome.SessionManager", "Inhibit"},
        };
    }

    Result inhibitIdle(Bus &bus, const char *app, const char *reason)
    {
        Result result;
        for (const Call &call : calls())
        {
            uint32_t cookie = 0;
            std::string error;
            if (bus.call(call, app, reason, cookie, error))
            {
                result.by = call.inhibitor;
                result.cookie = cookie;
                return result;
            }
            if (!result.error.empty())
                result.error += "; ";
            result.error += std::string(name(call.inhibitor)) + ": " + (error.empty() ? "no reply" : error);
        }
        return result;
    }

    namespace
    {
        // libdbus-1's C API, the few calls used (dbus/dbus-*.h; the ABI has been stable since 1.0).
        struct DBusConnection;
        struct DBusMessage;
        struct DBusError // dbus-errors.h
        {
            const char *name;
            const char *message;
            unsigned int dummy1 : 1, dummy2 : 1, dummy3 : 1, dummy4 : 1, dummy5 : 1;
            void *padding1;
        };
        using dbus_bool_t = uint32_t;
        constexpr int kBusSession = 0;          // DBUS_BUS_SESSION
        constexpr int kTypeInvalid = 0;         // DBUS_TYPE_INVALID
        constexpr int kTypeString = 's';        // DBUS_TYPE_STRING
        constexpr int kTypeUint32 = 'u';        // DBUS_TYPE_UINT32
        constexpr int kReplyTimeoutMs = 3000;

        struct LibDbus
        {
            void (*error_init)(DBusError *) = nullptr;
            void (*error_free)(DBusError *) = nullptr;
            dbus_bool_t (*error_is_set)(const DBusError *) = nullptr;
            DBusConnection *(*bus_get_private)(int, DBusError *) = nullptr;
            void (*set_exit_on_disconnect)(DBusConnection *, dbus_bool_t) = nullptr;
            DBusMessage *(*new_method_call)(const char *, const char *, const char *, const char *) = nullptr;
            dbus_bool_t (*append_args)(DBusMessage *, int, ...) = nullptr;
            DBusMessage *(*send_with_reply_and_block)(DBusConnection *, DBusMessage *, int, DBusError *) = nullptr;
            dbus_bool_t (*get_args)(DBusMessage *, DBusError *, int, ...) = nullptr;
            void (*message_unref)(DBusMessage *) = nullptr;

            bool load(std::string &error)
            {
                void *lib = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
                if (!lib)
                {
                    const char *why = dlerror();
                    error = std::string("libdbus-1.so.3 did not load (") + (why ? why : "?") + ")";
                    return false;
                }
                bool all = true;
                auto get = [&](auto &fn, const char *symbol)
                {
                    fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, symbol));
                    all = all && fn != nullptr;
                };
                get(error_init, "dbus_error_init");
                get(error_free, "dbus_error_free");
                get(error_is_set, "dbus_error_is_set");
                get(bus_get_private, "dbus_bus_get_private");
                get(set_exit_on_disconnect, "dbus_connection_set_exit_on_disconnect");
                get(new_method_call, "dbus_message_new_method_call");
                get(append_args, "dbus_message_append_args");
                get(send_with_reply_and_block, "dbus_connection_send_with_reply_and_block");
                get(get_args, "dbus_message_get_args");
                get(message_unref, "dbus_message_unref");
                if (!all)
                    error = "libdbus-1.so.3 lacks a function it needs";
                return all;
            }
        };

        std::string describe(const DBusError &e)
        {
            return std::string(e.name ? e.name : "?") + (e.message ? std::string(" (") + e.message + ")" : "");
        }

        class SessionBus final : public Bus
        {
        public:
            SessionBus(const LibDbus &dbus, DBusConnection *connection) : m_dbus(dbus), m_connection(connection) {}

            bool call(const Call &call, const char *app, const char *reason, uint32_t &cookie,
                      std::string &error) override
            {
                DBusMessage *message = m_dbus.new_method_call(call.service, call.path, call.interface, call.method);
                if (!message)
                {
                    error = "out of memory";
                    return false;
                }
                bool built = false;
                if (call.inhibitor == Inhibitor::GnomeSessionManager)
                {
                    const uint32_t xid = 0, flags = kGnomeInhibitIdle;
                    built = m_dbus.append_args(message, kTypeString, &app, kTypeUint32, &xid, kTypeString, &reason,
                                               kTypeUint32, &flags, kTypeInvalid);
                }
                else
                    built = m_dbus.append_args(message, kTypeString, &app, kTypeString, &reason, kTypeInvalid);
                if (!built)
                {
                    m_dbus.message_unref(message);
                    error = "out of memory";
                    return false;
                }
                DBusError e;
                m_dbus.error_init(&e);
                DBusMessage *reply = m_dbus.send_with_reply_and_block(m_connection, message, kReplyTimeoutMs, &e);
                m_dbus.message_unref(message);
                if (!reply)
                {
                    error = m_dbus.error_is_set(&e) ? describe(e) : "no reply";
                    m_dbus.error_free(&e);
                    return false;
                }
                uint32_t value = 0;
                const bool read = m_dbus.get_args(reply, &e, kTypeUint32, &value, kTypeInvalid);
                m_dbus.message_unref(reply);
                if (!read)
                {
                    error = "the reply is not a cookie (" + (m_dbus.error_is_set(&e) ? describe(e) : "?") + ")";
                    m_dbus.error_free(&e);
                    return false;
                }
                cookie = value;
                return true;
            }

        private:
            const LibDbus &m_dbus;
            DBusConnection *m_connection;
        };
    }

    Bus *openSessionBus(std::string &error)
    {
        static LibDbus dbus;
        static std::string loadError;
        static const bool loaded = dbus.load(loadError);
        if (!loaded)
        {
            error = loadError;
            return nullptr;
        }
        const char *address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
        if (!address || !*address)
        {
            // libdbus's own default when the address is unset ($XDG_RUNTIME_DIR/bus, systemd's user bus), which it
            // takes only when that is a socket the user owns (_dbus_lookup_user_bus); anything else there and it would
            // try X11 autolaunch (dbus-launch: the X display's bus if it has one, else one of its own that no desktop
            // hears). So exactly libdbus's test, or no bus.
            const char *runtime = std::getenv("XDG_RUNTIME_DIR");
            struct stat st{};
            if (!runtime || !*runtime || ::stat((std::string(runtime) + "/bus").c_str(), &st) != 0 ||
                !S_ISSOCK(st.st_mode) || st.st_uid != ::getuid())
            {
                error = "no session bus ($DBUS_SESSION_BUS_ADDRESS unset, no $XDG_RUNTIME_DIR/bus socket of ours)";
                return nullptr;
            }
        }
        DBusError e;
        dbus.error_init(&e);
        // A private connection: GHR's own, so no other library sharing the process's session connection (SDL's) can
        // close it, and its exit-on-disconnect is ours to turn off (keep_awake.h: never reached today). Untested: no
        // other library in the tests shares the process's bus connection.
        DBusConnection *connection = dbus.bus_get_private(kBusSession, &e);
        if (!connection)
        {
            error = "no session bus (" + (dbus.error_is_set(&e) ? describe(e) : std::string("?")) + ")";
            dbus.error_free(&e);
            return nullptr;
        }
        dbus.set_exit_on_disconnect(connection, 0u);
        return new SessionBus(dbus, connection); // kept, as the connection is: the inhibit lasts as long
    }

    namespace
    {
        bool startX11ResetOnce(int periodMs, std::string &error);
    }

    bool startX11Reset(int periodMs, std::string &error)
    {
        // Once per process: a second start would install the IO handler over itself (then calling itself forever).
        // Later calls give the first one's answer.
        static std::mutex mutex;
        static int state = 0; // 0 not tried, 1 running, 2 failed
        static std::string firstError;
        std::lock_guard<std::mutex> lock(mutex);
        if (state == 0)
        {
            state = startX11ResetOnce(periodMs, firstError) ? 1 : 2;
        }
        if (state == 2)
            error = firstError;
        return state == 1;
    }

    namespace
    {
    bool startX11ResetOnce(int periodMs, std::string &error)
    {
        struct Display;
        using OpenDisplay = Display *(*)(const char *);
        using DisplayCall = int (*)(Display *);
        void *lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
        if (!lib)
        {
            const char *why = dlerror();
            error = std::string("libX11.so.6 did not load (") + (why ? why : "?") + ")";
            return false;
        }
        const auto open = reinterpret_cast<OpenDisplay>(dlsym(lib, "XOpenDisplay"));
        const auto reset = reinterpret_cast<DisplayCall>(dlsym(lib, "XResetScreenSaver"));
        const auto flush = reinterpret_cast<DisplayCall>(dlsym(lib, "XFlush"));
        if (!open || !reset || !flush)
        {
            error = "libX11.so.6 lacks XOpenDisplay / XResetScreenSaver / XFlush";
            return false;
        }
        // A connection of our own, used by this thread alone (GLFW's and the renderer's stay theirs).
        Display *display = open(nullptr);
        if (!display)
        {
            error = "no X display ($DISPLAY)";
            return false;
        }
        // A lost connection (the X server or Xwayland gone): libX11's default ends the process from this thread. Its
        // per-connection exit handler (1.7+) lets the thread stop instead; the calls on a dead connection return at once.
        // Two steps, as libX11 runs them: the process-wide IO error handler first (its default prints "XIO: fatal IO
        // error" and exits), then the connection's exit handler. Ours returns for this connection only and hands every
        // other one (GLFW's, the renderer's) to the handler that was there before, which still ends the process.
        using ExitHandler = void (*)(Display *, void *);
        using SetExitHandler = void (*)(Display *, ExitHandler, void *);
        using IOHandler = int (*)(Display *);
        using SetIOHandler = IOHandler (*)(IOHandler);
        static std::atomic<bool> lost{false};
        static std::atomic<Display *> ours{nullptr};
        static std::atomic<IOHandler> previous{nullptr};
        const auto setExit = reinterpret_cast<SetExitHandler>(dlsym(lib, "XSetIOErrorExitHandler"));
        const auto setIO = reinterpret_cast<SetIOHandler>(dlsym(lib, "XSetIOErrorHandler"));
        if (setExit && setIO)
        {
            ours.store(display);
            setExit(display, [](Display *, void *) { lost.store(true); }, nullptr);
            // The handler there now, kept before ours goes in (an IO error on another thread meanwhile finds it). The
            // swap through nullptr leaves libX11's default (print and exit) in place for that instant: what any X
            // connection's loss did before, though a custom handler set earlier would miss that one instant.
            previous.store(setIO(nullptr));
            const IOHandler between = setIO([](Display *d) -> int
                  {
                      if (d == ours.load())
                      {
                          std::fprintf(stderr, "[window] the X11 screensaver reset's X connection is gone\n");
                          return 0; // on to the exit handler above: the thread stops
                      }
                      const IOHandler before = previous.load();
                      return before ? before(d) : 0;
                  });
            if (between) // libX11's default, or one another thread put in during the swap: chained either way
                previous.store(between);
        }
        reset(display);
        flush(display);
        try
        {
            std::thread([display, reset, flush, periodMs]
                        {
                            while (!lost.load())
                            {
                                std::this_thread::sleep_for(std::chrono::milliseconds(periodMs));
                                reset(display);
                                flush(display);
                            }
                            std::fprintf(stderr, "[window] X11 screensaver reset stopped: the X connection is gone\n");
                        })
                .detach();
        }
        catch (...)
        {
            error = "no thread for it (reset once only)";
            return false;
        }
        return true;
    }
    }

    namespace
    {
        // libwayland-client's C ABI (wayland-util.h, wayland-client-core.h; stable since 1.0, the constructor calls since
        // 1.4 and 1.10: wl_proxy_marshal_flags, 1.20, isn't needed). Protocol objects are made the way
        // wayland-scanner's generated inline functions make them; the two protocols' tables are written out below.
        struct WlProxy;
        struct WlInterface;
        struct WlMessage
        {
            const char *name;
            const char *signature;
            const WlInterface **types;
        };
        struct WlInterface
        {
            const char *name;
            int version;
            int method_count;
            const WlMessage *methods;
            int event_count;
            const WlMessage *events;
        };
        using WlHandler = void (*)(void);

        // Types for arguments that take no object (unused) and for get_popup (never sent).
        const WlInterface *gNoTypes[8] = {};

        // idle-inhibit-unstable-v1 (wayland-protocols), version 1.
        const WlMessage kInhibitorRequests[] = {{"destroy", "", gNoTypes}};
        const WlInterface gInhibitor = {"zwp_idle_inhibitor_v1", 1, 1, kInhibitorRequests, 0, nullptr};
        const WlInterface *gInhibitManagerTypes[2] = {&gInhibitor, nullptr}; // [1] wl_surface: libwayland's, at load
        const WlMessage kInhibitManagerRequests[] = {{"destroy", "", gNoTypes},
                                                     {"create_inhibitor", "no", gInhibitManagerTypes}};
        const WlInterface gInhibitManager = {"zwp_idle_inhibit_manager_v1", 1, 2, kInhibitManagerRequests, 0, nullptr};

        // wlr-layer-shell-unstable-v1 (wlr-protocols), bound at version 1: the requests and events version 1 has.
        const WlMessage kLayerSurfaceRequests[] = {{"set_size", "uu", gNoTypes},
                                                   {"set_anchor", "u", gNoTypes},
                                                   {"set_exclusive_zone", "i", gNoTypes},
                                                   {"set_margin", "iiii", gNoTypes},
                                                   {"set_keyboard_interactivity", "u", gNoTypes},
                                                   {"get_popup", "o", gNoTypes},
                                                   {"ack_configure", "u", gNoTypes},
                                                   {"destroy", "", gNoTypes}};
        const WlMessage kLayerSurfaceEvents[] = {{"configure", "uuu", gNoTypes}, {"closed", "", gNoTypes}};
        const WlInterface gLayerSurface = {"zwlr_layer_surface_v1", 1, 8, kLayerSurfaceRequests, 2, kLayerSurfaceEvents};
        // get_layer_surface(new_id, wl_surface, ?wl_output, uint layer, string namespace): [1] at load; [2] left
        // empty, as the output sent is always null (the compositor picks one).
        const WlInterface *gLayerShellTypes[5] = {&gLayerSurface, nullptr, nullptr, nullptr, nullptr};
        const WlMessage kLayerShellRequests[] = {{"get_layer_surface", "no?ous", gLayerShellTypes}};
        const WlInterface gLayerShell = {"zwlr_layer_shell_v1", 1, 1, kLayerShellRequests, 0, nullptr};

        // Request opcodes (the order in each protocol's XML).
        enum : uint32_t
        {
            kDisplayGetRegistry = 1,
            kRegistryBind = 0,
            kCompositorCreateSurface = 0,
            kCompositorCreateRegion = 1,
            kSurfaceDestroy = 0,
            kSurfaceAttach = 1,
            kSurfaceDamage = 2,
            kSurfaceSetInputRegion = 5,
            kSurfaceCommit = 6,
            kRegionDestroy = 0,
            kShmCreatePool = 0,
            kShmPoolCreateBuffer = 0,
            kShmPoolDestroy = 1,
            kInhibitorDestroy = 0,
            kInhibitManagerCreateInhibitor = 1,
            kLayerShellGetLayerSurface = 0,
            kLayerSurfaceSetSize = 0,
            kLayerSurfaceSetAnchor = 1,
            kLayerSurfaceAckConfigure = 6,
            kLayerSurfaceDestroy = 7,
        };
        constexpr uint32_t kLayerBackground = 0;         // zwlr_layer_shell_v1.layer.background
        constexpr uint32_t kAnchorTopLeft = 1u | 4u;     // zwlr_layer_surface_v1.anchor.top | .left
        constexpr uint32_t kShmFormatArgb8888 = 0;       // wl_shm.format.argb8888
        constexpr int kSurfaceRetryFirstMs = 1000, kSurfaceRetryMaxMs = 60000;

        struct LibWayland
        {
            WlProxy *(*connect_to_fd)(int) = nullptr; // a wl_display is a wl_proxy (wayland-client-core.h)
            void (*disconnect)(WlProxy *) = nullptr;
            int (*roundtrip)(WlProxy *) = nullptr;
            int (*dispatch)(WlProxy *) = nullptr;
            int (*get_error)(WlProxy *) = nullptr;
            void (*marshal)(WlProxy *, uint32_t, ...) = nullptr;
            WlProxy *(*construct)(WlProxy *, uint32_t, const WlInterface *, ...) = nullptr;
            WlProxy *(*constructVersioned)(WlProxy *, uint32_t, const WlInterface *, uint32_t, ...) = nullptr;
            int (*add_listener)(WlProxy *, WlHandler *, void *) = nullptr;
            void (*destroy)(WlProxy *) = nullptr;
            const WlInterface *registry = nullptr, *compositor = nullptr, *surface = nullptr, *region = nullptr,
                              *shm = nullptr, *shmPool = nullptr, *buffer = nullptr;

            bool load(std::string &error)
            {
                void *lib = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
                if (!lib)
                {
                    const char *why = dlerror();
                    error = std::string("libwayland-client.so.0 did not load (") + (why ? why : "?") + ")";
                    return false;
                }
                bool all = true;
                auto get = [&](auto &p, const char *symbol)
                {
                    p = reinterpret_cast<std::remove_reference_t<decltype(p)>>(dlsym(lib, symbol));
                    all = all && p != nullptr;
                };
                get(connect_to_fd, "wl_display_connect_to_fd");
                get(disconnect, "wl_display_disconnect");
                get(roundtrip, "wl_display_roundtrip");
                get(dispatch, "wl_display_dispatch");
                get(get_error, "wl_display_get_error");
                get(marshal, "wl_proxy_marshal");
                get(construct, "wl_proxy_marshal_constructor");
                get(constructVersioned, "wl_proxy_marshal_constructor_versioned");
                get(add_listener, "wl_proxy_add_listener");
                get(destroy, "wl_proxy_destroy");
                get(registry, "wl_registry_interface");
                get(compositor, "wl_compositor_interface");
                get(surface, "wl_surface_interface");
                get(region, "wl_region_interface");
                get(shm, "wl_shm_interface");
                get(shmPool, "wl_shm_pool_interface");
                get(buffer, "wl_buffer_interface");
                if (!all)
                {
                    error = "libwayland-client.so.0 lacks a function it needs (older than 1.10?)";
                    return false;
                }
                gInhibitManagerTypes[1] = surface;
                gLayerShellTypes[1] = surface;
                return true;
            }
        };

        // The connection, its globals, and the surface the inhibitor is on. Used by its own thread only (start, then
        // run), and never freed: the inhibit lasts as long as the connection.
        class WaylandInhibit
        {
        public:
            explicit WaylandInhibit(const LibWayland &wl) : m_wl(wl) {}

            bool start(std::string &error)
            {
                // The socket by hand, not wl_display_connect: that would take $WAYLAND_SOCKET first (an fd handed to the
                // process, not ours to use up) and unsetenv it from this thread while others may read the environment.
                const char *name = std::getenv("WAYLAND_DISPLAY"), *runtime = std::getenv("XDG_RUNTIME_DIR");
                std::string path = name; // set: startWaylandInhibitOnce checked
                if (path[0] != '/') // libwayland's rule: relative to $XDG_RUNTIME_DIR, else a path of its own
                {
                    if (!runtime || !*runtime)
                    {
                        error = "no Wayland compositor ($XDG_RUNTIME_DIR unset)";
                        return false;
                    }
                    path = std::string(runtime) + "/" + path;
                }
                sockaddr_un where{};
                where.sun_family = AF_UNIX;
                if (path.size() >= sizeof(where.sun_path))
                {
                    error = "no Wayland compositor (its socket path is too long)";
                    return false;
                }
                std::memcpy(where.sun_path, path.c_str(), path.size() + 1);
                const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
                if (fd < 0 || ::connect(fd, reinterpret_cast<sockaddr *>(&where), sizeof(where)) != 0)
                {
                    error = "no Wayland compositor at " + path + " (" + std::strerror(errno) + ")";
                    if (fd >= 0)
                        ::close(fd);
                    return false;
                }
                m_display = m_wl.connect_to_fd(fd); // libwayland owns fd now (on failure too: never closed twice)
                if (!m_display)
                {
                    error = std::string("libwayland didn't take the connection (") + std::strerror(errno) + ")";
                    return false;
                }
                if (!startConnected(error))
                {
                    m_wl.disconnect(m_display); // nothing of ours is left on it: the proxies made are only memory
                    m_display = nullptr;
                    return false;
                }
                return true;
            }

            // Dispatches the connection for the life of the process (the layer surface's configure and closed
            // events). Making a new surface when the compositor closes ours; stopping when the connection is gone.
            void run()
            {
                int waitMs = kSurfaceRetryFirstMs;
                // m_closed before each wait: a close read in makeSurface's own dispatching may leave no event behind to
                // wake the dispatch here (today the surface's delete_id replies do; this doesn't count on them).
                for (;;)
                {
                    if (!m_closed)
                    {
                        if (m_wl.dispatch(m_display) == -1)
                            break;
                        continue;
                    }
                    dropSurface();
                    std::fprintf(stderr, "[window] Wayland: the compositor closed the keep-awake surface; another in "
                                         "%d s\n", waitMs / 1000);
                    for (;;)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
                        std::string error;
                        if (makeSurface(error))
                        {
                            std::fprintf(stderr, "[window] Wayland idle inhibit held again (a new layer surface)\n");
                            waitMs = kSurfaceRetryFirstMs;
                            break;
                        }
                        if (m_wl.get_error(m_display) != 0)
                            break; // the connection is gone: the dispatch above says so and ends the loop
                        waitMs = waitMs * 2 > kSurfaceRetryMaxMs ? kSurfaceRetryMaxMs : waitMs * 2;
                        std::fprintf(stderr, "[window] Wayland: no keep-awake surface (%s); again in %d s\n",
                                     error.c_str(), waitMs / 1000);
                    }
                }
                std::fprintf(stderr, "[window] Wayland idle inhibit stopped: the compositor connection is gone (%s)\n",
                             std::strerror(m_wl.get_error(m_display)));
            }

        private:
            bool startConnected(std::string &error)
            {
                static WlHandler registryListener[2] = {reinterpret_cast<WlHandler>(&onGlobal),
                                                        reinterpret_cast<WlHandler>(&onGlobalRemove)};
                WlProxy *registry = m_wl.construct(m_display, kDisplayGetRegistry, m_wl.registry, nullptr);
                if (!registry || m_wl.add_listener(registry, registryListener, this) != 0 ||
                    m_wl.roundtrip(m_display) == -1)
                {
                    error = "the Wayland registry didn't answer (" + lastError() + ")";
                    return false;
                }
                std::string missing;
                for (const auto &[proxy, what] : {std::pair<WlProxy *, const char *>{m_compositor, "wl_compositor"},
                                                  {m_shm, "wl_shm"},
                                                  {m_manager, "zwp_idle_inhibit_manager_v1"},
                                                  {m_layerShell, "zwlr_layer_shell_v1"}})
                    if (!proxy)
                        missing += std::string(missing.empty() ? "" : ", ") + what;
                if (!missing.empty())
                {
                    error = "the compositor lacks " + missing;
                    return false;
                }
                if (!makeBuffer(error))
                    return false;
                if (makeSurface(error))
                    return true;
                if (m_wl.get_error(m_display) != 0)
                    return false;
                // The connection lives, the surface was closed (its output gone as it was made): run() makes another,
                // as it does for a later close.
                std::fprintf(stderr, "[window] Wayland: no keep-awake surface yet (%s); trying again\n", error.c_str());
                m_closed = true;
                return true;
            }

            // One transparent ARGB pixel in shared memory, attached to every surface made.
            bool makeBuffer(std::string &error)
            {
                const int fd = ::memfd_create("ghr-keep-awake", MFD_CLOEXEC);
                if (fd < 0 || ::ftruncate(fd, 4) != 0) // a new memfd reads as zeros: alpha 0
                {
                    error = std::string("no shared memory for the surface (") + std::strerror(errno) + ")";
                    if (fd >= 0)
                        ::close(fd);
                    return false;
                }
                WlProxy *pool = m_wl.construct(m_shm, kShmCreatePool, m_wl.shmPool, nullptr, fd, 4);
                ::close(fd); // libwayland sent a copy of it
                if (!pool)
                {
                    error = "out of memory";
                    return false;
                }
                m_buffer = m_wl.construct(pool, kShmPoolCreateBuffer, m_wl.buffer, nullptr, 0, 1, 1, 4,
                                          kShmFormatArgb8888);
                m_wl.marshal(pool, kShmPoolDestroy); // the buffer keeps the memory
                m_wl.destroy(pool);
                if (!m_buffer)
                {
                    error = "out of memory";
                    return false;
                }
                return true;
            }

            // The layer surface, mapped (its first configure acked, the pixel attached), then the inhibitor on it.
            // In that order: sway weighs an inhibitor when it is made (and again only after a transaction or a lock),
            // and an unmapped layer surface doesn't count; on an idle session nothing else would make it look again.
            bool makeSurface(std::string &error)
            {
                static WlHandler layerListener[2] = {reinterpret_cast<WlHandler>(&onConfigure),
                                                     reinterpret_cast<WlHandler>(&onClosed)};
                m_configured = m_closed = false;
                m_surface = m_wl.construct(m_compositor, kCompositorCreateSurface, m_wl.surface, nullptr);
                WlProxy *region = m_wl.construct(m_compositor, kCompositorCreateRegion, m_wl.region, nullptr);
                if (!m_surface || !region)
                {
                    error = "out of memory";
                    return false;
                }
                m_wl.marshal(m_surface, kSurfaceSetInputRegion, region); // empty: clicks go through it
                m_wl.marshal(region, kRegionDestroy);
                m_wl.destroy(region);
                m_layer = m_wl.construct(m_layerShell, kLayerShellGetLayerSurface, &gLayerSurface, nullptr, m_surface,
                                         nullptr, kLayerBackground, kWaylandNamespace);
                if (!m_layer || m_wl.add_listener(m_layer, layerListener, this) != 0)
                {
                    error = "out of memory";
                    return false;
                }
                m_wl.marshal(m_layer, kLayerSurfaceSetSize, 1u, 1u);
                m_wl.marshal(m_layer, kLayerSurfaceSetAnchor, kAnchorTopLeft);
                m_wl.marshal(m_surface, kSurfaceCommit); // no buffer yet: asks for the first configure
                while (!m_configured && !m_closed)
                    if (m_wl.dispatch(m_display) == -1)
                    {
                        error = "the compositor connection is gone (" + lastError() + ")";
                        return false;
                    }
                if (m_closed)
                {
                    error = "the compositor closed the layer surface at once (no output?)";
                    dropSurface();
                    return false;
                }
                m_wl.marshal(m_surface, kSurfaceAttach, m_buffer, 0, 0);
                m_wl.marshal(m_surface, kSurfaceDamage, 0, 0, 1, 1);
                m_wl.marshal(m_surface, kSurfaceCommit);
                m_mapped = true;
#ifdef PS2X_KEEP_AWAKE_TEST_PAUSE_AFTER_MAP // keep_awake_test_pause only: a window for the output to go (the test's)
                if (m_pauseOnce)
                {
                    m_pauseOnce = false;
                    m_wl.roundtrip(m_display); // the map reaches sway before the output goes
                    std::fprintf(stderr, "test: paused after the map\n");
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    m_drainOnce = true;
                }
#endif
                m_inhibitor = m_wl.construct(m_manager, kInhibitManagerCreateInhibitor, &gInhibitor, nullptr, m_surface);
                if (!m_inhibitor || m_wl.roundtrip(m_display) == -1)
                {
                    error = m_inhibitor ? "the compositor refused it (" + lastError() + ")" : "out of memory";
                    return false;
                }
#ifdef PS2X_KEEP_AWAKE_TEST_PAUSE_AFTER_MAP
                if (m_drainOnce) // every event of the output's going read here: none left to wake run()'s dispatch
                {
                    m_drainOnce = false;
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    m_wl.roundtrip(m_display);
                }
#endif
                if (m_closed) // closed meanwhile (its output gone between the configure and now): not held
                {
                    error = "the compositor closed the layer surface as it was shown";
                    dropSurface();
                    return false;
                }
                return true;
            }

            void dropSurface()
            {
                if (m_inhibitor)
                {
                    m_wl.marshal(m_inhibitor, kInhibitorDestroy);
                    m_wl.destroy(m_inhibitor);
                }
                if (m_layer)
                {
                    m_wl.marshal(m_layer, kLayerSurfaceDestroy);
                    m_wl.destroy(m_layer);
                }
                if (m_surface)
                {
                    m_wl.marshal(m_surface, kSurfaceDestroy);
                    m_wl.destroy(m_surface);
                }
                m_inhibitor = m_layer = m_surface = nullptr;
                m_mapped = false;
            }

            std::string lastError() const
            {
                const int code = m_wl.get_error(m_display);
                return code ? std::strerror(code) : "no error";
            }

            WlProxy *bind(WlProxy *registry, uint32_t name, const WlInterface *interface)
            {
                const uint32_t version = 1; // every request used is in version 1 of each
                return m_wl.constructVersioned(registry, kRegistryBind, interface, version, name, interface->name,
                                               version, nullptr);
            }

            static void onGlobal(void *data, WlProxy *registry, uint32_t name, const char *interface, uint32_t)
            {
                auto *self = static_cast<WaylandInhibit *>(data);
                auto take = [&](WlProxy *&slot, const WlInterface *what)
                {
                    if (!slot && std::strcmp(interface, what->name) == 0)
                        slot = self->bind(registry, name, what);
                };
                take(self->m_compositor, self->m_wl.compositor);
                take(self->m_shm, self->m_wl.shm);
                take(self->m_manager, &gInhibitManager);
                take(self->m_layerShell, &gLayerShell);
            }
            static void onGlobalRemove(void *, WlProxy *, uint32_t) {}

            static void onConfigure(void *data, WlProxy *layer, uint32_t serial, uint32_t, uint32_t)
            {
                auto *self = static_cast<WaylandInhibit *>(data);
                self->m_wl.marshal(layer, kLayerSurfaceAckConfigure, serial);
                if (self->m_mapped) // a later configure (output change): the same pixel, committed again
                    self->m_wl.marshal(self->m_surface, kSurfaceCommit);
                self->m_configured = true;
            }
            static void onClosed(void *data, WlProxy *) { static_cast<WaylandInhibit *>(data)->m_closed = true; }

            const LibWayland &m_wl;
            WlProxy *m_display = nullptr, *m_compositor = nullptr, *m_shm = nullptr, *m_manager = nullptr,
                    *m_layerShell = nullptr, *m_buffer = nullptr, *m_surface = nullptr, *m_layer = nullptr,
                    *m_inhibitor = nullptr;
            bool m_configured = false, m_closed = false, m_mapped = false;
#ifdef PS2X_KEEP_AWAKE_TEST_PAUSE_AFTER_MAP
            bool m_pauseOnce = true, m_drainOnce = false;
#endif
        };

        bool startWaylandInhibitOnce(std::string &error)
        {
            const char *name = std::getenv("WAYLAND_DISPLAY");
            if (!name || !*name)
            {
                // Unset: libwayland's "wayland-0" default isn't tried, never a compositor the session didn't name.
                error = "no Wayland compositor ($WAYLAND_DISPLAY unset)";
                return false;
            }
            static LibWayland wl;
            if (!wl.load(error))
                return false;
            auto *inhibit = new WaylandInhibit(wl); // never freed: see WaylandInhibit
            if (!inhibit->start(error))
            {
                delete inhibit;
                return false;
            }
            try
            {
                std::thread([inhibit]
                            {
                                try // nothing may leave this thread: an exception here would std::terminate the game
                                {
                                    inhibit->run();
                                }
                                catch (...)
                                {
                                    std::fprintf(stderr, "[window] Wayland idle inhibit stopped (out of memory)\n");
                                }
                            })
                    .detach();
            }
            catch (...)
            {
                // Held, undispatched: it lasts until the compositor's buffer for us fills and it drops the connection.
                error = "no thread to dispatch it";
                return false;
            }
            return true;
        }
    }

    bool startWaylandInhibit(std::string &error)
    {
        // Once per process, as startX11Reset: a second inhibitor would be harmless but a second connection and thread.
        static std::mutex mutex;
        static int state = 0; // 0 not tried, 1 held, 2 failed
        static std::string firstError;
        std::lock_guard<std::mutex> lock(mutex);
        if (state == 0)
            state = startWaylandInhibitOnce(firstError) ? 1 : 2;
        if (state == 2)
            error = firstError;
        return state == 1;
    }
}

void ps2xHostKeepDisplayAwake() noexcept
{
    static std::atomic<bool> done{false};
    if (done.exchange(true))
        return;
    try
    {
        // The Wayland inhibit on a thread of its own: a compositor that never answers holds only it.
        std::thread([]
                    {
                        try
                        {
                            std::string error;
                            if (ps2x::keepawake::startWaylandInhibit(error))
                                std::fprintf(stderr, "[window] keeping the display awake: Wayland idle inhibit (a "
                                                     "layer surface of its own)\n");
                            else
                                std::fprintf(stderr, "[window] no Wayland idle inhibit: %s\n", error.c_str());
                        }
                        catch (...)
                        {
                            std::fprintf(stderr, "[window] the Wayland idle inhibit stopped (out of memory)\n");
                        }
                    })
            .detach();
        std::thread([]
                    {
                      try // nothing may leave this thread: an exception here would std::terminate the game
                      {
                        using namespace ps2x::keepawake;
                        // The X11 reset first: it is local and quick, where the session bus may hang without end (a
                        // bus that takes the connection and never answers: libdbus's connect has no timeout, only the
                        // calls after it have kReplyTimeoutMs). Then this thread may wait there; nothing else does.
                        std::string x11Error;
                        if (startX11Reset(kX11ResetPeriodMs, x11Error))
                            std::fprintf(stderr, "[window] X11 screensaver timer reset every %d s\n",
                                         kX11ResetPeriodMs / 1000);
                        else
                            std::fprintf(stderr, "[window] no X11 screensaver reset: %s\n", x11Error.c_str());
                        std::string error;
                        if (Bus *bus = openSessionBus(error))
                        {
                            const Result held = inhibitIdle(*bus, kApp, kReason);
                            if (held.by != Inhibitor::None)
                                std::fprintf(stderr, "[window] keeping the display awake: %s (cookie %u)\n",
                                             name(held.by), held.cookie);
                            else
                                std::fprintf(stderr, "[window] the desktop didn't take the screen-blank inhibit: %s\n",
                                             held.error.c_str());
                        }
                        else
                            std::fprintf(stderr, "[window] no screen-blank inhibit: %s\n", error.c_str());
                      }
                      catch (...)
                      {
                          std::fprintf(stderr, "[window] keeping the display awake stopped (out of memory)\n");
                      }
                    })
            .detach();
    }
    catch (...) // no thread to be had: the display may blank, the game goes on
    {
        std::fprintf(stderr, "[window] couldn't keep the display awake (no thread)\n");
    }
}
#endif
