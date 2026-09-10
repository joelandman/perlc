#!/usr/bin/perl
# Deep test for D109: s///'s REPLACEMENT text is parsed like a
# double-quoted string in real Perl — full variable interpolation, not
# just $0-$9/$& capture refs (which already worked via a separate, older
# fast path — src/runtime.c perl_regex_subst — that this fix must not
# disturb). Also covers the D109-widened finding that this is really a
# "the replacement isn't run through the same interpolation engine as
# other double-quoted text" bug, not an s///-specific one.
use strict;
use warnings;

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

{
    my $name = "World";
    my $s = "hello X";
    $s =~ s/X/$name/;
    check('scalar_interp', $s eq "hello World");
}
{
    my @arr = (1, 2, 3);
    my $s = "list: X";
    $s =~ s/X/@arr/;
    check('array_interp', $s eq "list: 1 2 3");
}
{
    # array element
    my @a = (10, 20, 30);
    my $s = "val:X";
    $s =~ s/X/$a[1]/;
    check('array_elem_interp', $s eq "val:20");
}
{
    # hash element
    my %h = (k => "V");
    my $s = "hh:X";
    $s =~ s/X/$h{k}/;
    check('hash_elem_interp', $s eq "hh:V");
}
{
    # /g with a named-variable replacement
    my $x = "a.b.c";
    my $sep = "-";
    $x =~ s/\./$sep/g;
    check('global_interp', $x eq "a-b-c");
}
{
    # escaped $ must not trigger interpolation (regression vs D107)
    my $s = "X";
    $s =~ s/X/\$name literally/;
    check('escaped_dollar_still_literal', $s eq "\$name literally");
}
{
    # capture refs alone must still use the old fast path unaffected
    # (no named variable trigger at all)
    my $s = "abc123";
    $s =~ s/(\d+)/[$1]/;
    check('capture_ref_unaffected', $s eq "abc[123]");
}
{
    # capture ref AND a named variable together in one replacement
    my $tag = "num";
    my $s = "abc123";
    $s =~ s/(\d+)/$tag=$1/;
    check('capture_plus_named_var', $s eq "abc" . "num=123");
}
{
    # $& (whole match) plus a named variable together
    my $pre = "<";
    my $s = "abc";
    $s =~ s/b/$pre$&>/;
    check('ampersand_plus_named_var', $s eq "a<b>c");
}
{
    # $@ (eval error variable) in replacement text
    eval { die "boom\n"; };
    my $s = "err:X";
    $s =~ s/X/$@/;
    check('dollar_at_interp', $s eq "err:boom\n");
}
{
    # /e path must be completely unaffected by this fix (different,
    # separate branch of the same case in codegen)
    my $s = "5";
    $s =~ s/5/2+3/e;
    check('eval_replacement_unaffected', $s eq "5");
}
{
    # deref scalar $$ref in replacement text
    my $name = "Bob";
    my $ref = \$name;
    my $s = "hi:X";
    $s =~ s/X/$$ref/;
    check('deref_scalar_interp', $s eq "hi:Bob");
}
{
    # plain text with no interpolation trigger at all — must be
    # completely unaffected (stays on the fast raw path)
    my $s = "plain X here";
    $s =~ s/X/\t/;
    check('plain_escape_only_unaffected', $s eq "plain \t here");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "d109_subst_interp_done\n";
