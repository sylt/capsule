#include <errno.h>
#include <fcntl.h>
#include <libevdev/libevdev-uinput.h>
#include <linux/input.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define ERROR(fmt, ...) fprintf(stderr, "Error: " fmt "\n", ##__VA_ARGS__);
#define DEBUG(fmt, ...) \
  if (log_level >= LOG_LEVEL_DEBUG) \
  printf("Info: " fmt "\n", ##__VA_ARGS__)
#define ARRAY_SIZE(some_array) (sizeof(some_array) / sizeof((some_array)[0]))

static enum {
  LOG_LEVEL_ERROR,
  LOG_LEVEL_DEBUG,
} log_level = LOG_LEVEL_ERROR;

static const struct {
  const uint16_t code;  // If Caps Lock is pressed, try match with this key code
  const struct {
    uint16_t code;
    bool left_alt;
    bool right_alt;
    bool left_ctrl;
  } output;  // ... and if it matches, send this key combo
} action_table[] = {
    // Use Vim bindings for HJKL
    {.code = KEY_H, .output = {.code = KEY_LEFT}},
    {.code = KEY_J, .output = {.code = KEY_DOWN}},
    {.code = KEY_K, .output = {.code = KEY_UP}},
    {.code = KEY_L, .output = {.code = KEY_RIGHT}},

    // Remap N and P to produce PageUp and PageDown
    {.code = KEY_P, .output = {.code = KEY_PAGEUP}},
    {.code = KEY_N, .output = {.code = KEY_PAGEDOWN}},

    // Remap D to be Delete key
    {.code = KEY_D, .output = {.code = KEY_DELETE}},

    // Remap Y and O to produce { and } with Swedish keyboard layout
    {.code = KEY_Y, .output = {.code = KEY_7, .right_alt = true}},
    {.code = KEY_O, .output = {.code = KEY_0, .right_alt = true}},

    // Remap U and I to produce [ and ] with Swedish keyboard layout
    {.code = KEY_U, .output = {.code = KEY_8, .right_alt = true}},
    {.code = KEY_I, .output = {.code = KEY_9, .right_alt = true}},
};

static struct {
  bool caps_lock_pressed;

  bool key_pressed_while_caps_lock_pressed;
  bool action_table_activated[ARRAY_SIZE(action_table)];
} kbd_state;

static struct libevdev* open_device(const char* path)
{
  int event_fd = open(path, O_RDONLY | O_NONBLOCK);
  if (event_fd == -1) {
    ERROR("Couldn't open %s: %s", path, strerror(errno));
    return NULL;
  }

  struct libevdev* evdev;
  int rc = libevdev_new_from_fd(event_fd, &evdev);
  if (rc < 0) {
    ERROR("Couldn't initialize from fd (%s): %s", path, strerror(-rc));
    close(event_fd);
    return NULL;
  }

  return evdev;
}

bool is_keyboard_event_device(const struct libevdev* evdev, bool require_led)
{
  return evdev && libevdev_has_event_type(evdev, EV_KEY)
         && (!require_led || libevdev_has_event_type(evdev, EV_LED))
         && libevdev_has_event_code(evdev, EV_KEY, KEY_CAPSLOCK);
}

struct libevdev* find_keyboard_device(void)
{
  for (size_t i = 0; i <= 999; i++) {
    char path[sizeof("/dev/input/event999")];
    snprintf(path, sizeof(path), "/dev/input/event%zd", i);
    if (access(path, F_OK) != 0) {
      break;  // Most likely no more event devices
    }

    struct libevdev* evdev = open_device(path);
    if (!evdev) {
      continue;
    }

    const bool require_led = true;
    if (is_keyboard_event_device(evdev, require_led)) {
      DEBUG("Found candidate: %s", path);
      return evdev;
    }

    close(libevdev_get_fd(evdev));
    libevdev_free(evdev);
  }

  return NULL;
}

static void write_event_to_uinput(struct libevdev_uinput* uinput_dev,
                                  unsigned int type,
                                  unsigned int code,
                                  int value)
{
  DEBUG("    W Event: %s %s %d",
        libevdev_event_type_get_name(type),
        libevdev_event_code_get_name(type, code),
        value);
  libevdev_uinput_write_event(uinput_dev, type, code, value);
}

static void handle_event(struct libevdev_uinput* uinput_dev, struct input_event* ev)
{
  if (ev->type != EV_KEY) {
    goto forward_event;
  }

  if (ev->code == KEY_CAPSLOCK) {
    if (ev->value > 1) {  // Key repeat
      return;
    }
    else if (ev->value == 1) {
      kbd_state.caps_lock_pressed = true;
      kbd_state.key_pressed_while_caps_lock_pressed = false;
      return;
    }

    kbd_state.caps_lock_pressed = false;
    if (kbd_state.key_pressed_while_caps_lock_pressed) {
      return;
    }

    // We want to activate caps-lock, since no other key was pressed while it was held down
    write_event_to_uinput(uinput_dev, EV_KEY, KEY_CAPSLOCK, 1);
    write_event_to_uinput(uinput_dev, EV_KEY, EV_SYN, 0);
    // ... and then we simply forward the deactivation
  }
  else {
    for (size_t i = 0; i < ARRAY_SIZE(action_table); i++) {
      if (action_table[i].code != ev->code) {
        continue;
      }

      // From this line on, we have a match, but first handle some cases where we back off
      if (ev->value == 1 && !kbd_state.caps_lock_pressed) {
        goto forward_event;  // Key was pressed "normally", without caps lock held in
      }

      if (ev->value != 1 && !kbd_state.action_table_activated[i]) {
        goto forward_event;  // Key was pressed while caps lock wasn't held, so treat normally
      }

      // From here on, we know we should do something
      if (action_table[i].output.right_alt && ev->value <= 1) {
        write_event_to_uinput(uinput_dev, EV_KEY, KEY_RIGHTALT, ev->value);
      }
      if (action_table[i].output.left_ctrl && ev->value <= 1) {
        write_event_to_uinput(uinput_dev, EV_KEY, KEY_LEFTCTRL, ev->value);
      }
      write_event_to_uinput(uinput_dev, EV_KEY, action_table[i].output.code, ev->value);

      // Something was done, and that's worth book keeping
      if (ev->value <= 1) {
        const bool activated = (ev->value == 1 && kbd_state.caps_lock_pressed);
        kbd_state.action_table_activated[i] = activated;
        kbd_state.key_pressed_while_caps_lock_pressed |= activated;
      }

      return;
    }

    // No mapping found; if caps lock is pressed, just eat up the event
    if (kbd_state.caps_lock_pressed) {
      kbd_state.key_pressed_while_caps_lock_pressed |= ev->value == 1;
      return;
    }
  }

forward_event:
  write_event_to_uinput(uinput_dev, ev->type, ev->code, ev->value);
}

static bool is_killswitch_active(const struct libevdev* evdev)
{
  return libevdev_get_event_value(evdev, EV_KEY, KEY_LEFTCTRL) > 0
         && libevdev_get_event_value(evdev, EV_KEY, KEY_RIGHTCTRL) > 0;
}

static bool run_event_loop(struct libevdev* evdev)
{
  // Creates our virtual keyboard
  struct libevdev_uinput* uinput_dev;
  if (libevdev_uinput_create_from_device(evdev, LIBEVDEV_UINPUT_OPEN_MANAGED, &uinput_dev) != 0) {
    ERROR("Failed creating uinput device");
    return false;
  }

  // Give X11/Wayland "some time" to find our newly created uinput device
  usleep(500 * 1000);

  // To remove duplicate events (i.e., 1 from real device + 1 from virtual device)
  libevdev_grab(evdev, LIBEVDEV_GRAB);

  struct pollfd fds[] = {{.fd = libevdev_get_fd(evdev), .events = POLLIN}};

  int rc = -1;
  while (poll(&fds[0], ARRAY_SIZE(fds), -1) > 0) {
    do {
      // Internally, libevdev caches events
      struct input_event ev;
      rc = libevdev_next_event(evdev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
      if (rc == 0) {
        DEBUG("   R  Event: %s %s %d",
              libevdev_event_type_get_name(ev.type),
              libevdev_event_code_get_name(ev.type, ev.code),
              ev.value);

        if (is_killswitch_active(evdev)) {
          ERROR("KILLSWITCH detected; exiting\n");
          goto done;
        }

        handle_event(uinput_dev, &ev);
      }
      else if (rc == -ENODEV) {
        // TODO: Happens when we disconnect keyboard. Best we can do for now is just to break
        ERROR("Hot-plugging support not implemented; exiting");
        goto done;
      }
      else if (rc != -EAGAIN) {
        ERROR("next_event: Got %s", strerror(-rc));
      }
    } while (rc == LIBEVDEV_READ_STATUS_SUCCESS);
  }

done:
  libevdev_grab(evdev, LIBEVDEV_UNGRAB);
  libevdev_uinput_destroy(uinput_dev);
  return rc == LIBEVDEV_READ_STATUS_SUCCESS;
}

static void print_usage(void)
{
  fprintf(stderr, "Usage: %s [--debug] [path-to-keyboard-input-device]\n", program_invocation_name);
}

int main(int argc, char* argv[])
{
  if (geteuid() != 0) {
    ERROR("Program must run as root to be able to access inputs");
    print_usage();
    return -1;
  }

  if (argc > 1 && strcmp("--debug", argv[1]) == 0) {
    log_level = LOG_LEVEL_DEBUG;
    argc--;
    argv++;
  }

  const bool require_led = false;
  struct libevdev* evdev = (argc > 1) ? open_device(argv[1]) : find_keyboard_device();
  if (!is_keyboard_event_device(evdev, require_led)) {
    ERROR("Found no valid keyboard device, please supply one yourself as arg");
    goto done;
  }

  if (!run_event_loop(evdev)) {
    ERROR("Couldn't relay keypresses for some reason, exiting");
  }

done:
  if (evdev && libevdev_get_fd(evdev) != -1) {
    close(libevdev_get_fd(evdev));
  }
  libevdev_free(evdev);

  return -1;  // Can't get here without something being wrong
}
