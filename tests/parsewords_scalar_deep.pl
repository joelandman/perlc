#!/usr/bin/perl
# Deep test for D162: Text::ParseWords's scalar-context shellwords()/
# quotewords()/parse_line() produced a malformed LLVM module (an
# invalid-IR compile-time error, "Call parameter type does not match
# function signature!") rather than wrong output at runtime — found
# via a real /usr/sbin/pam_getenv compile. Root cause: perl_array_len
# already returns a boxed PerlValue* (its signature is
# `PerlValue *perl_array_len(PerlArray *a)`), but the scalar-context
# codegen then passed that PerlValue* straight into perl_alloc_int
# (which expects a raw i64) — a pointer-as-integer type confusion.
# Fixed by returning perl_array_len's result directly (plus
# perl_array_free on the now-unused array), the same scalar-context
# convention keys/values/split already use.
use strict;
use warnings;
use Text::ParseWords;

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# shellwords: scalar context returns the word count
{
    my $n = shellwords("a b c");
    check('shellwords_scalar_count', $n == 3);
}
{
    my $n = shellwords('foo "bar baz" qux');
    check('shellwords_scalar_quoted', $n == 3);
}

# quotewords: scalar context returns the word count
{
    my $n = quotewords(' ', 0, "x y z w");
    check('quotewords_scalar_count', $n == 4);
}

# parse_line: scalar context returns the word count
{
    my $n = parse_line(' ', 0, "p q");
    check('parse_line_scalar_count', $n == 2);
}

# regression: list context is completely unaffected by this fix
{
    my @w = shellwords("a b c");
    check('shellwords_list_unaffected', join(",", @w) eq "a,b,c");
    my @q = quotewords(' ', 0, "x y z");
    check('quotewords_list_unaffected', join(",", @q) eq "x,y,z");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "parsewords_scalar_done\n";
