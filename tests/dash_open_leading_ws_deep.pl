# Regression: leading whitespace before the mode indicator must be stripped.
#
# perl's do_open skips leading blanks (space/tab) before the mode character,
# so " < -", " <-", " >>path", " >path", " +<path" all parse identically to
# their no-leading-space forms. The pre-fix perl_open2_fh matched s[0]
# directly, so a leading space fell through to a literal-filename open and
# failed -- that is the exact gap this guards against (it broke grog,
# shasum, and other tools that pass " < -"/" > -" to open()).
#
# STDIN is a fixed payload. perlc must:
#   * read it via a leading-ws read mode,
#   * append/truncate/update via leading-ws file modes (each on its own
#     target so the checks are independent). File targets are used rather
#     than a leading-ws ">" -> STDOUT to stay immune to STDOUT block
#     buffering ordering, which is an orthogonal issue.
use strict;
use warnings;

# --- 1. leading-ws read mode consumes STDIN like perl does ---
my $fh;
if (open($fh, " < -")) {
    my $buf = do { local $/; <$fh> };
    print "ws_read_len=", length($buf), "\n";
    print "ws_read=", defined($buf) ? $buf : "undef\n";
} else {
    print "ws_read_openfail: $!\n";
}

# --- 2. leading-ws append / truncate / update modes operate on files ---
my $ok = "";

my $appf = "/tmp/perlc_ws_app.dat";
ok_seed($appf, "ORIG");
ok_append($appf, "APP");
$ok = "APPEND_OK" if read_back($appf) eq "ORIGAPP";

my $trun = "/tmp/perlc_ws_trun.dat";
ok_seed($trun, "ORIG");
ok_truncate($trun, "NOW");
$ok .= ":TRUNCATE_OK" if read_back($trun) eq "NOW";

my $upd = "/tmp/perlc_ws_upd.dat";
ok_seed($upd, "ORIG");
ok_update($upd, "X");
$ok .= ":UPDATE_OK" if read_back($upd) eq "XRIG";

print "ws_modes=$ok\n";

sub ok_seed {
    my ($f, $orig) = @_;
    open(my $s, ">", $f) or die "$f seed: $!\n";
    print $s $orig;
    close($s);
}
sub ok_append {
    my ($f, $add) = @_;
    open(my $f_, " >>$f") or die "$f append: $!\n";
    print $f_ $add;
    close($f_);
}
sub ok_truncate {
    my ($f) = @_;
    ok_seed($f, "ORIG");
    open(my $f2, " >$f") or die "$f truncate: $!\n";
    print $f2 "NOW";
    close($f2);
}
sub ok_update {
    my ($f) = @_;
    ok_seed($f, "ORIG");
    open(my $f3, " +<$f") or die "$f update: $!\n";
    print $f3 "X";
    close($f3);
}
sub read_back {
    my ($f) = @_;
    open(my $r, "<", $f) or die "$f read: $!\n";
    local $/;
    my $d = <$r>;
    return defined($d) ? $d : "undef";
}
