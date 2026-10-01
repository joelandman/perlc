# Deep test for D166: two separate, pre-existing parser gaps found
# via a real-script survey.
#
# 1. -t FILEHANDLE (isatty test) was not recognized at all: lowercase
#    't' was simply missing from the lexer's filetest-operator
#    character set (ftOps had uppercase 'T' for -T text-file test,
#    but not lowercase 't'), so `-t` never became a TK::FILETEST
#    token in the first place. Separately, a *bareword* filehandle
#    operand (`-t STDERR`, no sigil/parens) wasn't parseable even
#    once -t itself worked, since the filetest operand parser only
#    knew how to grab $var/"str"/$arr[i] shapes, not a standalone
#    bareword filehandle name. Found via a real dpkg-preconfigure
#    script: `-t STDERR` inside `($apt && @debs > 30 && -t STDERR)`.
#    Also fixed: bare `-t` (no filehandle at all) must test STDIN
#    specifically, per real Perl -- not $_ like every other bare
#    filetest -- and a bare filetest followed directly by `?`
#    (ternary) must not try to consume the `?` as an operand.
#
# 2. do { ... } while/until COND -- found via a real perlbug script:
#    `} while !((($alt) = grep(/^$alt/i, @alts)));`. The parser
#    unconditionally required a literal '(' right after while/until,
#    even though do{}while's condition is actually the same
#    unparenthesized statement-modifier `while EXPR`/`until EXPR`
#    form used everywhere else in this codebase (which already
#    allows an expression starting with any token, including a
#    leading '(' as ordinary grouping or a leading '!').
use POSIX qw(:fcntl_h);

# -t: bare (tests STDIN), with a bareword filehandle operand, and
# combined with other boolean operators (the real-world shape)
print "bare_t=", ((-t) ? "1" : "0"), "\n";
print "t_stdin=", ((-t STDIN) ? "1" : "0"), "\n";
print "t_stderr=", ((-t STDERR) ? "1" : "0"), "\n";
print "t_stdout=", ((-t STDOUT) ? "1" : "0"), "\n";
my $apt = 1;
my @debs = (1..35);
my $show_progress = ($apt && @debs > 30 && -t STDERR);
print "combined=", ($show_progress ? "1" : "0"), "\n";
print "ternary=", ((-t STDIN) ? "a" : "b"), "\n";

# regression: other filetest ops (parenthesized, bare-$_, chained,
# bareword-operand-adjacent code) must be completely unaffected
print "e1=", ((-e "/etc/passwd") ? "1" : "0"), "\n";
print "e2=", (-e "/etc/passwd" ? "1" : "0"), "\n";
print "d1=", (-d "/etc" ? "1" : "0"), "\n";
my $f = "/etc/passwd";
print "both=", ((-f $f && -r $f) ? "1" : "0"), "\n";
print "missing=", (-e "/nonexistent_xyz_abc" ? "1" : "0"), "\n";

# do{}while/until: unparenthesized, parenthesized, and the exact
# real-world negated-nested-parens shape
my $i = 0;
do { $i++; } while $i < 5;
print "while_noparen=$i\n";

my $j = 0;
do { $j++; } while ($j < 4);
print "while_paren=$j\n";

my $k = 0;
do { $k++; } until $k >= 3;
print "until_noparen=$k\n";

my $m = 0;
do { $m++; } until ($m >= 6);
print "until_paren=$m\n";

my @alts = ("foo", "bar", "baz");
my $alt = "xyz";
my $tries = 0;
do {
    $tries++;
    $alt = "bar";
} while !((($alt) = grep(/^$alt/i, @alts)));
print "realworld_alt=$alt realworld_tries=$tries\n";
