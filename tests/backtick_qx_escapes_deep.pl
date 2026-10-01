#!/usr/bin/perl
# Deep test for D161: two related, pre-existing gaps in backtick/qx()
# command execution.
#
# 1. Backtick (`` `CMD` ``) string lexing was a bare raw character-copy
#    loop with ZERO escape processing of any kind — \$/\@ (meant to
#    protect a literal $/@ from Perl's own interpolation before the
#    text reaches the shell) passed through completely unhandled:
#    `` `echo "\$x hello"` `` both interpolated $x to its value AND
#    left a stray literal backslash in front of it, instead of real
#    Perl's \$x -> literal "$x" text for the shell to see. Fixed by
#    routing backtick lexing through the same readString()/
#    appendEscape() path "..." literals already use (src/lexer.cpp),
#    including the \x02 \$/\@-protection marker.
#
# 2. qx(...)/qx{...}/qx/.../ (real Perl's alternate spelling of
#    backticks, with flexible delimiters — not a separate feature, the
#    exact same command-execution operator) was not recognized AT ALL:
#    a hard parse error ("String found where operator expected"). Now
#    lexed exactly like backticks (reusing the same escape handling,
#    hence the same \$/\@ fix above), across every delimiter form real
#    Perl supports: qx(...), qx{...} (with nested-brace-depth
#    tracking), qx/.../, qx[...], qx<...>.
use strict;
use warnings;

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

my $name = "world";

# backtick: genuine interpolation still works
{
    my $r = `echo "hello $name"`;
    check('backtick_interpolation', $r eq "hello world\n");
}

# backtick: \$ protects from interpolation (the original repro)
{
    my $r = `echo "\$name hello"`;
    check('backtick_escaped_dollar', $r eq " hello\n");
}

# backtick: \@ protects from interpolation
{
    my @arr = (1, 2, 3);
    my $r = `echo "\@arr test"`;
    check('backtick_escaped_at', $r eq "\@arr test\n");
}

# backtick: mixed escaped + real interpolation in one string
{
    my $r = `echo "\$name and $name"`;
    check('backtick_mixed_escape_and_interp', $r eq " and world\n");
}

# backtick: plain escapes (tab) still work, unaffected by this fix
{
    my $r = `printf 'a\\tb\\n'`;
    check('backtick_plain_escapes_unaffected', $r eq "a\tb\n");
}

# qx(): genuine interpolation
{
    my $r = qx(echo "hello $name");
    check('qx_paren_interpolation', $r eq "hello world\n");
}

# qx(): escaped $ protects from interpolation
{
    my $r = qx(echo "\$name hello");
    check('qx_paren_escaped_dollar', $r eq " hello\n");
}

# qx{}: brace delimiter, including a nested brace in the command
{
    my $r = qx{echo "\$name hello"};
    check('qx_brace_escaped_dollar', $r eq " hello\n");
    my $r2 = qx{echo {a,b}};
    check('qx_brace_nested', $r2 eq "{a,b}\n");
}

# qx// and qx[] and qx<> delimiter forms
{
    my $r1 = qx/echo slash/;
    check('qx_slash_delim', $r1 eq "slash\n");
    my $r2 = qx[echo bracket];
    check('qx_bracket_delim', $r2 eq "bracket\n");
    my $r3 = qx<echo angle>;
    check('qx_angle_delim', $r3 eq "angle\n");
}

# regression: "qx" as a bareword hash key is unaffected
{
    my %h = (qx => 1);
    check('qx_as_hash_key_unaffected', $h{qx} == 1);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "backtick_qx_escapes_done\n";
