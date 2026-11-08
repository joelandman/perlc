# Regression: Digest::SHA->new(256)->addfile("-") reads STDIN.
#
# This is the exact code path shasum uses (sumfile() calls
# Digest::SHA->new($alg)->addfile($file, $mode) with $file eq '-'). perlc used
# to fopen("-", "rb") and read an empty digest. SHA1/"" == da39a3ee..., so a
# stale empty digest is the tell-tale sign of the regression.
#
# Input "hello world" -> sha256 b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9.
use strict;
use warnings;
use Digest::SHA qw(sha256_hex);

my $d = Digest::SHA->new(256);
eval { $d->addfile("-"); 1 };
if ($@) {
    print "addfile_dash_error=$@\n";
} else {
    print "addfile_dash_sha256=", $d->hexdigest, "\n";
}
