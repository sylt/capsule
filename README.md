# CAPSULE: Dual function Caps-Lock key

CAPSULE tries to provide a better default behavior in Linux for the
Caps Lock key. Since Caps Lock is fitted at a rather "ergonomical"
spot on the keyboard, it's a pity that it's not used more. Hopefully,
CAPSULE can help a little bit with the utilization.

The idea is not rocket science: CAPSULE will make Caps Lock act as a
modifier key when it is pressed together with some other key. For
example, while Caps Lock is held and H, J, K, or L is pressed, it will
generate input events as if the arrow keys would have been pressed, in
the style of Vim. However, if no key is pressed while holding the Caps
Lock key, then Caps Lock gets enabled as it normally would.

As of writing this, CAPSULE is extremely limited in functionality,
having only some hard-coded aliases. These aliases are:

| Key | Translation |
|-----|-------------|
| H   | Arrow Left  |
| J   | Arrow Down  |
| K   | Arrow Up    |
| L   | Arrow Right |
|     |             |
| D   | Delete      |
|     |             |
| A   | Home        |
| E   | End         |
|     |             |
| N   | PageDown    |
| P   | PageUp      |

CAPSULE injects itself at a very low level of the Linux input
stack. Due to this, the keyboard shortcuts are global, meaning that
you can use them from whatever application without any extra set up:
Your browser, your editor, or your chat program.

It should be noted that CAPSULE works the same when holding down say
the Control or Alt key. So, for example, if you hold down the Caps
Lock and Alt key while pressing down H, it will produce the same
result as pressing Alt + Arrow Right. This is a feature.

Swapping of the Escape and Caps Lock key is possible by using the
switch `--swap-caps-lock-and-escape`. This means that pressing only
Caps Lock on the keyboard will make it behave as if Escape was
pressed.

# How to compile and run

To compile, simply type `make`. You might need to install
`libevdev-dev` from your package manager.

To run, open up a terminal and execute `sudo ./capsule`, and it should
auto detect all your keyboard. New keyboards are automatically
detected when plugged in.

# Installing in systemd

1. Copy `capsule.service` file to `/lib/systemd/system/`.
2. Copy `capsule` binary to `/usr/sbin/capsule`.
3. Try and start to see if it work: `sudo systemctl start capsule`.
4. Then, enable it by default: `sudo systemctl enable capsule`.
5. Done. At next start-up, capsule will start automatically.

# Killswitch

Although CAPSULE has been tested, it should still be considered
unstable software at the moment. In case the keyboard starts behave
weird in such a way that nothing seems to work, it's possible to quit
CAPSULE by holding down both left and right control at the same time.

# Missing functionality

Lots, but on top of my mind:

* Config file support - Can't keep it hard-coded forever
* Different keyboard combos per user? Not sure how to even tackle this.
