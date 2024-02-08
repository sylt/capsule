#include <Windows.h>
#include <stdio.h>
#include <stdbool.h>

#define LOG_ERROR(fmt, ...) fprintf(stderr, "Error: " fmt "\n", ##__VA_ARGS__);
#define LOG_WARNING(fmt, ...) fprintf(stderr, "Warning: " fmt "\n", ##__VA_ARGS__);
#define LOG_DEBUG(fmt, ...)         \
  if (log_level >= LOG_LEVEL_DEBUG) \
  printf("Debug [%s]: " fmt " [%s:%d]\n", __func__, ##__VA_ARGS__, __FILE__, __LINE__)

#define ARRAY_SIZE(some_array) (sizeof(some_array) / sizeof((some_array)[0]))

static enum {
  LOG_LEVEL_ERROR,
  LOG_LEVEL_WARNING,
  LOG_LEVEL_DEBUG,
} log_level = LOG_LEVEL_WARNING;

static const struct
{
  const DWORD code; // If Caps Lock is pressed, try match with this key code
  const struct
  {
    DWORD code;
    bool left_alt;
    bool right_alt;
    bool left_ctrl;
  } output; // ... and if it matches, send this key combo
} action_table[] = {
    // Use Vim bindings for HJKL
    {.code = 'H', .output = {.code = VK_LEFT}},
    {.code = 'J', .output = {.code = VK_DOWN}},
    {.code = 'K', .output = {.code = VK_UP}},
    {.code = 'L', .output = {.code = VK_RIGHT}},

    // Remap N and P to produce PageUp and PageDown
    {.code = 'P', .output = {.code = VK_PRIOR}},
    {.code = 'N', .output = {.code = VK_NEXT}},

    // Remap D to Delete and Semicolon (ö) to backspace
    {.code = 'D', .output = {.code = VK_DELETE}},
    {.code = VK_OEM_3, .output = {.code = VK_BACK}},

    // Remap M to Enter
    {.code = 'M', .output = {.code = VK_RETURN}},

    // Remap A and E to Home and End
    {.code = 'A', .output = {.code = VK_HOME}},
    {.code = 'E', .output = {.code = VK_END}},
};

static struct
{
  bool swap_caps_lock_and_escape;

  struct keyboard
  {
    struct
    {
      bool grabbed;
      bool caps_lock_pressed;

      bool key_pressed_while_caps_lock_pressed;
      bool action_table_activated[ARRAY_SIZE(action_table)];
    } state;
  } keyboard;
} capsule = {.swap_caps_lock_and_escape = true};

#define KEY_CAPSLOCK VK_CAPITAL
#define KEY_ESC VK_ESCAPE

void SendKeyPress(DWORD vk, DWORD keyEventId)
{
  DWORD event = (keyEventId == WM_KEYUP || keyEventId == WM_SYSKEYUP) ? KEYEVENTF_KEYUP : 0;
  INPUT input[1] = {{
      .type = INPUT_KEYBOARD,
      .ki.wVk = vk,
      .ki.dwFlags = event,
  }};

  LOG_DEBUG("Injecting vkCode=0x%02x %s\n", vk, event ? "down" : "up");
  if (SendInput(ARRAY_SIZE(input), input, sizeof(input[0])) != ARRAY_SIZE(input))
  {
    LOG_ERROR("SendInput failed: 0x%x\n", HRESULT_FROM_WIN32(GetLastError()));
  }
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM keyEventId, LPARAM lParam)
{
  if (nCode != HC_ACTION)
  {
    goto call_next_hook;
  }

  PKBDLLHOOKSTRUCT p = (PKBDLLHOOKSTRUCT)lParam;
  if (p->flags & LLKHF_INJECTED)
  {
    goto call_next_hook;
  }

  struct keyboard *keyboard = &capsule.keyboard;

  LOG_DEBUG("Incoming [keyEventId=0x%04x] scanCode=0x%02x vkCode=0x%02x flags=0x%04x dwExtraInfo=0x%x\n",
            keyEventId, p->scanCode, p->vkCode, p->flags, p->dwExtraInfo);

  if (capsule.swap_caps_lock_and_escape && p->vkCode == KEY_ESC)
  {
    SendKeyPress(KEY_CAPSLOCK, keyEventId);
    return 1;
  }

  // If it's not pressed, it's released
  const bool keyPressed = keyEventId == WM_KEYDOWN || keyEventId == WM_SYSKEYDOWN;

  if (p->vkCode == KEY_CAPSLOCK)
  {
    if (keyPressed)
    {
      keyboard->state.caps_lock_pressed = true;
      keyboard->state.key_pressed_while_caps_lock_pressed = false;
      return 1;
    }

    keyboard->state.caps_lock_pressed = false;
    if (keyboard->state.key_pressed_while_caps_lock_pressed)
    {
      return 1;
    }

    const unsigned int key = capsule.swap_caps_lock_and_escape ? KEY_ESC : KEY_CAPSLOCK;
    SendKeyPress(key, WM_KEYDOWN);
    SendKeyPress(key, WM_KEYUP);
    return 1;
  }

  for (size_t i = 0; i < ARRAY_SIZE(action_table); i++)
  {
    if (action_table[i].code != p->vkCode)
    {
      continue;
    }

    // From this line on, we have a match, but first handle some cases where we back off
    if (keyPressed && !keyboard->state.caps_lock_pressed)
    {
      goto call_next_hook; // Key was pressed "normally", without caps lock held in
    }

    if (!keyPressed && !keyboard->state.action_table_activated[i])
    {
      goto call_next_hook; // Key was pressed while caps lock wasn't held, so treat normally
    }

    // From here on, we know we should do something

    //[wParam=0x0104] scanCode=0x21d vkCode=0xa2 flags=0x0020 dwExtraInfo=0x0
    //[wParam=0x0104] scanCode=0x38 vkCode=0xa5 flags=0x0021 dwExtraInfo=0x0
    //[wParam=0x0101] scanCode=0x21d vkCode=0xa2 flags=0x0080 dwExtraInfo=0x0
    //[wParam=0x0101] scanCode=0x38 vkCode=0xa5 flags=0x0081 dwExtraInfo=0x0
    if (action_table[i].output.right_alt)
    {
      // write_event_to_uinput(keyboard->uinput_dev, EV_KEY, KEY_RIGHTALT, ev->value);
    }

    //[wParam=0x0100] scanCode=0x1d vkCode=0xa2 flags=0x0000 dwExtraInfo=0x0
    //[wParam=0x0101] scanCode=0x1d vkCode=0xa2 flags=0x0080 dwExtraInfo=0x0
    if (action_table[i].output.left_ctrl)
    {
      // write_event_to_uinput(keyboard->uinput_dev, EV_KEY, KEY_LEFTCTRL, ev->value);
    }
    SendKeyPress(action_table[i].output.code, keyEventId);

    // Something was done, and that's worth book keeping
    const bool activated = (keyPressed && keyboard->state.caps_lock_pressed);
    keyboard->state.action_table_activated[i] = activated;
    keyboard->state.key_pressed_while_caps_lock_pressed |= activated;

    return 1;
  }

  if (keyboard->state.caps_lock_pressed)
  {
    keyboard->state.key_pressed_while_caps_lock_pressed |= keyPressed;
  }

call_next_hook:
  return CallNextHookEx(NULL, nCode, keyEventId, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
  if (log_level != LOG_LEVEL_DEBUG)
  {
    HWND hwnd = GetConsoleWindow();
    ShowWindow(hwnd, 0);
  }

  // Install the low-level keyboard & mouse hooks
  HHOOK hhkLowLevelKybd = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, 0, 0);

  // Keep this app running until we're told to stop
  MSG msg;
  while (!GetMessage(&msg, NULL, 0, 0))
  { // this while loop keeps the hook
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }

  UnhookWindowsHookEx(hhkLowLevelKybd);

  return 0;
}