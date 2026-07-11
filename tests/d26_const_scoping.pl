#!/usr/bin/perl
# In-depth test suite for `use constant` package scoping.
#
# Root cause (D26): `inlineModules()` (main.cpp) textually rewrote any
# `use constant NAME => VAL` found anywhere in the recursively-inlined
# token stream — including inside `.pm` files pulled in via `use`/
# `require` — into a plain global `sub NAME { return VAL; }`, with no
# regard for which package the constant was declared in or whether the
# module actually exported it. A constant a module author intended as
# purely internal (never added to @EXPORT/@EXPORT_OK) silently became
# callable, unqualified, from any importer's namespace.
#
# Fixed by threading two things through inlineModules(): an isMainScript
# flag (true only for the outermost call, over the user's own script) and
# an explicitImportNames list (the qw(...) list from the `use Module
# qw(...)` call site that pulled in the module currently being scanned,
# empty when no explicit list was given). For constants found while
# isMainScript is false (i.e. inside an inlined module), the fix now:
#   - always emits a fully-qualified sub (Package::NAME), tracking
#     `package NAME;` statements as it scans so explicit qualification
#     always works, matching real Perl;
#   - additionally emits the unqualified/bareword-global sub only when
#     the name is actually visible: present in explicitImportNames when
#     an explicit list was given (an explicit list replaces the default
#     export set, it doesn't add to it — matching Exporter), otherwise
#     present in the module's own @EXPORT (never @EXPORT_OK by default,
#     matching real Perl: EXPORT_OK names require an explicit request).
# The main script's own top-level `use constant` (isMainScript true) is
# unchanged — no cross-package boundary applies there.
#
# This fix also resolved tests/fileops.pl's pre-existing harness failure:
# that file references MathOps.pm's PI/MAX constants as bare names, but
# neither is in MathOps.pm's @EXPORT or @EXPORT_OK — real Perl (with no
# `use strict` in that file) falls back to treating the unresolved
# barewords as literal strings ("PI"/"MAX"), which perlc's old leaking
# behavior didn't replicate (it printed the numeric values instead).
#
# Deliberately no `use strict` — several checks below rely on real Perl's
# non-strict bareword-as-string fallback for an unresolved bareword,
# rather than a fatal "Bareword not allowed" compile error, so the script
# can keep running and report every check.
use lib "tests/lib";
use D26Const;
use D26ConstOk qw(OK_ONLY);

use constant MAIN_CONST => 999;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: the original bug — default-exported (@EXPORT) constant ──────
# ── works unqualified after a plain `use Module;` ───────────────────────────
{
    check('default_export_const_unqualified', EXPORTED_C() == 111);
}

# ── Section 2: a constant NOT in @EXPORT or @EXPORT_OK must not leak ───────
{
    check('private_const_bareword_not_leaked', PRIV_C eq "PRIV_C");
}

# ── Section 3: private constants declared via the `use constant { ... }` ───
# ── hash form are scoped identically to the single-constant form ───────────
{
    check('private_hash_const_a_not_leaked', PRIV_HASH_A eq "PRIV_HASH_A");
    check('private_hash_const_b_not_leaked', PRIV_HASH_B eq "PRIV_HASH_B");
}

# ── Section 4: explicit package qualification always works, exported or ───
# ── not — real Perl never blocks fully-qualified access ────────────────────
{
    check('qualified_access_to_private_const', D26Const::PRIV_C() == 333);
    check('qualified_access_to_exported_const', D26Const::EXPORTED_C() == 111);
}

# ── Section 5: explicit `use Module qw(NAME)` grants access to an ─────────
# ── @EXPORT_OK name that the default (no-args) `use Module;` would not ─────
{
    check('explicit_import_ok_name', OK_ONLY() == 777);
    # PRIV_ONLY is in neither @EXPORT nor @EXPORT_OK for D26ConstOk, and
    # wasn't requested by the explicit qw() list above — still not visible.
    check('explicit_import_leaves_other_names_private', PRIV_ONLY eq "PRIV_ONLY");
    check('explicit_import_qualified_fallback', D26ConstOk::PRIV_ONLY() == 888);
}

# ── Section 6: regression — the main script's own `use constant` is a ──────
# ── file-scope declaration with no cross-package boundary, unaffected ──────
{
    check('main_script_const_unqualified', MAIN_CONST() == 999);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "d26_const_scoping_tests_done\n";
