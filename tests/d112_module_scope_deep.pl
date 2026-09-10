#!/usr/bin/perl
# Deep test for D112: package-qualified global storage for file-scope
# `my`/`our` variables — a module's own subs must see the module's own
# file-scope variable (not the main script's, or another module's, same-
# named variable), from both directions (read and write), and normal
# same-package `our` cross-access must be unaffected by the fix.
use strict;
use warnings;
use lib 'tests/lib';
use D112Leaky;

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

my $counter = 7;

check('module_sees_own_var', D112Leaky::get() == 42);
check('main_var_unaffected', $counter == 7);

# module's own sub mutates the module's own var — must not touch main's
D112Leaky::bump();
check('module_bump_isolated', D112Leaky::get() == 43);
check('main_still_unaffected', $counter == 7);

# main script's own same-named var is independently mutable
$counter++;
check('main_bump_independent', $counter == 8);
check('module_still_43', D112Leaky::get() == 43);

# unrelated: our-declared globals in the SAME package still cross-access
# correctly (this already worked before D112 — regression check)
package D112Same;
our $shared = "same-pkg";
sub read_shared { return $shared; }
package main;
check('our_same_package_still_works', D112Same::read_shared() eq "same-pkg");

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "d112_module_scope_done\n";
