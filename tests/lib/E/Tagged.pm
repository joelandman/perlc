package E::Tagged;
use strict;
use warnings;
use Exporter 'import';
our @EXPORT = qw(munge);
our @EXPORT_OK = qw(munge frobnicate);
our %EXPORT_TAGS = (
    basic => [ qw(munge) ],
    all   => [ qw(munge frobnicate) ],
);
our $Cfg = "cfg-1";
sub munge {
    my ($s) = @_;
    return uc($s) . "-munged";
}
sub frobnicate {
    my ($a, $b) = @_;
    return $a * $b + 1;
}
1;
