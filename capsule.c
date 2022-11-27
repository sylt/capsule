#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libevdev/libevdev-uinput.h>
#include <linux/input.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#define ERROR(fmt, ...) fprintf(stderr, "Error: " fmt "\n", ##__VA_ARGS__);
#define WARNING(fmt, ...) fprintf(stderr, "Warning: " fmt "\n", ##__VA_ARGS__);
#define DEBUG(fmt, ...) \
  if (log_level >= LOG_LEVEL_DEBUG) \
  printf("Debug [%s]: " fmt " [%s:%d]\n", __func__, ##__VA_ARGS__, __FILE__, __LINE__)

#define ARRAY_SIZE(some_array) (sizeof(some_array) / sizeof((some_array)[0]))

#define INPUT_DEVICE_PATH "/dev/input/by-path"

static enum {
  LOG_LEVEL_ERROR,
  LOG_LEVEL_WARNING,
  LOG_LEVEL_DEBUG,
} log_level = LOG_LEVEL_WARNING;

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
  DIR* dev_dirp;  // Base dir of where we find/monitor for keyboard devices
  int inotify_fd;
  int inotify_wd;

  bool swap_caps_lock_and_escape;

  struct keyboard {
    struct {
      bool grabbed;
      bool caps_lock_pressed;

      bool key_pressed_while_caps_lock_pressed;
      bool action_table_activated[ARRAY_SIZE(action_table)];

      bool marked_for_deletion;  // Used for detecting when keyboard has been unplugged
    } state;

    ino_t inode;
    int event_fd;
    struct libevdev* dev;
    struct libevdev_uinput* uinput_dev;
  } keyboards[16];  // Should be enough for anybody
} capsule;

#define POLLFDS_MAX_NUM_FDS (ARRAY_SIZE(capsule.keyboards) + 1 /*inotify_fd*/)

#define FOR_EACH_KEYBOARD(kbd) \
  for (struct keyboard* kbd = &capsule.keyboards[0]; \
       kbd < &capsule.keyboards[ARRAY_SIZE(capsule.keyboards)]; \
       ++kbd)

static void close_keyboard(struct keyboard* keyboard)
{
  if (keyboard->inode > 0) {
    DEBUG("ino=%ju", (uintmax_t)keyboard->inode);
  }

  if (keyboard->dev) {
    if (keyboard->state.grabbed) {
      libevdev_grab(keyboard->dev, LIBEVDEV_UNGRAB);
    }
    close(libevdev_get_fd(keyboard->dev));
    libevdev_free(keyboard->dev);
  }
  if (keyboard->uinput_dev) {
    libevdev_uinput_destroy(keyboard->uinput_dev);
  }

  if (keyboard->event_fd >= 0) {
    close(keyboard->event_fd);
  }

  memset(keyboard, 0, sizeof(*keyboard));
  keyboard->event_fd = -1;
}

static bool init_capsule(void)
{
  capsule.inotify_fd = -1;
  capsule.inotify_wd = -1;

  FOR_EACH_KEYBOARD (keyboard) {
    close_keyboard(keyboard);
  }

  capsule.dev_dirp = opendir(INPUT_DEVICE_PATH);
  if (!capsule.dev_dirp) {
    ERROR("Couldn't open " INPUT_DEVICE_PATH ": %s", strerror(errno));
    return false;
  }

  capsule.inotify_fd = inotify_init1(O_NONBLOCK);
  if (capsule.inotify_fd == -1) {
    ERROR("Couldn't open inotify fd: %s", strerror(errno));
    return false;
  }

  capsule.inotify_wd =
      inotify_add_watch(capsule.inotify_fd, INPUT_DEVICE_PATH, IN_CREATE | IN_DELETE);
  if (capsule.inotify_wd == -1) {
    ERROR("inotify_add_watch failed: %s", strerror(errno));
    return false;
  }

  return true;
}

static struct keyboard* find_keyboard_by_inode(ino_t inode)
{
  FOR_EACH_KEYBOARD (keyboard) {
    if (keyboard->inode == inode) {
      return keyboard;
    }
  }
  return NULL;
}

static struct keyboard* find_free_keyboard_struct(void)
{
  FOR_EACH_KEYBOARD (keyboard) {
    if (!keyboard->dev) {
      assert(!keyboard->uinput_dev);
      return keyboard;
    }
  }
  return NULL;
}

static bool setup_keyboard(struct keyboard* keyboard, DIR* base_dirp, struct dirent* dirent)
{
  DEBUG("%s (ino=%ju)", dirent->d_name, (uintmax_t)dirent->d_ino);
  keyboard->event_fd = openat(dirfd(base_dirp), dirent->d_name, O_RDONLY | O_NONBLOCK);
  if (keyboard->event_fd == -1) {
    ERROR("Couldn't open %s: %s", dirent->d_name, strerror(errno));
    goto done;
  }

  keyboard->dev = libevdev_new();
  assert(keyboard->dev);  // Weird enough that it's worth getting a crash to know

  if (log_level == LOG_LEVEL_DEBUG) {
    // TODO: Implement installing of libevdev log callbacks here
  }

  keyboard->inode = dirent->d_ino;

  if (libevdev_set_fd(keyboard->dev, keyboard->event_fd) < 0) {
    ERROR("Couldn't set fd for device %s", dirent->d_name);
    goto done;
  }

  if (libevdev_uinput_create_from_device(
          keyboard->dev, LIBEVDEV_UINPUT_OPEN_MANAGED, &keyboard->uinput_dev)
      != 0) {
    ERROR("Failed creating uinput device");
  }

done:
  if (!keyboard->uinput_dev) {
    close_keyboard(keyboard);
  }

  return keyboard->uinput_dev;
}

static bool scan_keyboards(void)
{
  FOR_EACH_KEYBOARD (keyboard) {
    keyboard->state.marked_for_deletion = (keyboard->dev != NULL);
  }

  rewinddir(capsule.dev_dirp);

  struct dirent* dirent;
  while ((dirent = readdir(capsule.dev_dirp))) {
    DEBUG("%s", dirent->d_name);
    if (!strstr(dirent->d_name, "event-kbd")) {
      continue;
    }

    struct keyboard* keyboard = find_keyboard_by_inode(dirent->d_ino);
    if (keyboard) {
      keyboard->state.marked_for_deletion = false;
      continue;
    }

    keyboard = find_free_keyboard_struct();
    assert(keyboard);

    if (!setup_keyboard(keyboard, capsule.dev_dirp, dirent)) {
      ERROR("Couldn't set-up keyboard %s", dirent->d_name);
    }
  }

  size_t num_keyboards_setup = 0;
  FOR_EACH_KEYBOARD (keyboard) {
    if (keyboard->state.marked_for_deletion) {
      close_keyboard(keyboard);
    }
    else {
      keyboard->state.marked_for_deletion = false;
    }
    num_keyboards_setup += (keyboard->dev != NULL);
  }

  return num_keyboards_setup > 0;
}

static void write_event_to_uinput(struct libevdev_uinput* uinput_dev,
                                  unsigned int type,
                                  unsigned int code,
                                  int value)
{
  DEBUG("W Event: %s %s %d",
        libevdev_event_type_get_name(type),
        libevdev_event_code_get_name(type, code),
        value);
  libevdev_uinput_write_event(uinput_dev, type, code, value);
}

static void handle_input_event(struct keyboard* keyboard, struct input_event* ev)
{
  if (ev->type != EV_KEY) {
    goto forward_event;
  }

  if (capsule.swap_caps_lock_and_escape && ev->code == KEY_ESC) {
    ev->code = KEY_CAPSLOCK;
    goto forward_event;
  }

  if (ev->code == KEY_CAPSLOCK) {
    if (ev->value > 1) {  // Key repeat
      return;
    }
    else if (ev->value == 1) {
      keyboard->state.caps_lock_pressed = true;
      keyboard->state.key_pressed_while_caps_lock_pressed = false;
      return;
    }

    keyboard->state.caps_lock_pressed = false;
    if (keyboard->state.key_pressed_while_caps_lock_pressed) {
      return;
    }

    const unsigned int key = capsule.swap_caps_lock_and_escape ? KEY_ESC : KEY_CAPSLOCK;
    write_event_to_uinput(keyboard->uinput_dev, EV_KEY, key, 1);
    write_event_to_uinput(keyboard->uinput_dev, EV_KEY, EV_SYN, 0);
    write_event_to_uinput(keyboard->uinput_dev, EV_KEY, key, 0);
    return;
  }

  for (size_t i = 0; i < ARRAY_SIZE(action_table); i++) {
    if (action_table[i].code != ev->code) {
      continue;
    }

    // From this line on, we have a match, but first handle some cases where we back off
    if (ev->value == 1 && !keyboard->state.caps_lock_pressed) {
      goto forward_event;  // Key was pressed "normally", without caps lock held in
    }

    if (ev->value != 1 && !keyboard->state.action_table_activated[i]) {
      goto forward_event;  // Key was pressed while caps lock wasn't held, so treat normally
    }

    // From here on, we know we should do something
    if (action_table[i].output.right_alt && ev->value <= 1) {
      write_event_to_uinput(keyboard->uinput_dev, EV_KEY, KEY_RIGHTALT, ev->value);
    }
    if (action_table[i].output.left_ctrl && ev->value <= 1) {
      write_event_to_uinput(keyboard->uinput_dev, EV_KEY, KEY_LEFTCTRL, ev->value);
    }
    write_event_to_uinput(keyboard->uinput_dev, EV_KEY, action_table[i].output.code, ev->value);

    // Something was done, and that's worth book keeping
    if (ev->value <= 1) {
      const bool activated = (ev->value == 1 && keyboard->state.caps_lock_pressed);
      keyboard->state.action_table_activated[i] = activated;
      keyboard->state.key_pressed_while_caps_lock_pressed |= activated;
    }

    return;
  }

  if (keyboard->state.caps_lock_pressed) {
    keyboard->state.key_pressed_while_caps_lock_pressed |= ev->value == 1;
  }

forward_event:
  write_event_to_uinput(keyboard->uinput_dev, ev->type, ev->code, ev->value);
}

static bool is_killswitch_active(const struct libevdev* evdev)
{
  return libevdev_get_event_value(evdev, EV_KEY, KEY_LEFTCTRL) > 0
         && libevdev_get_event_value(evdev, EV_KEY, KEY_RIGHTCTRL) > 0;
}

static void drain_inotify_events(void)
{
  DEBUG();
  for (;;) {
    // As recommended in man inotify(7):
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t len = read(capsule.inotify_fd, buf, ARRAY_SIZE(buf));
    if (len <= 0) {
      if (errno != EAGAIN) {
        ERROR("read() gave error %s", strerror(errno));
      }
      break;
    }

    // We're not really interested in handling the exact events; we just want to know when it's time
    // to rescan our watched directory (and thus the name of this method)
  }
}

static size_t construct_pollfd_array(struct pollfd* pfds, size_t pfds_size)
{
  *pfds++ = (struct pollfd){.fd = capsule.inotify_fd, .events = POLLIN};
  pfds_size--;

  size_t remaining = pfds_size;
  FOR_EACH_KEYBOARD (keyboard) {
    if (remaining == 0) {
      ERROR("Bad sizing of pollfd array");
      break;
    }

    const int fd = keyboard->dev ? libevdev_get_fd(keyboard->dev) : -1;
    *pfds++ = (struct pollfd){.fd = fd, .events = POLLIN};
    --remaining;
  }
  return pfds_size - remaining;
}

struct keyboard* get_keyboard_from_pollfd(struct pollfd* pollfd_array, struct pollfd* pfd)
{
  return &capsule.keyboards[(pfd - pollfd_array) - 1];
}

static bool handle_keyboard_evdev_event(struct keyboard* keyboard)
{
  int rc;
  do {
    // Internally, libevdev caches events
    struct input_event ev;
    rc = libevdev_next_event(keyboard->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
    if (rc == 0) {
      DEBUG("R Event: %s %s %d",
            libevdev_event_type_get_name(ev.type),
            libevdev_event_code_get_name(ev.type, ev.code),
            ev.value);

      if (is_killswitch_active(keyboard->dev)) {
        ERROR("KILLSWITCH detected; exiting\n");
        return false;
      }

      handle_input_event(keyboard, &ev);
    }
    else if (rc == -ENODEV) {
      DEBUG("No device; it will probably be removed soon");
      break;
    }
    else if (rc != -EAGAIN) {
      ERROR("next_event: Got %s", strerror(-rc));
    }
  } while (rc == LIBEVDEV_READ_STATUS_SUCCESS);

  return true;
}

static void grab_all_keyboards(void)
{
  // Grab devices to remove duplicate events (i.e., 1 from real device + 1 from virtual device)
  FOR_EACH_KEYBOARD (keyboard) {
    if (keyboard->dev && !keyboard->state.grabbed) {
      keyboard->state.grabbed = libevdev_grab(keyboard->dev, LIBEVDEV_GRAB) == 0;
    }
  }
}

static void run_event_loop(void)
{
  // Unfortunate, but give X11/Wayland "some time" to find our newly created uinput devices
  usleep(500 * 1000);

  grab_all_keyboards();

  struct pollfd pollfd_array[POLLFDS_MAX_NUM_FDS];
  size_t num_fds = construct_pollfd_array(pollfd_array, ARRAY_SIZE(pollfd_array));

  int events_to_handle = -1;  // -1 = any number, >= 0 = as many before exiting
  while (poll(pollfd_array, num_fds, -1) > 0) {
    if (pollfd_array[0].revents & POLLIN) {
      drain_inotify_events();
      scan_keyboards();
      grab_all_keyboards();
      num_fds = construct_pollfd_array(pollfd_array, ARRAY_SIZE(pollfd_array));
      continue;
    }

    struct pollfd* keyboard_pollfd = &pollfd_array[1];
    do {
      struct keyboard* keyboard = get_keyboard_from_pollfd(pollfd_array, keyboard_pollfd);
      if (keyboard_pollfd->revents & POLLERR) {
        close_keyboard(keyboard);
        num_fds = construct_pollfd_array(pollfd_array, ARRAY_SIZE(pollfd_array));
        break;
      }
      if (!(keyboard_pollfd->revents & POLLIN)) {
        continue;
      }

      if (!handle_keyboard_evdev_event(keyboard)) {
        goto done;
      }
    } while (++keyboard_pollfd != &pollfd_array[ARRAY_SIZE(pollfd_array)]);

    if (events_to_handle >= 0 && --events_to_handle == -1)
      break;
  }

done:
  (void)0;  // Makes clang happy
}

static void print_usage(void)
{
  fprintf(stderr,
          "Usage: %s"
          " [--swap-caps-lock-and-escape]"
          " [--debug]"
          "\n",
          program_invocation_name);
}

int main(int argc, char* argv[])
{
  if (geteuid() != 0) {
    ERROR("Program must run as root to be able to access inputs");
    print_usage();
    return -1;
  }

  if (!init_capsule()) {
    goto done;
  }

  while (argc > 1) {
    if (strcmp("-h", argv[1]) == 0 || strcmp("-help", argv[1]) == 0
        || strcmp("--help", argv[1]) == 0) {
      print_usage();
      return 0;
    }

    if (strcmp("--debug", argv[1]) == 0) {
      log_level = LOG_LEVEL_DEBUG;
    }
    else if (strcmp("--swap-caps-lock-and-escape", argv[1]) == 0) {
      capsule.swap_caps_lock_and_escape = true;
    }
    else {
      ERROR("Unrecognized switch: %s", argv[1]);
      print_usage();
      return -1;
    }

    argc--;
    argv++;
  }

  if (!scan_keyboards()) {
    WARNING("Found no keyboards connected; this is probably a bug");
    goto done;
  }

  run_event_loop();

done:
  FOR_EACH_KEYBOARD (keyboard) {
    close_keyboard(keyboard);
  }

  if (capsule.dev_dirp) {
    closedir(capsule.dev_dirp);
  }

  return -1;  // Can't get here without something being wrong
}
