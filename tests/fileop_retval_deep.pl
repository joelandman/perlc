# D151 deep: return values of open/close/closedir/chdir/opendir in scalar,
# list and boolean context. open used to return the filehandle cell itself
# (GLOB(0x...)), close always returned 1, closedir undef, chdir/opendir 0.
use strict;
sub v { defined $_[0] ? "[$_[0]]" : "u" }
my $bad = "/nonexistent/perlc/x";
my $tmp = "/tmp/perlc_fileop_retval_$$";

# ---- open: 1 / undef, in every context
my $s = open(my $w, ">", $tmp);
print "open-scalar ", v($s), " ref=", (ref(\$s)), "\n";
my @l = (open(my $r1, "<", $tmp), open(my $r2, "<", $bad));
print "open-list ", join(",", map { v($_) } @l), " n=", scalar(@l), "\n";
print "open-bool ", (open(my $r3, "<", $tmp) ? "T" : "F"), (open(my $r4, "<", $bad) ? "T" : "F"), "\n";
my $cnt = 0;
for my $f ($tmp, $bad, $tmp) { $cnt++ if open(my $h, "<", $f) }
print "open-count $cnt\n";
my $buf = "";
my $m = open(my $mem, ">", \$buf);
print "open-inmem ", v($m), "\n";
print $w "line1\n";

# ---- close: true only for a handle that is actually open
my $cw = close($w);
my $cw2 = close($w);
print "close-open ", v($cw), " close-again ", v($cw2), "\n";
print "close-failed-open ", v(close($r4)), "\n";
print "close-read ", v(close($r1)), "\n";
print $mem "mem";
print "close-inmem ", v(close($mem)), " buf=$buf\n";
my $lines = "";
open(my $in, "<", $tmp) or die "reopen";
while (my $x = <$in>) { $lines .= $x }
close $in or print "never\n";
close $in and print "never\n";
print "read-back ", $lines;
my $st = close($in) ? "T" : "F";
print "close-closed-bool $st\n";
unless (close($r3)) { print "never\n" } else { print "close-r3 ok\n" }

# ---- opendir / closedir
my $od = opendir(my $dh, "/tmp");
my $odbad = opendir(my $dh2, $bad);
print "opendir ", v($od), " ", v($odbad), "\n";
print "closedir ", v(closedir($dh)), " again ", v(closedir($dh)), "\n";
opendir(my $dh3, $bad) or print "opendir-or-ran\n";

# ---- chdir
my $cb = chdir($bad);
print "chdir-bad ", v($cb), " len=", length($cb), "\n";
print "chdir-ok ", v(chdir("/tmp")), "\n";
chdir $bad or print "chdir-or-ran\n";

# ---- mkdir / rmdir / unlink (already correct; guard against regressions)
print "mkdir ", v(mkdir("$tmp.d")), v(mkdir("$tmp.d")), "\n";
print "rmdir ", v(rmdir("$tmp.d")), v(rmdir("$tmp.d")), "\n";
print "unlink ", v(unlink($tmp)), v(unlink($tmp)), "\n";

# ---- unless ... elsif ... else (was a parse error)
for my $k (0, 1, 2) {
    unless ($k) { print "k=$k unless\n" }
    elsif ($k == 1) { print "k=$k elsif\n" }
    else { print "k=$k else\n" }
}
