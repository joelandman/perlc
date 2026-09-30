#!/usr/bin/perl
# Deep test for D159: split() in scalar context (`my $n = split ...`)
# never actually worked — emitExpr's NK::SplitFunc case (used whenever
# a scalar value is expected) returned the raw PerlArray* from
# perl_split/perl_split_regex directly, type-punned as a PerlValue*.
# Every consumer of that "value" (perl_assign, perl_clone, etc.) then
# read a PerlArray's fields as if they belonged to a PerlValue,
# producing silently wrong output (empty, when assigned to a `my`
# inside a sub — whatever codegen path that declaration happens to
# take doesn't crash on the type confusion) or segfaulting outright
# (assigned to a *file-scope* global scalar, whose assignment codegen
# path does dereference the bogus "value"). Fixed by converting to the
# field count via perl_array_len + perl_array_free, mirroring the
# identical scalar-context conversion `keys`/`values` already do.
use strict;
use warnings;

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# the exact file-scope repro that used to segfault
my $top_n = split(' ', "a b c d");
check('file_scope_scalar_split', $top_n == 4);

sub in_sub {
    my $n = split(' ', "one two three");
    return $n;
}
check('sub_scope_scalar_split', in_sub() == 3);

# string-separator (non-whitespace) scalar-context split
{
    my $n = split(':', "a:b:c:d:e");
    check('string_sep_scalar', $n == 5);
}

# regex-pattern scalar-context split
{
    my $n = split(/,\s*/, "a, b,c,  d");
    check('regex_sep_scalar', $n == 4);
}

# implicit $_ (D158) combined with scalar context (D159) together
{
    local $_ = "x y z";
    my $n = split ' ';
    check('implicit_underscore_scalar', $n == 3);
}

# LIMIT still applies correctly in scalar context
{
    my $n = split(/:/, "a:b:c:d", 2);
    check('limit_scalar_context', $n == 2);
}

# scalar-context split used directly in a boolean/numeric expression
# (not just assigned to a `my`) — exercises the value being consumed
# immediately rather than stored
{
    check('scalar_split_in_condition', (split(' ', "a b")) == 2 ? 1 : 0);
    my $doubled = 2 * split(' ', "p q r");
    check('scalar_split_in_arithmetic', $doubled == 6);
}

# regression: list context is completely unaffected by this fix
{
    my @f = split(' ', "a b c");
    check('list_context_unaffected', join(",", @f) eq "a,b,c");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "split_scalar_context_done\n";
