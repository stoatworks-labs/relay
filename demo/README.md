# demo/ — the browser demo

Live at **<https://relay-demo.stoatworks-labs.com>**. Not served from this
README: `.assetsignore` keeps this file and `tools/` out of the upload.

    index.html       the shell
    plugin.js        the parameters, the plugin's two shaders, the frame logic
    model.js         the CPU half, ported: Controls.cpp, Raster.cpp, Model.cpp
    vendor/          the shared kit, copied in by sync.sh — DO NOT EDIT
    tools/           check_shaders.py, run by tools/verify.sh
    _headers         CSP and caching, honoured by the Cloudflare assets runtime

## What this page is, exactly

A **port**, not a recording and not the plugin.

The shaders are the plugin's, copied across unedited by script: `VERTEX` and
`RELAY` in `plugin.js` are `kVertexShader` and `kRelayShader` from
`source/Shaders.cpp`. `tools/check_shaders.py` compares them character for
character and `../tools/verify.sh` runs it.

The CPU half is a port — `model.js` (the coil's hysteresis, the bounce
schedule, the PAL/NTSC raster mapping, the second-order re-lock, the crosstalk
filter, every `...FromParam`) and the frame logic of `Relay::ProcessOpenGL` in
`plugin.js` (operate time, Anywhere / Vertical Interval firing, a switch during
a switch, the schedule turned into this frame's cuts, the roll, the Take
latch) — and **nothing checks it but a reader.**

Relay is a **mixer**, and the kit hands a demo one input. So A (the layer
below) is the kit's clip, relabelled `Clip A`, and B (this layer) is a second
copy of the kit's clip generator, picked by the transport's `Clip B`. In
Resolume the coil voltage is the layer's opacity fader; here it is the
`Opacity` slider. `Take` is an FF_TYPE_EVENT in the plugin and a toggle the
page releases here. "Hold switching frame" in the transport is the page's
convenience, not a plugin control: it pauses the page's clock on the first
frame a bounce schedule cuts into.

Everything else is not the plugin: no Resolume, no layer stack, no FFGL, no
padded textures (both MaxUVs are 1), the page's clock in place of SetTime, and
GLSL ES 3.00 in WebGL2 rather than desktop GL 4.1 core. The page's own
disclosure lists every difference.

## Working on it

```bash
python3 -m http.server 8952          # from this directory
python3 tools/check_shaders.py       # the copies still match the C++
../tools/verify.sh                   # everything, including the above
```

There is no build step. It is hand-written ES modules and what is committed is
what is served. A push to main deploys it (`.github/workflows/deploy.yml`);
by hand, `cf-run npx wrangler deploy` from the repo root. Verify by content:
`curl -s 'https://relay-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'`.

**After changing a shader in `source/Shaders.cpp`, copy it across here too** —
`check_shaders.py` names the shader and the first differing line. After
changing Controls.cpp, Raster.cpp, Model.cpp or ProcessOpenGL, change
`model.js` or `plugin.js` to match; nothing will tell you if you forget.
