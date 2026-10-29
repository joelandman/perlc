# Design Document: Perl `format` and `write` Support for perlc

**Date:** 2026-10-29
**Status:** Draft
**Target:** perlc (Perl→LLVM AOT compiler, C++17 + LLVM 18)

---

## 1. Summary of Perl `format`/`write` Syntax and Semantics

### 1.1 Declaration

```perl
format NAME =
    picture_line_1
    picture_line_2
    .
```

`NAME` defaults to the currently selected filehandle (typically `STDOUT`).
The declaration is a **compile-time** construct — the format is parsed once,
not at runtime. The `.` on its own line terminates the format body.

### 1.2 Picture Lines

Picture lines define the output layout using format specifiers:

| Specifier | Meaning |
|-----------|---------|
| `@<<<<<<` | Left-aligned string field, 7 chars wide |
| `@>>>>>>` | Right-aligned string field, 7 chars wide |
| `@|||||||` | Centered string field (alternating `\|` and space) |
| `^<<<<` | Multi-line string field, truncates at N lines (N = char count) |
| `~~` | Scalar expression — evaluated and stringified at runtime |
| `~~ @arr` | List expression — iterates over array elements |
| `.` | End of picture (also terminates format body) |

Each picture line becomes one output line when `write` is called.
`@<<<<<` pads/truncates to exactly the field width. `^<<<<` outputs up
to N lines of the string (one line of picture per line of text). `~~`
evaluates a scalar expression once per `write` call.

### 1.3 `write` Statement

```perl
write;                              # write to selected FH
write NAME;                         # write to named FH
write $fh;                          # write to variable FH
write STDOUT;
write LOGFILE;
```

`write` evaluates the format associated with the target filehandle,
producing one page of output. Each picture line is evaluated left-to-right:
- `@<<<<<<` / `@>>>>>>` / `@|||||||` — format the next argument as a
  string in the specified field
- `^<<<<` — outputs the next argument one line at a time, up to N lines
- `~~` — evaluates the expression, stringifies it, and outputs it

After each picture line is emitted, the filehandle's record separator
(`$\`) is appended (if defined). After a page is complete, a top-of-page
format is emitted (if one exists and `$-` has reached `format_lines_per_page`).

### 1.4 Associated Special Variables

| Variable | Role |
|----------|------|
| `$~` | Name of the format for the currently selected FH (default: same as FH name) |
| `$^` | Name of the top-of-page format |
| `$-` | Number of lines remaining on the current page (default: `format_lines_per_page`, default 60) |
| `$%` | Current page number |
| `$^A` | Format accumulator (used internally by Perl, not needed for our minimal implementation) |
| `$/` | Format line-break characters (only relevant for complex wrapping) |
| Select FH | `select(FH)` changes the default output FH for `write` |

### 1.5 Top-of-Page Format

```perl
format STDOUT_TOP =
    Header line
    .
```

A `_TOP` suffixed format name is the top-of-page format for the given
format name. It is emitted before the first page of output.

---

## 2. Analysis of Affected Scripts

### 2.1 `/usr/bin/ucfq` — ucfq (Priority: important)

**Format declarations:** 2 formats

#### Format `STDOUT_TOP` (line 298)
```perl
format STDOUT_TOP =
Configuration file                            Package             Exists Changed
.
```
- No picture specifiers — just a static header line
- Acts as a one-time page header

#### Format `STDOUT` (line 302)
```perl
format STDOUT =
@<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<< @<<<<<<<<<<<<<<<<<< @|||   @|||
$out_pkg,                                    $out_pkg,           $there,$mod
.
```
- 4 fields: left-aligned string (49 chars), left-aligned string (15 chars),
  centered field, centered field
- Uses 4 global variables: `$out_pkg`, `$out_file`, `$there`, `$mod`
- These are set in the `report()` sub before each `write` call

**`write` call:** 1 call (line 316)
```perl
write;   # implicitly writes to selected FH (STDOUT)
```
Inside a `for` loop iterating over files — each iteration emits one
formatted line.

**What the output looks like:**
```
Configuration file                            Package             Exists Changed
/var/lib/ucf/registry                         ucf                 Yes    No
```

### 2.2 `/usr/bin/perlbug` — perlbug / perlthanks

**Format declarations:** 1 format (line 1216)

#### Format `STDOUT` (line 1216)
```perl
format STDOUT =
^<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<< ~~
$_
.
```
- 1 field: `^<<...` (100 `<<` characters = 100-line multi-line field)
- 1 expression: `~~` followed by `$_`
- This is the **`paraprint`** helper function (line 975) that wraps
  paragraphs for display

**`write` call:** 1 call (line 979)
```perl
write;   # inside paraprint, writes $_ formatted
```

**How it works:** `paraprint` splits text into paragraphs (double-newline
delimiter), then for each paragraph:
1. Normalizes whitespace (replaces newlines with spaces)
2. Calls `write` which emits the paragraph through the `^<<...` field
3. The `^<<...` field outputs up to 100 lines of the string (effectively
   all of it since paragraphs are short)
4. `~~` evaluates `$_` and stringifies it

**What the output looks like:** Paragraphs wrapped to 100 columns, one
paragraph per `write` call.

### 2.3 `/usr/bin/perlthanks` — perlthanks

Identical to `perlbug` — same source file, same format, same `paraprint`
function. The format and `write` usage are byte-for-byte the same.

### 2.4 Summary of Required Features

| Feature | ucfq | perlbug/perlthanks | Needed? |
|---------|------|---------------------|---------|
| `format NAME =` | Yes | Yes | Yes |
| `format STDOUT_TOP =` (top-of-page) | Yes | No | Yes |
| `@<<<<<<` (left-aligned) | Yes | No | Yes |
| `@>>>>>>` (right-aligned) | No | No | No |
| `@|||||||` (centered) | Yes | No | Yes |
| `^<<<<` (multi-line) | No | Yes | Yes |
| `~~` (scalar expr) | No | Yes | Yes |
| `~~ @arr` (list expr) | No | No | No |
| `write;` (implicit FH) | Yes | Yes | Yes |
| `write NAME;` | No | No | No |
| `write $fh;` | No | No | No |
| `select(FH)` | No | No | No |
| Top-of-page format | Yes | No | Yes |

**All three scripts use only:**
1. `format` declaration with static picture lines
2. `@<<<<<<` (left-aligned string)
3. `@|||||||` (centered string)
4. `^<<<<` (multi-line string, one char = one output line)
5. `~~` (scalar expression evaluation + stringify)
6. `write;` (implicit selected FH)
7. Top-of-page format (`_TOP`)

No right-aligned fields, no `~~ @arr` list iteration, no `select()`,
no `write FILEHANDLE`.

---

## 3. Implementation Approaches

### 3.1 Approach A: Minimal Rewrite (Recommended for MVP)

**Idea:** Convert `format`/`write` to equivalent `print`/`printf` calls
at the parser level. This is a source-to-source transformation that
reuses the existing `print` codegen path.

**How it works:**

At parse time, when the parser encounters a `format` declaration, it:
1. Stores the format's picture lines and variable references
2. Converts the format into a synthetic subroutine that, when called,
   emits the equivalent `print` statements

For example, `ucfq`'s format:
```perl
format STDOUT =
@<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<< @<<<<<<<<<<<<<<<<<< @|||   @|||
$out_pkg, $out_file, $there, $mod
.
```

Gets transformed to something like:
```perl
# Synthetic sub generated by the compiler
sub __perlc_format_STDOUT {
    my ($out_pkg, $out_file, $there, $mod) = @_;
    print sprintf("%-49s %-15s %-3s %-3s\n",
                  $out_pkg, $out_file, $there, $mod);
}
```

And `write;` becomes:
```perl
__perlc_format_STDOUT($out_pkg, $out_file, $there, $mod);
```

For `perlbug`'s format:
```perl
format STDOUT =
^<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<< ~~
$_
.
```

Gets transformed to:
```perl
sub __perlc_format_STDOUT {
    my ($_) = @_;
    # ^<<100 means: output the string, one line per character
    # (up to 100 lines). For short strings, just print with newline.
    my $wrapped = wrap_paragraph($_);  # simple line-wrap helper
    print $wrapped;
}
```

And `paraprint`'s `write;` becomes:
```perl
__perlc_format_STDOUT($_);
```

**Pros:**
- Minimal code changes: only parser-level transformation
- Reuses existing `print`/`sprintf` codegen (already implemented)
- No new runtime functions needed for the common cases
- No new AST node types (or very few)
- No new lexer tokens (format/write are keywords already)
- Can be implemented as a single parser pass that rewrites the AST
- Fast to implement (days, not weeks)

**Cons:**
- Not a complete implementation — edge cases may differ
- No true page handling (top-of-page format needs special treatment)
- Multi-line `^<<<<` field with actual multi-line strings needs a
  line-wrapping helper
- `$-`, `$%`, `$~` are not fully modeled (top-of-page only)
- `~~ @arr` list iteration not supported (not needed by current scripts)

### 3.2 Approach B: Full Native Implementation

**Idea:** Implement `format`/`write` as first-class language features
with proper AST nodes, runtime support, and codegen.

**How it works:**

#### Lexer changes:
- Add `TK::FORMAT` keyword token
- Add `TK::WRITE` keyword token
- Picture line parsing: recognize `@<<`, `@>>`, `@|||`, `^<<`, `~~`
  as part of the format body (not as operators)
- The `.` on its own line terminates the format

#### AST changes:
- New node types:
  - `FormatDecl` — format name + picture lines + variable list
  - `PictureField` — type (left_align, right_align, center, multi_line)
    + width + expression (for `~~`)
  - `WriteStmt` — target FH (optional, defaults to selected)

#### Runtime changes:
- New runtime functions in `runtime.c`:
  ```c
  void perl_format_write(PerlValue *fh, const char *format_name);
  void perl_format_select_fh(PerlValue *fh);
  PerlValue *perl_format_get_dollar_dash(void);    // $-
  PerlValue *perl_format_get_dollar_pct(void);     // $%
  PerlValue *perl_format_get_dollar_tilde(void);   // $~
  PerlValue *perl_format_get_dollar_caret(void);   // $^
  PerlValue *perl_format_get_dollar_a(void);       // $^A
  void perl_format_register(const char *name, const char *picture);
  ```
- Format picture lines are stored as a process-wide registry at startup
- `write` dispatches to the format's picture lines, evaluating each field

#### Codegen changes:
- `emitStmt` handles `FormatDecl` by registering the format at program init
- `emitStmt` handles `WriteStmt` by calling `perl_format_write`
- Each picture field is evaluated and passed to the runtime

**Pros:**
- Complete, spec-compliant implementation
- Supports all picture types including `~~ @arr`
- Proper page handling, top-of-page formats
- `$-`, `$%`, `$~`, `$^`, `$^A` all work correctly
- Future scripts can use any format feature

**Cons:**
- Significant implementation effort (weeks)
- New runtime functions (C code in `runtime.c`)
- New AST node types and parser grammar
- More code to test and maintain
- Risk of introducing bugs in a complex feature

### 3.3 Approach C: Hybrid (Minimal AST + Targeted Runtime)

**Idea:** Add minimal AST nodes for `format`/`write` but implement the
picture-line evaluation mostly in the runtime, not the codegen.

**How it works:**

- Parser recognizes `format`/`write` syntax and builds AST nodes
- Format picture lines are stored as raw text strings in a format registry
- At runtime, `perl_format_write` receives the format name and evaluates
  each picture line:
  - `@<<<<<<` → `sprintf("%-Ns", value)`
  - `@|||||||` → center the string in N chars
  - `^<<<<` → line-wrap the string to N columns, output up to N lines
  - `~~ expr` → evaluate the expression, stringify, output

The runtime handles the formatting logic; codegen just passes the
evaluated values to the runtime function.

**Pros:**
- Less codegen complexity than Approach B
- Runtime handles formatting (one place to fix bugs)
- Supports all picture types without codegen changes
- Good balance of completeness vs effort

**Cons:**
- Runtime formatting is slower than compile-time `sprintf`
- Still needs some runtime functions and AST nodes
- Format picture lines must be stored and parsed at runtime

---

## 4. Recommended Approach: A (Minimal Rewrite)

### 4.1 Rationale

1. **Scope fits the need perfectly.** All three affected scripts use a
   tiny subset of format/write features. No right-aligned fields, no
   `~~ @arr` list iteration, no `select()`, no dynamic FH in `write`.

2. **Minimal code changes.** Approach A requires:
   - Parser changes only (a new statement handler for `format`)
   - AST support for format declarations (2-3 new node types)
   - No new runtime functions (reuses `print`/`sprintf`)
   - No lexer changes (keywords already exist)

3. **Fast to implement and test.** The transformation is mechanical:
   - Parse the format body as raw text
   - Convert picture specifiers to `sprintf` format strings
   - Convert `~~` expressions to arguments
   - Convert `write` calls to subroutine calls

4. **Lower risk.** No new runtime C code means no new memory-management
   or threading bugs. The transformation is purely at the AST level,
   where existing patterns are well-understood.

5. **Incremental.** Approach A can be extended to Approach B later
   if more complex format usage is encountered. The AST nodes defined
   for Approach A are the same ones Approach B would need.

### 4.2 What Approach A Does NOT Support (and why that's OK)

| Feature | Reason for exclusion |
|---------|---------------------|
| `@>>>>>>` (right-aligned) | Not used by any of the 3 scripts |
| `~~ @arr` (list iteration) | Not used by any of the 3 scripts |
| `write FILEHANDLE` | Not used by any of the 3 scripts |
| `select(FH)` | Not used by any of the 3 scripts |
| True page handling ($-) | Top-of-page is enough for ucfq |
| `$^A` accumulator | Not used by any of the 3 scripts |
| Dynamic format names ($~) | Not used by any of the 3 scripts |

---

## 5. Estimated Effort

### 5.1 Lexer (0 hours)
No changes needed. `format` and `write` can be treated as keywords
(like `print`/`say`) and recognized as statement starters. The
picture line content is just text within the format body — no new
token types needed.

### 5.2 Parser (~16 hours)

**Format declaration parsing (~8 hours):**
- New `parseFormat()` method in `Parser`
- Recognizes `format IDENT =` then reads picture lines until `.`
- Stores format name, picture lines, and variable references
- Builds a `FormatDecl` AST node

**`write` statement parsing (~4 hours):**
- New `parseWrite()` method
- Handles `write;`, `write NAME;`, `write $fh;`
- Builds a `WriteStmt` AST node

**Transformation logic (~4 hours):**
- For each `FormatDecl`, generate a synthetic sub that:
  - Takes the referenced variables as parameters
  - Emits `print sprintf(...)` for each picture line
- For each `WriteStmt`, emit a call to the synthetic sub

### 5.3 AST (~2 hours)

Add 2-3 new node types to `ast.h`:
```cpp
FormatDecl,    // format NAME = ...
WriteStmt,     // write; / write NAME;
PictureField,  // optional: represents one picture-line field
```

Each `FormatDecl` holds:
- `sval`: format name (e.g., "STDOUT")
- `args`: list of variable names referenced in picture lines
- `body`: the picture lines (stored as raw text or parsed fields)

### 5.4 Codegen (~8 hours)

**FormatDecl emission (~4 hours):**
- Generate a synthetic subroutine (like existing `AnonSub` handling)
- Inside the sub, emit `print sprintf(...)` calls for each picture line
- Register the sub in the module's function table

**WriteStmt emission (~4 hours):**
- Resolve the target filehandle (default to selected FH)
- Evaluate the variable arguments
- Call the synthetic format sub

### 5.5 Runtime (0 hours)
No new runtime functions needed. The approach reuses existing
`perl_print`, `perl_sprintf`, and `perl_print_fh` functions.

### 5.6 Total: ~26 hours (3-4 working days)

### 5.7 Testing (~8 hours)
- Smoke tests for each of the 3 scripts
- Deep tests covering edge cases:
  - Empty format body
  - Format with only `~~` expressions
  - Multi-line `^<<` with short strings
  - Top-of-page format with no body format
  - Multiple formats in one file
  - Format with variables from different packages

---

## 6. Incremental Implementation Plan

### Phase 1: Core Format Declaration + Basic Write (~2 days)

**Goal:** Compile `ucfq` successfully.

1. **AST:** Add `FormatDecl` and `WriteStmt` node types to `ast.h`
2. **Parser:** Implement `parseFormat()` and `parseWrite()`
   - Parse `format NAME =` ... `.`
   - Parse `write;` (implicit FH)
   - Store format name and picture line text
3. **Codegen:** Generate synthetic sub for each format
   - Convert `@<<<<<<` to `sprintf("%-Ns", ...)`
   - Convert `@|||||||` to a centering helper call
   - Convert `~~ expr` to the evaluated expression argument
4. **Test:** Compile and run `ucfq`

### Phase 2: Top-of-Page Formats (~0.5 days)

**Goal:** Handle ucfq's `format STDOUT_TOP =` header.

1. **Parser:** Recognize `_TOP` suffix in format names
2. **Codegen:** Emit the top-of-page format sub and call it once
   before the main format sub (at program start)
3. **Test:** Verify ucfq output matches real Perl

### Phase 3: perlbug/perlthanks Support (~1 day)

**Goal:** Compile `perlbug` and `perlthanks` successfully.

1. **Parser/Codegen:** Implement `^<<<<` (multi-line field) handling
   - For short strings: just print the string with newlines
   - For long strings: line-wrap to the field width
   - The `~~` expression after `^<<` provides the string value
2. **Codegen:** Handle `paraprint`'s `write;` inside a loop
3. **Test:** Compile and run both scripts

### Phase 4: Edge Cases & Hardening (~1 day)

**Goal:** Handle edge cases and ensure correctness.

1. **Parser:** Handle formats with no variables (static output)
2. **Codegen:** Handle `write` on non-STDOUT filehandles
   (even if not currently needed, for future compatibility)
3. **Test:** Add comprehensive smoke + deep tests

### Phase 5: Future Extensions (Post-MVP)

If more complex format usage is discovered:

1. **Extend parser:** Add `@>>>>>>` (right-aligned), `~~ @arr` (list)
2. **Extend runtime:** Add `perl_format_select_fh`, page handling
3. **Full spec compliance:** Support `$-`, `$%`, `$~`, `$^`, `$^A`

---

## 7. Detailed Design: Approach A Transformation

### 7.1 Format Declaration AST

```cpp
// In ast.h, add to NK enum:
FormatDecl,    // format NAME = ... .
WriteStmt,     // write; / write NAME;
```

`FormatDecl` node:
- `sval`: format name (e.g., "STDOUT")
- `args`: variable names used in picture lines (as ScalarVar nodes)
- `body`: raw picture line text (stored as `std::string` in `sval`
  of a synthetic child, or as a separate field in the Node struct)

`WriteStmt` node:
- `left`: optional target FH expression (nullptr = selected FH)
- `args`: variable values to pass to the format sub

### 7.2 Parser Grammar

```
parseStmt():
    if check(TK::KW_FORMAT): return parseFormat()
    if check(TK::KW_WRITE):  return parseWrite()
    ...

parseFormat():
    consume(KW_FORMAT)
    format_name = consume(IDENT).text
    consume('=')
    picture_lines = []
    while not check('.'):
        line = read_until_newline()
        picture_lines.push(line)
    consume('.')
    // Parse picture lines to extract variable references
    variables = extractVariables(picture_lines)
    return makeFormatDecl(format_name, variables, picture_lines)

parseWrite():
    consume(KW_WRITE)
    if check(IDENT) or check(SCALAR):
        fh = parseExpr()
    else:
        fh = nullptr  // selected FH
    consume(';')  // optional
    return makeWriteStmt(fh)
```

### 7.3 Picture Line to sprintf Conversion

**`@<<<<<<` (left-aligned, N chars):**
```
@<<<<<<  →  sprintf("%-Ns", value)
@<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<  →  sprintf("%-49s", value)
```

**`@|||||||` (centered, N chars):**
```
@|||||||  →  center_string(value, N)
```
Where `center_string` is a simple helper:
```c
// In runtime.c or inline in codegen
char *center_string(const char *s, int width) {
    int len = strlen(s);
    int pad = (width - len) / 2;
    if (pad < 0) return strdup(s);  // truncate
    char *result = malloc(width + 1);
    memset(result, ' ', width);
    memcpy(result + pad, s, len);
    result[width] = '\0';
    return result;
}
```

**`^<<<<` (multi-line, N chars = N output lines):**
```
^<<<<<<<<... (100 chars)  →  output string, one line per character, up to 100 lines
```

For the `perlbug` case, the string is short (a paragraph), so:
```
^<<100 + "text"  →  print("text")
```
The `^<<` field width only matters for strings longer than the width.

**`~~` (scalar expression):**
```
~~ $_  →  evaluate $_, stringify
~~ $var  →  evaluate $var, stringify
```

**Mixed picture line:**
```
@<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<< @<<<<<<<<<<<<<<<<<< @|||   @|||
$out_pkg,                                    $out_file,           $there,$mod
```
Converts to:
```perl
print sprintf("%-49s %-15s %s %s\n", $out_pkg, $out_file, $there, $mod);
```
(where `%s` for centered fields is a simplification — proper centering
needs a helper)

### 7.4 Synthetic Sub Generation

For each `FormatDecl`, generate:
```llvm
define void @__perlc_format_STDOUT(PerlValue* $out_pkg, PerlValue* $out_file,
                                   PerlValue* $there, PerlValue* $mod) {
    ; evaluate arguments, stringify
    ; call perl_sprintf with format string
    ; call perl_print_fh with selected FH
    ret void
}
```

For `write;` (implicit FH):
```llvm
call void @__perlc_format_STDOUT($out_pkg_pv, $out_file_pv, $there_pv, $mod_pv)
```

For `write LOGFILE;`:
```llvm
; get LOGFILE fh value
; call format sub with fh context
```

### 7.5 Top-of-Page Format

For `format STDOUT_TOP =`:
```
format STDOUT_TOP =
Configuration file                            Package             Exists Changed
.
```

Generate:
```perl
sub __perlc_format_STDOUT_TOP {
    print "Configuration file                            Package             Exists Changed\n";
}
```

Call at program start (before the main loop):
```perl
__perlc_format_STDOUT_TOP();
```

### 7.6 perlbug `paraprint` Transformation

Original:
```perl
sub paraprint {
    my @paragraphs = split /\n{2,}/, "@_";
    for (@paragraphs) {   # implicit local $_
        s/(\S)\s*\n/$1 /g;
        write;
        print "\n";
    }
}
```

Transformed:
```perl
sub paraprint {
    my @paragraphs = split /\n{2,}/, "@_";
    for (@paragraphs) {
        s/(\S)\s*\n/$1 /g;
        # write; → __perlc_format_STDOUT($_)
        __perlc_format_STDOUT($_);
        print "\n";
    }
}
```

Where `__perlc_format_STDOUT` is:
```perl
sub __perlc_format_STDOUT {
    my ($text) = @_;
    # ^<<100: output up to 100 lines of text
    # For short text, just print it
    print $text;
}
```

---

## 8. Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Picture line parsing is ambiguous | Low | Medium | Strict grammar: `@`, `^`, `~~` only valid in format body |
| Variable capture across packages | Medium | Medium | Track package context during format parsing |
| `^<<` multi-line wrapping differs from Perl | Medium | Low | Test against real Perl output |
| Top-of-page format ordering | Low | Low | Emit TOP format at program start |
| Performance: sprintf vs native format | Low | Low | sprintf is fast enough for these use cases |
| Future scripts need more features | Medium | Low | AST nodes designed for extensibility |

---

## 9. Appendix: Real Perl Output Samples

### 9.1 ucfq Output

```
Configuration file                            Package             Exists Changed
/var/lib/ucf/registry                         ucf                 Yes    No
/var/lib/ucf/hashfile                         ucf                 Yes    No
```

### 9.2 paraprint Output (from perlbug)

```
This program provides an easy way to generate a bug report for the core
perl distribution (along with tests or patches).  To send a thank-you
note to perl-thanks@perl.org instead of a bug report, please run
'perlthanks'.

The GitHub issue tracker at https://github.com/Perl/perl5/issues is the
best place to submit your report so it can be tracked and resolved.

Please do not use perlbug to report bugs in perl modules from CPAN.
```

Each paragraph is output through the `^<<100` field, which wraps to
100 columns.

---

## 10. Appendix: Files to Modify

| File | Changes |
|------|---------|
| `src/ast.h` | Add `FormatDecl`, `WriteStmt` to NK enum; add `body` field to Node |
| `src/parser.cpp` | Add `parseFormat()`, `parseWrite()`, picture-line analyzer |
| `src/parser.h` | Declare new parser methods |
| `src/codegen.cpp` | Add `emitFormatDecl()`, `emitWriteStmt()` |
| `src/runtime.c` | Add `center_string()` helper (optional, or inline in codegen) |
| `tests/format_{smoke,deep}.pl` | New test file |
| `tests/realworld/baseline.txt` | Update: ucfq, perlbug, perlthanks → ok |
