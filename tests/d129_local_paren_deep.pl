#!/usr/bin/perl
# Deep test for D129: local(VAR, VAR, ...) = EXPR — parenthesized
# list-form local, including the single-variable case. The parser only
# ever expected a bare sigil-variable directly after `local`; a `(`
# was a hard parse error regardless of how many variables were inside.
# Fix desugars exactly like `my (...)`'s identical list form: a
# FlatBlock of per-variable local-decl statements followed by one
# list-assignment, reusing the same, already-tested codegen path.
use strict;
use warnings;

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# the exact real-world repro (Pod::Usage.pm, verbatim). NB: kept at
# true file scope, not nested in a bare `{ }` block — `our $x;` inside
# a nested block is a separate, unrelated, still-open bug found while
# writing this test (a sub referencing it then sees nothing), not
# D129's concern.
our $result;
sub foo { local($_) = shift; $result = $_; }
foo("hi");
check('single_var_underscore', $result eq "hi");

our $g = 10;
sub bump_g {
    local($g) = 99;
    return inner_g();
}
sub inner_g { return $g; }
check('single_named_var_dynamic_scope', bump_g() == 99);
check('single_named_var_restored', $g == 10);

our ($a, $b) = (1, 2);
sub multi_local {
    local($a, $b) = (7, 8);
    return "$a,$b";
}
check('multi_var_local', multi_local() eq "7,8");
check('multi_var_restored', "$a,$b" eq "1,2");

# local(@arr) / local(%hash) single-var array/hash forms
our @arr = (1, 2, 3);
sub local_array {
    local(@arr) = (9, 9);
    return scalar(@arr);
}
check('local_array_paren', local_array() == 2);
check('local_array_restored', scalar(@arr) == 3);

our %hash = (x => 1);
sub local_hash {
    local(%hash) = (y => 2);
    return join(",", %hash);
}
check('local_hash_paren', local_hash() eq "y,2");
check('local_hash_restored', join(",", %hash) eq "x,1");

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "d129_local_paren_done\n";
