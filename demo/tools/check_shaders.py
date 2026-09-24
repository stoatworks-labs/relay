"""The demo's shaders must be the plugin's, character for character.

    python3 demo/tools/check_shaders.py

Called from `tools/verify.sh`. Exit code 1 means the two copies have drifted.

------------------------------------------------------------------- why

`demo/plugin.js` holds the two GLSL sources that `source/Shaders.cpp` holds:
the vertex quad and the relay pass itself -- the switching frame's cuts, the
roll and the crosstalk. Two copies of the same text drift, quietly, because a
demo that renders a *plausible* switching frame looks exactly like one that
renders the right one. The page's whole claim is that it runs the plugin's own
shader, so the claim needs something enforcing it. `rltest` drives the real
plugin class and has no idea the page exists, and verify.sh's glslc step never
looks at the JS copy.

------------------------------------------------------------------- what it does

Galvo's check, pointed at this repo: pulls each `R"( ... )"` body out of the
C++ and each matching backtick literal out of `plugin.js`, and compares them
exactly -- no whitespace normalisation, no comment stripping. The one
transformation is a decode: a backtick inside the shader would have to be
escaped as \\` in a template literal, so that escape is undone here and *any
other backslash on the JS side is rejected*. Relay's shaders carry neither a
backtick nor a backslash today, so the decoder is idle; if one ever appears in
the C++, this is where it is accounted for.

------------------------------------------------------------------- what it cannot

Nothing here checks the *ported* half: `demo/model.js` (Controls.cpp,
Raster.cpp, Model.cpp) and the frame logic in `demo/plugin.js` (Relay.cpp's
ProcessOpenGL and the Take latch) are a hand translation, and only a reader
can tell whether they still agree. When you change one of those, change it
here too.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

# JS constant, C++ file, C++ symbol. Relay has one pass: the quad and the relay.
SHADERS = [
    ("VERTEX", "source/Shaders.cpp", "kVertexShader"),
    ("RELAY", "source/Shaders.cpp", "kRelayShader"),
]


def from_cpp(path, symbol):
    with open(os.path.join(REPO, path)) as handle:
        source = handle.read()
    match = re.search(r'const char\* const ' + symbol + r' = R"\((.*?)\)";', source, re.S)
    if match is None:
        return None
    return match.group(1)


def from_js(source, name):
    match = re.search(r'^const ' + name + r' = `(.*?)`;$', source, re.S | re.M)
    if match is None:
        return None, None

    body = match.group(1)

    # Undo the one escape the literal needs, and refuse the rest. The C++ carries
    # no backslash at all, so a stray one here is either a typo or a difference
    # being smuggled through the decoder.
    stray = re.search(r"\\(?!`)", body)
    if stray is not None:
        upto = body[: stray.start()]
        return None, f"backslash that is not an escaped backtick, at line {upto.count(chr(10)) + 1}"

    return body.replace("\\`", "`"), None


def main():
    with open(os.path.join(REPO, "demo", "plugin.js")) as handle:
        js = handle.read()

    problems = 0
    for name, path, symbol in SHADERS:
        cpp_text = from_cpp(path, symbol)
        js_text, complaint = from_js(js, name)

        if cpp_text is None:
            print(f"FAIL  {symbol} not found in {path}")
            problems += 1
            continue
        if complaint is not None:
            print(f"FAIL  {name} in demo/plugin.js has a {complaint}")
            problems += 1
            continue
        if js_text is None:
            print(f"FAIL  {name} not found in demo/plugin.js")
            problems += 1
            continue

        if cpp_text == js_text:
            print(f"ok    {name:<20} matches {symbol} ({len(cpp_text)} chars)")
            continue

        problems += 1
        print(f"FAIL  {name} has drifted from {symbol} in {path}")

        cpp_lines = cpp_text.splitlines()
        js_lines = js_text.splitlines()
        for i in range(max(len(cpp_lines), len(js_lines))):
            a = cpp_lines[i] if i < len(cpp_lines) else "<missing>"
            b = js_lines[i] if i < len(js_lines) else "<missing>"
            if a != b:
                print(f"        first difference at line {i + 1}")
                print(f"          C++: {a}")
                print(f"          js : {b}")
                break

    print()
    if problems:
        print(f"{problems} shader(s) differ -- copy the C++ across, do not edit plugin.js by hand")
        return 1

    print(f"all {len(SHADERS)} shaders are identical to the plugin's")
    return 0


if __name__ == "__main__":
    sys.exit(main())
