#!/bin/bash
# D259: a prebuilt runtime asset follows the translation unit.
# Size and mtime staying put must still select a new object, and a
# quoted header edit must do the same when the .c file is untouched.
# Not a byte-for-byte perl compare. make test-all runs this after the
# harness. The compiler and sources it mutates are copies under /tmp.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=/tmp/pt/d259
rm -rf "$ROOT"
mkdir -p "$ROOT/src"
cp ./perlc "$ROOT/perlc"
cp src/runtime.c src/runtime.h src/mini-gmp.c src/mini-gmp.h \
   src/native_stdlib.c src/config_data.h src/native_constants.h \
   "$ROOT/src/"
PROG=/tmp/pt/d259_die.pl
printf 'die "boom";\n' > "$PROG"

asset_of() {
    if ! "$ROOT/perlc" -v "$PROG" -o "$ROOT/bin" >"$ROOT/cout" 2>"$ROOT/cerr"; then
        echo "perlc failed" >&2
        cat "$ROOT/cerr" >&2
        exit 1
    fi
    local p
    p=$(grep -o '/[^ ]*runtime\.c\.[^ ]*\.o2\.o' "$ROOT/cerr" | head -1)
    if [[ -z "$p" || ! -f "$p" ]]; then
        echo "link did not use a cached runtime asset" >&2
        cat "$ROOT/cerr" >&2
        exit 1
    fi
    printf '%s' "$p"
}

die_err() {
    set +e
    "$ROOT/bin" >/dev/null 2>"$ROOT/err"
    set -e
    cat "$ROOT/err"
}

A=$(asset_of)
B=$(asset_of)
if [[ "$A" != "$B" ]]; then
    echo "unchanged sources produced a different asset" >&2
    echo "A=$A" >&2
    echo "B=$B" >&2
    exit 1
fi
die_err | grep -F 'boom at ' >/dev/null

cp "$ROOT/src/runtime.c" "$ROOT/src/runtime.c.orig"
python3 - <<'PY'
import pathlib
p = pathlib.Path("/tmp/pt/d259/src/runtime.c")
text = p.read_text()
old = '"%s at %s line %d.\\n"'
new = '"%s AT %s line %d.\\n"'
n = text.count(old)
if n != 1:
    raise SystemExit("die format count is %s" % n)
p.write_text(text.replace(old, new, 1))
PY
touch -r "$ROOT/src/runtime.c.orig" "$ROOT/src/runtime.c"
if [[ $(stat -c %s "$ROOT/src/runtime.c") != $(stat -c %s "$ROOT/src/runtime.c.orig") ]]; then
    echo "same-length edit changed the file size" >&2
    exit 1
fi
if [[ $(stat -c %Y "$ROOT/src/runtime.c") != $(stat -c %Y "$ROOT/src/runtime.c.orig") ]]; then
    echo "restored mtime did not stick" >&2
    exit 1
fi
C=$(asset_of)
if [[ "$C" == "$A" ]]; then
    echo "same-size same-mtime edit reused the asset" >&2
    echo "A=$A" >&2
    exit 1
fi
die_err | grep -F 'boom AT ' >/dev/null

cp "$ROOT/src/runtime.c.orig" "$ROOT/src/runtime.c"
touch -r "$ROOT/src/runtime.c.orig" "$ROOT/src/runtime.c"
D=$(asset_of)
if [[ "$D" != "$A" ]]; then
    echo "restored runtime.c did not select the original asset" >&2
    echo "A=$A" >&2
    echo "D=$D" >&2
    exit 1
fi
die_err | grep -F 'boom at ' >/dev/null

printf '\n/* d259-asset-sync */\n' >> "$ROOT/src/runtime.h"
E=$(asset_of)
if [[ "$E" == "$A" ]]; then
    echo "runtime.h edit reused the asset" >&2
    echo "A=$A" >&2
    exit 1
fi

echo D259_OK
