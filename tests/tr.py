#!/usr/bin/env python3
# Port of tr.pl — Perl's tr/// (a.k.a. y///) transliteration operator.


def _expand(spec):
    """Expand a tr search/replace list, handling a-z style ranges."""
    out = []
    i = 0
    while i < len(spec):
        if i + 2 < len(spec) and spec[i + 1] == "-":
            for c in range(ord(spec[i]), ord(spec[i + 2]) + 1):
                out.append(chr(c))
            i += 3
        else:
            out.append(spec[i])
            i += 1
    return out


def tr(s, search, repl, flags=""):
    """Return (translated_string, count) mimicking Perl's tr///."""
    delete = "d" in flags
    squeeze = "s" in flags
    complement = "c" in flags

    sl = _expand(search)
    rl = _expand(repl)

    if complement:
        sset = set(sl)
        sl = [chr(c) for c in range(256) if chr(c) not in sset]

    mapping = {}
    deleted = set()
    if delete:
        for idx, ch in enumerate(sl):
            if idx < len(rl):
                mapping[ch] = rl[idx]
            else:
                deleted.add(ch)
    elif not rl:
        # no replacement list: identity (count / squeeze only)
        for ch in sl:
            mapping[ch] = ch
    else:
        for idx, ch in enumerate(sl):
            mapping[ch] = rl[idx] if idx < len(rl) else rl[-1]

    out = []
    count = 0
    prev_translated = False
    for ch in s:
        if ch in mapping:
            count += 1
            nc = mapping[ch]
            if squeeze and prev_translated and out and out[-1] == nc:
                continue
            out.append(nc)
            prev_translated = True
        elif ch in deleted:
            count += 1
            prev_translated = False
        else:
            out.append(ch)
            prev_translated = False
    return "".join(out), count


# ── basic translation ────────────────────────────────────────────────────────
s = "hello"
s, _ = tr(s, "a-z", "A-Z")
print(s)                          # HELLO

s = "WORLD"
s, _ = tr(s, "A-Z", "a-z")
print(s)                          # world

# single char
s = "banana"
s, _ = tr(s, "a", "o")
print(s)                          # bonono

# ── y/// alias ──────────────────────────────────────────────────────────────
s = "hello"
s, _ = tr(s, "a-z", "A-Z")
print(s)                          # HELLO

# ── return value (count of chars translated) ────────────────────────────────
s = "hello world"
_, count = tr(s, "a-z", "")
print(count)                      # 10

s = "aabbcc"
_, count = tr(s, "a", "a")
print(count)                      # 2

# ── delete flag /d ──────────────────────────────────────────────────────────
s = "hello world"
s, _ = tr(s, "aeiou", "", "d")
print(s)                          # hll wrld

s = "abc123def"
s, _ = tr(s, "0-9", "", "d")
print(s)                          # abcdef

# ── squeeze flag /s ─────────────────────────────────────────────────────────
s = "aaabbbccc"
s, _ = tr(s, "a-z", "a-z", "s")
print(s)                          # abc

s = "bookkeeper"
s, _ = tr(s, "a-z", "", "s")
print(s)                          # bokeper

# ── complement flag /c ──────────────────────────────────────────────────────
s = "abc123"
s, _ = tr(s, "a-z", "*", "c")
print(s)                          # abc***

# ── complement + delete /cd ─────────────────────────────────────────────────
s = "Hello, World! 123"
s, _ = tr(s, "a-zA-Z", "", "cd")
print(s)                          # HelloWorld

# ── complement + squeeze /cs ────────────────────────────────────────────────
s = "abc123def456ghi"
s, _ = tr(s, "a-z", " ", "cs")
print(s)                          # abc def ghi

# ── escape sequences in tr ──────────────────────────────────────────────────
s = "line1\nline2\nline3"
_, count = tr(s, "\n", "")
print(count)                      # 2

s = "col1\tcol2\tcol3"
s, _ = tr(s, "\t", "|")
print(s)                          # col1|col2|col3

# ── operating on $_ ─────────────────────────────────────────────────────────
_underscore = "Hello World"
_underscore, _ = tr(_underscore, "a-z", "A-Z")
print(_underscore)                # HELLO WORLD

# ── no replace list (count only, no mutation) ───────────────────────────────
s = "the cat sat on the mat"
_, count = tr(s, "aeiou", "")
print(count)                      # 6

# ── replicate last replacement char when replace list is shorter ─────────────
s = "abcdef"
s, _ = tr(s, "a-f", "xy")
print(s)                          # xyyyyy

# ── ROT13 ───────────────────────────────────────────────────────────────────
s = "Hello, World!"
s, _ = tr(s, "A-Za-z", "N-ZA-Mn-za-m")
print(s)                          # Uryyb, Jbeyq!

# re-apply ROT13 to decode
s, _ = tr(s, "A-Za-z", "N-ZA-Mn-za-m")
print(s)                          # Hello, World!

# ── tr inside a loop ────────────────────────────────────────────────────────
words = ["hello", "world", "perl"]
for w in words:
    w, _ = tr(w, "a-z", "A-Z")
    print(w)                      # HELLO / WORLD / PERL

# ── combined delete + squeeze (/ds) ─────────────────────────────────────────
s = "aaa1bbb2ccc"
s, _ = tr(s, "0-9", "", "ds")
print(s)                          # aaabbbccc

print("done")
