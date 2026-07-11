#!/usr/bin/perl
# Smoke test for `use constant` package scoping (D26: a constant declared
# inside an inlined module was exposed as a global bareword sub regardless
# of package/@EXPORT, so an unexported module constant silently leaked into
# the importer's namespace instead of being inaccessible).
# Fast, narrow coverage — see d26_const_scoping.pl for the in-depth suite.
# Deliberately no `use strict` — the check for "not visible" relies on real
# Perl's non-strict bareword-as-string fallback rather than a fatal error,
# so the script can keep running and report all checks.
use lib "tests/lib";
use D26Const;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# The original bug: a module's own @EXPORT-listed constant works unqualified.
check('smoke_exported_const_unqualified', EXPORTED_C() == 111);

# The original bug: a module constant NOT in @EXPORT must NOT leak into the
# importer's namespace — the bareword falls back to its own name as a string.
check('smoke_private_const_not_leaked', PRIV_C eq "PRIV_C");

# Explicit package qualification always reaches it regardless of export.
check('smoke_private_const_via_qualified_name', D26Const::PRIV_C() == 333);

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "d26_const_scoping_smoke_done\n";
