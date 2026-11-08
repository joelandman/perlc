# Regression: two-arg open(FH, "-") treats '-' as STDIN.
#
# The bare '-' form (no explicit IO mode) is read mode by default in perl, so
# '-' expands to STDIN. perlc used to fail to open it.
#
# Input "hello world" is 11 bytes.
use strict;
use warnings;

my $fh;
if (open($fh, "-")) {
    my $buf = do { local $/; <$fh> };
    print "bare_dash_read_len=", length($buf), "\n";
} else {
    print "bare_dash_open_failed: $!\n";
}
