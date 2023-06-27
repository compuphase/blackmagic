# Black Magic Probe (for NXP LPC series)

**Nota Bene** This is a fork of release 1.7 of the original [Black Magic Probe project](https://github.com/blackmagic-debug/blackmagic). For newer releases, please see that project. For documentation on the Black Magic Probe, also go to the original project, or read my free e-book [Embedded Debugging with the Black Magic Probe](https://github.com/compuphase/Black-Magic-Probe-Book).

## Why does this fork exist?
The official release 1.7 does not support a few of the micro-controllers of the LPC family (by NXP) that we use a lot for our products. I am specifically referring to the LPC800 series and the LPC1110-**XL** series.

While support for these micro-controllers was added in release 1.8 (my contribution, by the way), the low-level handling of the SWD protocol (done with bit-banging) was refactored in that release. Due to the changed sequencing of instructions, the minimum set-up and hold times for the `SWDIO` signal were no longer upheld. More concretely, on a *read*, a target microcontroller sets the `SWDIO` pin on a falling edge of the clock line (`SWCLK`); the probe then polls `SWDIO`, but should do so *after* a mimimum "set-up" delay of 4ns. In all 1.8.x releases, this minimum set-up time was not respected. Now, for a micro-controller, that 4ns is the *maximum* time they have to set `SWDIO`, and most are way faster. It is possible that the LPC micro-controllers are more critical with regard to `SWDIO` set-up time (than e.g. STM32), and that it therefore slipped through testing, but the net result is that the 1.8.x releases were *unreliable* for use with the microcontrollers that we use most.

Release 1.9 finally corrected the SWD protocol handling (due to my contribution to this issue). However, the method for probing the NXP LPC processor family had been refactored, and due to a divide-by-zero error, the Black Magic Probe (with firmware 1.9) crashes on *any* target with an LPC processor. The 1.9.1 release fixes that (again, due to my contribution), but it still crashes as soon as you try to download code into Flash memory (those routines have been refactored too).

So, in summary, release 1.7 lacked support for some microcontrollers that we use, and there has since not been a single release that is usable with the LPC family of microcontrollers. And it gets wearisome to help fix an issue, only to see that you still cannot use the next release because something else got broken. Which is why I decided to go back to the latest reliable release, and add corrections and/or improvements as I see fit. This fork will not be the latest and greatest in features, but if you work with microcontrollers from the LPC family, this fork is probably the one that works.

## Post 1.7 Features and Fixes
While this fork is basically release 1.7, selected fixes and changes from later releases have been merged in. The most notable ones are:
* Added support for LPC800, LPC1110-XL and LPC4000 series.
* Merged in the "fake thread" support needed for GDB 11 and later (see PR #1125 on the [Black Magic Probe project](https://github.com/blackmagic-debug/blackmagic)).
* Merged the changes needed for hardware revision 6 of the Black Magic Probe (so this fork runs on both BMP v2.1 and BMP v2.3).
