# Regression: two-arg open(FH, " -") treats a '-' filename as STDIN.
#
# Real Perl expands '-' (with an optional leading IO mode) to STDIN when the
# mode is a read. perlc used to treat '-' as a literal filename and fail to
# open it, which broke shasum/grog-style `open(FH, "< -")` code paths.
#
# Input "hello world" is 11 bytes. If the fix is present, open succeeds and
# the test prints the length; if perlc regresses, open fails and the test
# prints the "open failed" line instead.
use strict;
use warnings;

my $fh;
if (open($fh, "< -")) {
    my $buf = do { local $/; <$fh> };
    print "dash_open_read_len=", length($buf), "\n";
} else {
    print "dash_open_open_failed: $!\n";
}
