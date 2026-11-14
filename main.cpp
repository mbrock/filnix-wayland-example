// A tiny Wayland client compiled with Fil-C.
//
// It asks the D-Bus session bus for a unique name, opens an xdg-shell window
// filled with a colour, and prints the keys you press, translated with
// xkbcommon. Press Escape or close the window to quit.

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <dbus/dbus.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "xdg-shell.h"

namespace {

struct App {
  wl_compositor *compositor = nullptr;
  wl_shm *shm = nullptr;
  wl_seat *seat = nullptr;
  wl_keyboard *keyboard = nullptr;
  xdg_wm_base *wm_base = nullptr;

  wl_surface *surface = nullptr;
  xdg_surface *xsurface = nullptr;
  xdg_toplevel *toplevel = nullptr;

  xkb_context *xkb = nullptr;
  xkb_keymap *keymap = nullptr;
  xkb_state *xkb_state = nullptr;

  int width = 480;
  int height = 320;
  bool running = true;
};

// Make a shared-memory buffer the compositor can read, and fill it with one
// colour. The buffer is released by the compositor when it no longer needs it.
wl_buffer *make_buffer(App &app) {
  int stride = app.width * 4;
  int size = stride * app.height;
  int fd = memfd_create("filnix-wayland-example", MFD_CLOEXEC);
  if (fd < 0 || ftruncate(fd, size) < 0) {
    std::perror("memfd");
    return nullptr;
  }
  void *map = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED) {
    std::perror("mmap");
    close(fd);
    return nullptr;
  }
  auto *pixels = static_cast<uint32_t *>(map);
  for (int i = 0; i < app.width * app.height; i++)
    pixels[i] = 0xff2a6f97; // opaque teal, ARGB
  munmap(map, size);

  wl_shm_pool *pool = wl_shm_create_pool(app.shm, fd, size);
  wl_buffer *buffer = wl_shm_pool_create_buffer(
      pool, 0, app.width, app.height, stride, WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy(pool);
  close(fd);
  return buffer;
}

void buffer_release(void *, wl_buffer *buffer) { wl_buffer_destroy(buffer); }
const wl_buffer_listener buffer_listener = {buffer_release};

void draw(App &app) {
  wl_buffer *buffer = make_buffer(app);
  if (!buffer)
    return;
  wl_buffer_add_listener(buffer, &buffer_listener, nullptr);
  wl_surface_attach(app.surface, buffer, 0, 0);
  wl_surface_damage_buffer(app.surface, 0, 0, app.width, app.height);
  wl_surface_commit(app.surface);
}

// xdg-shell: the compositor configures the window, we acknowledge and draw.
void xsurface_configure(void *data, xdg_surface *xsurface, uint32_t serial) {
  auto &app = *static_cast<App *>(data);
  xdg_surface_ack_configure(xsurface, serial);
  draw(app);
  std::printf("wayland: window shown (%dx%d)\n", app.width, app.height);
}
const xdg_surface_listener xsurface_listener = {xsurface_configure};

void toplevel_configure(void *data, xdg_toplevel *, int32_t w, int32_t h,
                        wl_array *) {
  auto &app = *static_cast<App *>(data);
  if (w > 0 && h > 0) {
    app.width = w;
    app.height = h;
  }
}
void toplevel_close(void *data, xdg_toplevel *) {
  static_cast<App *>(data)->running = false;
}
void toplevel_bounds(void *, xdg_toplevel *, int32_t, int32_t) {}
void toplevel_caps(void *, xdg_toplevel *, wl_array *) {}
const xdg_toplevel_listener toplevel_listener = {
    toplevel_configure, toplevel_close, toplevel_bounds, toplevel_caps};

void wm_ping(void *, xdg_wm_base *base, uint32_t serial) {
  xdg_wm_base_pong(base, serial);
}
const xdg_wm_base_listener wm_listener = {wm_ping};

// Keyboard: the compositor sends a keymap, and xkbcommon turns key codes
// into key names.
void kb_keymap(void *data, wl_keyboard *, uint32_t format, int32_t fd,
               uint32_t size) {
  auto &app = *static_cast<App *>(data);
  if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
    close(fd);
    return;
  }
  void *map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (map == MAP_FAILED)
    return;
  xkb_keymap_unref(app.keymap);
  xkb_state_unref(app.xkb_state);
  app.keymap = xkb_keymap_new_from_string(app.xkb, static_cast<char *>(map),
                                          XKB_KEYMAP_FORMAT_TEXT_V1,
                                          XKB_KEYMAP_COMPILE_NO_FLAGS);
  munmap(map, size);
  app.xkb_state = app.keymap ? xkb_state_new(app.keymap) : nullptr;
}
void kb_enter(void *, wl_keyboard *, uint32_t, wl_surface *, wl_array *) {}
void kb_leave(void *, wl_keyboard *, uint32_t, wl_surface *) {}
void kb_key(void *data, wl_keyboard *, uint32_t, uint32_t, uint32_t key,
            uint32_t state) {
  auto &app = *static_cast<App *>(data);
  if (!app.xkb_state || state != WL_KEYBOARD_KEY_STATE_PRESSED)
    return;
  xkb_keysym_t sym = xkb_state_key_get_one_sym(app.xkb_state, key + 8);
  char name[64];
  xkb_keysym_get_name(sym, name, sizeof name);
  std::printf("key: %s\n", name);
  if (sym == XKB_KEY_Escape)
    app.running = false;
}
void kb_modifiers(void *data, wl_keyboard *, uint32_t, uint32_t depressed,
                  uint32_t latched, uint32_t locked, uint32_t group) {
  auto &app = *static_cast<App *>(data);
  if (app.xkb_state)
    xkb_state_update_mask(app.xkb_state, depressed, latched, locked, 0, 0,
                          group);
}
void kb_repeat(void *, wl_keyboard *, int32_t, int32_t) {}
const wl_keyboard_listener kb_listener = {kb_keymap, kb_enter,     kb_leave,
                                          kb_key,    kb_modifiers, kb_repeat};

void seat_caps(void *data, wl_seat *seat, uint32_t caps) {
  auto &app = *static_cast<App *>(data);
  bool has_kb = caps & WL_SEAT_CAPABILITY_KEYBOARD;
  if (has_kb && !app.keyboard) {
    app.keyboard = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(app.keyboard, &kb_listener, &app);
  } else if (!has_kb && app.keyboard) {
    wl_keyboard_release(app.keyboard);
    app.keyboard = nullptr;
  }
}
void seat_name(void *, wl_seat *, const char *) {}
const wl_seat_listener seat_listener = {seat_caps, seat_name};

void on_global(void *data, wl_registry *reg, uint32_t name, const char *iface,
               uint32_t version) {
  auto &app = *static_cast<App *>(data);
  if (!std::strcmp(iface, wl_compositor_interface.name)) {
    app.compositor = static_cast<wl_compositor *>(
        wl_registry_bind(reg, name, &wl_compositor_interface, 4));
  } else if (!std::strcmp(iface, wl_shm_interface.name)) {
    app.shm = static_cast<wl_shm *>(
        wl_registry_bind(reg, name, &wl_shm_interface, 1));
  } else if (!std::strcmp(iface, wl_seat_interface.name) && !app.seat) {
    app.seat = static_cast<wl_seat *>(wl_registry_bind(
        reg, name, &wl_seat_interface, version < 5 ? version : 5));
    wl_seat_add_listener(app.seat, &seat_listener, &app);
  } else if (!std::strcmp(iface, xdg_wm_base_interface.name)) {
    app.wm_base = static_cast<xdg_wm_base *>(
        wl_registry_bind(reg, name, &xdg_wm_base_interface, 1));
    xdg_wm_base_add_listener(app.wm_base, &wm_listener, &app);
  }
}
void on_global_remove(void *, wl_registry *, uint32_t) {}
const wl_registry_listener registry_listener = {on_global, on_global_remove};

void say_hello_to_dbus() {
  DBusError err;
  dbus_error_init(&err);
  DBusConnection *bus = dbus_bus_get(DBUS_BUS_SESSION, &err);
  if (bus) {
    std::printf("dbus: session bus, unique name %s\n",
                dbus_bus_get_unique_name(bus));
    dbus_connection_unref(bus);
  } else {
    std::printf("dbus: no session bus (%s)\n", err.message);
    dbus_error_free(&err);
  }
}

} // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IOLBF, 0); // print each line as it happens
  say_hello_to_dbus();

  App app;
  app.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

  wl_display *display = wl_display_connect(nullptr);
  if (!display) {
    std::fprintf(stderr,
                 "wayland: cannot connect; is WAYLAND_DISPLAY set and "
                 "XDG_RUNTIME_DIR the same as your desktop session's?\n");
    return 1;
  }
  wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &registry_listener, &app);
  wl_display_roundtrip(display);
  if (!app.compositor || !app.shm || !app.wm_base) {
    std::fprintf(stderr, "wayland: compositor lacks wl_shm or xdg_wm_base\n");
    return 1;
  }

  app.surface = wl_compositor_create_surface(app.compositor);
  app.xsurface = xdg_wm_base_get_xdg_surface(app.wm_base, app.surface);
  xdg_surface_add_listener(app.xsurface, &xsurface_listener, &app);
  app.toplevel = xdg_surface_get_toplevel(app.xsurface);
  xdg_toplevel_add_listener(app.toplevel, &toplevel_listener, &app);
  xdg_toplevel_set_title(app.toplevel, "Hello from Fil-C");
  xdg_toplevel_set_app_id(app.toplevel, "org.filnix.WaylandExample");
  wl_surface_commit(app.surface);

  while (app.running && wl_display_dispatch(display) != -1) {
  }

  xdg_toplevel_destroy(app.toplevel);
  xdg_surface_destroy(app.xsurface);
  wl_surface_destroy(app.surface);
  wl_display_disconnect(display);
  xkb_state_unref(app.xkb_state);
  xkb_keymap_unref(app.keymap);
  xkb_context_unref(app.xkb);
  return 0;
}
