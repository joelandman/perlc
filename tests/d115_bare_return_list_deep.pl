#!/usr/bin/perl
# Deep test for D115: bare `return;` (no expression) must yield an
# empty LIST in list context, not a 1-element (undef) list — checked
# via TWO separate, duplicate codegen sites that both needed the fix:
# case NK::Return (emitStmt, a bare return anywhere in a sub body) and
# an identical duplicate inside emitBlockLast (specifically when the
# bare return is the sub's last/only statement, the common case — this
# duplicate was still broken after fixing the first site alone).
# Scalar/void context must still get plain undef, unaffected.
use strict;
use warnings;

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

sub f_only_stmt { return; }
sub f_conditional { if (0) { return 5; } return; }

check('sole_statement_list_context', do { my @r = f_only_stmt(); scalar(@r) } == 0);
check('conditional_return_list_context', do { my @r = f_conditional(); scalar(@r) } == 0);

# scalar context: still plain undef
check('scalar_context_undef', !defined(scalar(f_only_stmt())));

# nested: a sub whose only work is returning another bare-return sub's
# list-context result
sub wrapper { my @x = f_only_stmt(); return @x; }
check('nested_propagation', do { my @r = wrapper(); scalar(@r) } == 0);

# boolean/if context (list assignment as a condition — must be false,
# matching an empty list's boolean-context count of 0). NB: `if (my
# @arr = EXPR)` — a single array variable declared inline as the
# condition — is a separate, unrelated, still-open parse gap (found
# while writing this test, not D115), so this uses a pre-declared
# array instead of that inline form.
my @cond_r;
if (@cond_r = f_only_stmt()) {
    check('boolean_context', 0);
} else {
    check('boolean_context', 1);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "d115_bare_return_list_done\n";
