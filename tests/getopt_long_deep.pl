#!/usr/bin/perl
# Deep: Getopt::Long — found completely unimplemented during a real-world
# code survey (2026-09): GetOptions() silently resolved to nothing and
# always returned false, so every script using it (probably the single
# most common way real Perl CLI tools parse @ARGV — found broken in
# /usr/bin/debconf-escape on this system) fell straight into its own
# error-handling path no matter what flags were passed.
#
# Implementation: src/runtime.c perl_getopt_long() (with helpers
# getopt_parse_spec/getopt_find_spec/getopt_store), dispatched from
# src/codegen.cpp's "GetOptions"/"Getopt::Long::GetOptions" case, which
# builds a plain array of the raw call arguments (spec strings and ref
# targets — refs evaluate to real REF_SCALAR/REF_ARRAY/REF_HASH via the
# ordinary \$x/\@x/\%x codegen) and passes it plus the live @ARGV array
# (mutated in place to remove recognized options, matching real
# Getopt::Long's contract).
#
# Supports: boolean flags, alternate names ("foo|f"), negation ("foo!" /
# --no-foo), increment ("foo+"), typed values (=s/=i/=f), array-collecting
# (=s@) and hash-collecting (=s%) options, both calling conventions
# (GetOptions(spec=>ref,...) and GetOptions(\%opt, spec, ...)), long
# (--foo, --foo=val, --foo val) and short (-f, -f val) forms, "--"
# end-of-options, and unknown-option failure. Also verifies glued short
# values (-fVALUE) are correctly REJECTED, matching real (default-config)
# Getopt::Long's own surprising behavior.
#
# NOT supported (documented simplifications, not seen in the real-world
# scripts this was built against): bundled short options (-abc),
# optional-value (:s) treated as required, pass_through/gnu_getopt config.
use strict;
use warnings;
use Getopt::Long;

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

{
    @ARGV = ("--escape");
    my ($escape, $unescape) = (0, 0);
    my $ok = GetOptions("escape|e" => \$escape, "unescape|u" => \$unescape);
    check('long_alt_name', $ok && $escape == 1 && $unescape == 0);
}
{
    @ARGV = ("-u");
    my ($escape, $unescape) = (0, 0);
    GetOptions("escape|e" => \$escape, "unescape|u" => \$unescape);
    check('short_alt_name', $escape == 0 && $unescape == 1);
}
{
    @ARGV = ("--no-verbose");
    my $verbose = 1;
    GetOptions("verbose!" => \$verbose);
    check('negation', $verbose == 0);
}
{
    @ARGV = ("-v", "-v", "-v");
    my $verbose = 0;
    GetOptions("verbose|v+" => \$verbose);
    check('increment', $verbose == 3);
}
{
    @ARGV = ("--count=5", "--name", "foo", "--ratio", "2.5");
    my ($count, $name, $ratio) = (0, "", 0);
    GetOptions("count=i" => \$count, "name=s" => \$name, "ratio=f" => \$ratio);
    check('typed_values', $count == 5 && $name eq "foo" && $ratio == 2.5);
}
{
    @ARGV = ("--include=a", "--include=b", "--include=c");
    my @includes;
    GetOptions("include=s@" => \@includes);
    check('array_collect', "@includes" eq "a b c");
}
{
    @ARGV = ("--define", "x=1", "--define", "y=2");
    my %defines;
    GetOptions("define=s%" => \%defines);
    check('hash_collect', $defines{x} == 1 && $defines{y} == 2);
}
{
    @ARGV = ("--verbose", "--name", "Bob");
    my %opt;
    my $ok = GetOptions(\%opt, "verbose", "name=s");
    check('hashref_form', $ok && $opt{verbose} == 1 && $opt{name} eq "Bob");
}
{
    @ARGV = ("--bogus");
    my $v;
    my $ok = GetOptions("verbose" => \$v);
    check('unknown_option_fails', !$ok);
}
{
    @ARGV = ("--verbose", "--", "--not-an-option", "file.txt");
    my $verbose = 0;
    GetOptions("verbose" => \$verbose);
    check('double_dash_terminator', $verbose == 1 && "@ARGV" eq "--not-an-option file.txt");
}
{
    @ARGV = ("--verbose", "file1.txt", "file2.txt");
    my $verbose = 0;
    GetOptions("verbose" => \$verbose);
    check('leftover_argv', "@ARGV" eq "file1.txt file2.txt");
}
{
    # real (default-config) Getopt::Long rejects glued short values like
    # -fVALUE as an unknown option "fvalue" — not a perlc limitation,
    # verified to match real Getopt::Long's own behavior.
    @ARGV = ("-fVALUE");
    my $foo = "";
    my $ok = GetOptions("foo|f=s" => \$foo);
    check('short_glued_value_rejected', !$ok && $foo eq "");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "getopt_long_deep_done\n";
