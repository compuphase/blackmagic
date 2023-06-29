# Black Magic Probe (for NXP LPC series)

**Nota Bene** This is a fork of release 1.7 of the original [Black Magic Probe project](https://github.com/blackmagic-debug/blackmagic). For newer releases, please see that project. For documentation on the Black Magic Probe, also go to the original project, or read my free e-book [Embedded Debugging with the Black Magic Probe](https://github.com/compuphase/Black-Magic-Probe-Book).

## Why does this fork exist?
The official release 1.7 has always run reliably in our production, but it does not support a few of the micro-controllers of the LPC family (by NXP) that we happen to use in our products. I am specifically referring to the LPC800 series and the LPC1110-**XL** series.

At the time of writing, *all* official releases after 1.7 have been broken when it comes to the support for the LPC series of microcontrollers. Versions 1.9.0 and 1.9.1 are fatally broken in the sense that they don't work at all with LPC microcontrollers; versions 1.8.0 to 1.8.2 are too flaky, with random failures.

### Rant: Why choose to fork?
One might say that, instead of forking, I could also choose to contribute to the original project. My response is that I did, but it got wearisome.

Support for various LPC microcontrollers in the project (not limited to the above-mentioned LPC800 and LPC1100-**XL** series) was contributed by me. I also pointed out the wrong order of polling `SWDIO` and toggling `SWCLK` that plagued the 1.8.x releases. I identified (and reported) the divide-by-zero bug in release 1.9.0. After expressing my concern how this could have passed testing, 1BitSquared replied that they did not have *any* development board with an LPC microcontroller, and thus cannot test. I swallowed my astonishment that "not having the ability to test" does apparently not withhold the team from refactoring the code, but offered them two of our own boards (with different LPC microcontrollers). I shipped these including the schematics, and readily built "blinky" programs with source code as a jump start. However, though the divide-by-zero bug was fixed, release 1.9.1 is now broken in the Flash programming routines (for LPC microcontrollers).

## Post 1.7 Features and Fixes
While this fork is basically release 1.7, selected fixes and changes from later releases have been merged in. The most notable ones are:
* Added support for LPC800, LPC1110-XL and LPC4000 series.
* Merged in the "fake thread" support needed for GDB 11 and later (see PR #1125 of the [Black Magic Probe project](https://github.com/blackmagic-debug/blackmagic)).
* Merged the changes needed for hardware revision 6 of the Black Magic Probe (so this fork runs on both BMP v2.1 and BMP v2.3).
* Correct scheduling of driving/polling `SWDIO` relative to `SWCLK` and in regard to set-up and hold times (this is my own implementation, not based on PR #1220 of the [Black Magic Probe project](https://github.com/blackmagic-debug/blackmagic), which is *still* not entirely correct).
