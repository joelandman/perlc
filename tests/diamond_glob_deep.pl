# Deep test for D164: <PATTERN> diamond-glob syntax and the related
# scalar-context glob() iterator bug.
#
# 1. <PATTERN> (real Perl core syntax) wasn't recognized at all when
#    PATTERN contains anything other than a bare identifier/$var —
#    e.g. <*.c>, <path/*.txt> — the lexer only ever captured
#    [A-Za-z0-9_] between < and >, so this fell through to a bare '<'
#    token and a confusing downstream parse error. Found via a real
#    /usr/bin/callgrind_annotate script doing
#    `$input_file = (<callgrind.out*>)[0];`.
# 2. Found while fixing (1): scalar-context glob() (and so also
#    scalar-context <PATTERN>) was an outright pre-existing INFINITE
#    LOOP bug whenever at least one file matched — it called
#    perl_glob_val(pat) fresh every time and always returned match
#    index 0, instead of real Perl's one-match-per-call, then undef,
#    then restart (readdir-style) behavior. `while (my $f = glob(...))`
#    never advanced and never terminated.
my $dir = "/tmp/perlc_dglob_deep";
mkdir $dir;
for my $n (1..4) {
    open(my $fh, ">", "$dir/f$n.tmp") or die $!;
    close $fh;
}

# list context: all matches. NOTE: the <PATTERN> form's pattern text
# is a literal, non-interpolated string (a documented, scoped-out gap
# — real Perl DOES interpolate $dir here; perlc doesn't) so the path
# must be hardcoded, not built from $dir, inside every <...> below.
my @all = sort </tmp/perlc_dglob_deep/*.tmp>;
print "list_count=", scalar(@all), "\n";
print "list_names=", join(",", map { s{.*/}{}; $_ } @all), "\n";

# scalar context: one match per call, then undef, then restart —
# must terminate on its own (no "last if" safety net) to prove it's
# not the old infinite-loop bug.
my $n = 0;
while (my $f = </tmp/perlc_dglob_deep/*.tmp>) {
    $n++;
}
print "scalar_iter_count=$n\n";

# calling again after exhaustion restarts a fresh pass
my $again = 0;
while (my $f = </tmp/perlc_dglob_deep/*.tmp>) {
    $again++;
}
print "scalar_iter_again=$again\n";

# the exact real-world shape: (<PATTERN>)[0] list-slice
my $one = (</tmp/perlc_dglob_deep/*.tmp>)[0];
print "slice_defined=", (defined($one) ? "yes" : "no"), "\n";

# zero-match pattern: empty list, not an error
my @none = </tmp/perlc_dglob_deep/*.nomatch>;
print "empty_count=", scalar(@none), "\n";

# core glob() builtin itself must also terminate correctly now
my $m = 0;
while (my $g = glob("$dir/*.tmp")) {
    $m++;
}
print "glob_builtin_count=$m\n";

# regression: existing <$fh> / bareword-filehandle readline forms
# (pure-identifier text) must be completely unaffected
open(my $wfh, ">", "$dir/lines.txt") or die $!;
print $wfh "a\nb\nc\n";
close $wfh;
open(my $rfh, "<", "$dir/lines.txt") or die $!;
my @lines = <$rfh>;
print "fh_lines=", scalar(@lines), "\n";
close $rfh;

open(BAREFH, "<", "$dir/lines.txt") or die $!;
my @lines2 = <BAREFH>;
print "bare_fh_lines=", scalar(@lines2), "\n";
close BAREFH;

# regression: ordinary '<' comparisons (including ones followed by a
# later unrelated '>' on the same line) must still work as comparisons
my $a = 3; my $b = 5; my $c = 2;
print "cmp1=", (($a < $b) ? "1" : "0"), "\n";
print "cmp2=", (($a < $b && $c > 1) ? "1" : "0"), "\n";
my %h = (k => 10);
print "cmp3=", (($h{k} < 20 && $c > 1) ? "1" : "0"), "\n";

for my $n (1..4) { unlink "$dir/f$n.tmp"; }
unlink "$dir/lines.txt";
rmdir $dir;
