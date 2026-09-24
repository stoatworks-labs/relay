# Attributions

Relay is built on other people's work. This file lists what that work is, who
did it, and what it is doing here.

It is a PROVISIONAL hand copy (2026-09-24), adapted from wipe's. The master
lists live in the `stoatworks-backend` repo and are pushed out by
`scripts/sync-attributions.py` when the plugin is registered in the fleet; that
sync overwrites this file.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Mixer mechanics, two-input harness and Timing — Stoatworks genlock and wipe

<https://github.com/stoatworks-labs/genlock>, <https://github.com/stoatworks-labs/wipe>  
Licence: MIT  
Copyright: Stoatworks Labs

The mixer mechanics of ProcessOpenGL (guard on the input count, guard on each pointer, one MaxUV per input, interleaved scoped bindings), the two-input harness rig, its --mixer check and its two-input --pipe, the host-clock Timing.*, the software-renderer pass and the CMake shape are genlock's and wipe's, adapted.

### PAL and NTSC raster timing — Stoatworks clamp

<https://github.com/stoatworks-labs/clamp>  
Licence: MIT  
Copyright: Stoatworks Labs

The line period, porches, sync and active-line counts of 625/50 and 525/59.94 in source/Raster.cpp are clamp's numbers, in a smaller struct.

### Diagnostics log — Stoatworks tinsel

<https://github.com/stoatworks-labs/tinsel>  
Licence: MIT  
Copyright: Stoatworks Labs

source/Diag.* is tinsel's log, renamed into this namespace.

### Sweep and verify shape — Stoatworks graticule

<https://github.com/stoatworks-labs/graticule>  
Licence: MIT  
Copyright: Stoatworks Labs

tools/sweep.py and the release-job-locally checks in tools/verify.sh follow genlock's, wipe's and graticule's.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl.

The plugin ABI itself. An FFGL effect, source or mixer is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### Relay-switched video routers

Implemented from the textbook behaviour of an electromechanical relay — coil hysteresis, operate time, contact bounce decaying by a coefficient of restitution — and of a monitor's vertical and horizontal PLLs as second- and first-order loops, applied to a raster scanned in real time. No particular router, relay datasheet, monitor or captured output was used; the look is a model, not a characterisation of anybody's equipment.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
