# Deep regression for the '-' -> STDIN convention.
#
#   * Two-arg read with "-" (and "< -") consumes STDIN, returning the full
#     piped content. perlc used to treat '-' as a literal filename and fail.
#   * Three-arg open(FH, "<", "-") does NOT expand '-': it is a literal
#     filename, so it fails to open.
#
# Input is a fixed 3-line payload. perlc must round-trip it byte-for-byte and
# match perl on the 3-arg literal-filename failure.
use strict;
use warnings;

my $fh;
if (open($fh, " -")) {
    my $buf = do { local $/; <$fh> };
    print "dash_full_len=", length($buf), "\n";
    print "dash_full=", defined($buf) ? $buf : "undef\n";
} else {
    print "dash_open_open_failed: $!\n";
}

my $ok = open(my $fh2, "<", "-");
print "threearg_dash=", (defined($ok) && $ok ? "OPENED" : "FAILED"), "\n";
