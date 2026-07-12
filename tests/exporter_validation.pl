#!/usr/bin/perl
# In-depth test suite for the D26 follow-up: `use Module qw(NAME)` now
# validates that NAME actually appears in the target module's @EXPORT
# or @EXPORT_OK, matching real Perl's Exporter (which fails a bogus
# request at compile time: "NAME" is not exported by the Module module,
# then "Can't continue after import errors").
#
# Root cause of the original gap: `inlineModules()` (main.cpp) populated
# `importMap[name] = modName + "::" + name` for every name in an
# explicit `use Module qw(...)` list unconditionally, with no check that
# the module actually exports it. Silently letting through an import
# request for a name the module never intended to export is a class of
# bug this validation is designed to catch at compile time, exactly
# where real Perl catches it.
#
# Fixed by validating each explicitly-requested name against the union
# of @EXPORT and @EXPORT_OK (scanned via the existing scanExports()
# helper) at both places import lists get processed: the first-load path
# (inside the `for (dir : searchDirs)` loop) and the already-loaded path
# (a second `use Module qw(...)` of a module `use`d earlier in the same
# file with a *different* list — previously unvalidated even after this
# fix's first pass, since that branch doesn't recompute `exports` at
# all; fixed by re-locating and re-scanning the file there too).
# Constant names (declared via `use constant` inside the module) aren't
# found by scanExports() at all — it only recognizes literal
# `our @EXPORT[_OK] = ...` arrays — so they're deliberately exempted
# from this check via `constMap`, which the pre-existing D26 fix already
# populates only for actually-visible constant names.
#
# IMPORTANT: since a *rejected* import must fail to compile, and
# tests/harness.sh always treats a compile failure as a test failure
# (there is no "expected to fail compilation" test category), this file
# only covers the *positive* path — every scenario here is a genuinely
# valid import that must keep working (no false-positive rejections).
# The negative path (a bad import name is correctly rejected, with real
# Perl's exact error text) was verified manually against both `perl` and
# `perlc` directly; see REMEDIATION.md's entry for this fix for the
# exact commands and side-by-side output compared.
use strict;
use warnings;
use lib "tests/lib";

# ── Section 1: explicit import of a name that's in the default @EXPORT ────
# ── list (not just imported by default — requested explicitly too) ─────────
use MathOps qw(add);

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

check('explicit_export_name', add(2, 3) == 5);

# ── Section 2: explicit import of @EXPORT_OK names (the common case — ─────
# ── these are never auto-imported, only available via an explicit list). ──
# ── MathOps was already loaded by Section 1's `use`, so this also ─────────
# ── exercises the "already loaded" re-import code path, which needed its ─
# ── own separate fix for validation to apply there too — and covers ───────
# ── multiple names, from @EXPORT_OK, in a single qw() list. ────────────────
use MathOps qw(subtract multiply);

check('export_ok_subtract', subtract(10, 4) == 6);
check('export_ok_multiply', multiply(6, 7) == 42);

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "exporter_validation_tests_done\n";
