# D152 deep: bareword filehandles in close/eof/tell/binmode/fileno —
# never opened, opened later in the source, reopened, closed twice.
use strict;
sub v { defined $_[0] ? "[$_[0]]" : "u" }
my $tmp = "/tmp/perlc_bareword_fh_$$";

# used before its open appears in the source
sub finish { return close(LATER) ? "closed" : "not-open" }
print "before-open: ", finish(), "\n";
open(LATER, ">", $tmp) or die "open: $!";
print LATER "alpha\nbeta\n";
print "fileno-open: ", (fileno(LATER) > 2 ? "fd" : "bad"), "\n";
print "tell-after-write: ", tell(LATER), "\n";
print "after-open: ", finish(), "\n";
print "after-close: ", finish(), "\n";

# reopen for reading via the same bareword
open(LATER, "<", $tmp) or die "reopen: $!";
binmode LATER or die "binmode";
my $first = <LATER>;
print "first=$first";
print "eof-mid: ", (eof(LATER) ? "T" : "F"), " tell=", tell(LATER), "\n";
my $second = <LATER>;
print "second=$second";
print "eof-end: ", (eof(LATER) ? "T" : "F"), "\n";
close LATER or die "close";
print "closed-twice: ", v(close(LATER)), "\n";
print "eof-closed: ", (eof(LATER) ? "T" : "F"), " fileno-closed: ", v(fileno(LATER)), "\n";

# never opened, in every position
print "never: ", v(close(NEVER)), " ", (eof(NEVER) ? "T" : "F"), " ", tell(NEVER),
      " ", v(fileno(NEVER)), " ", v(binmode(NEVER)), "\n";
close NEVER or print "close-or-ran\n";
unless (close(NEVER)) { print "unless-close\n" }

# lexical handles and STD handles still work
open(my $lex, "<", $tmp) or die;
print "lex: ", (fileno($lex) > 2 ? "fd" : "bad"), " ", tell($lex), " ", v(close($lex)), "\n";
print "std: ", fileno(STDIN), fileno(STDOUT), fileno(STDERR), "\n";
unlink $tmp;

# eof() is true right after the last line is read (it used to need one more
# read, so `while (!eof)` loops ran an extra, undef iteration)
my $data = "one\ntwo\nthree\n";
open(my $m, "<", \$data) or die;
my @got;
while (!eof($m)) { my $l = <$m>; push @got, defined $l ? $l : "UNDEF\n" }
print "eof-loop: ", scalar(@got), " ", join("", @got);
close $m;
