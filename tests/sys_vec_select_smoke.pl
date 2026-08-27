#!/usr/bin/perl
# Smoke: vec, 4-arg select, 1-arg select.
use strict;
use warnings;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

my $s = "";
vec($s, 0, 8) = 65;
check('vec8_store', $s eq "A");
check('vec8_get', vec($s, 0, 8) == 65);

$s = "";
vec($s, 0, 1) = 1;
vec($s, 7, 1) = 1;
check('vec1_bits', vec($s, 0, 1) == 1 && vec($s, 7, 1) == 1 && vec($s, 1, 1) == 0);

my ($r, $w);
pipe($r, $w);
my $rin = "";
vec($rin, fileno($r), 1) = 1;
my $rout = $rin;
my $n = select($rout, undef, undef, 0);
check('select_empty', $n == 0);
syswrite($w, "x");
$rout = $rin;
$n = select($rout, undef, undef, 1);
check('select_ready', $n == 1 && vec($rout, fileno($r), 1) == 1);
close($r); close($w);

my $path = "/tmp/perlc_select_fh_$$";
open my $fh, ">", $path or die $!;
my $old = select($fh);
print "hello-select";
select($old);
close $fh;
open my $in, "<", $path or die $!;
my $got = <$in>;
close $in;
unlink $path;
check('select_fh_print', defined($got) && $got eq "hello-select");

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "sys_vec_select_smoke_done\n";
