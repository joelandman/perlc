#!/usr/bin/perl
# Deep: vec bit layout + select timeout/ready.
use strict;
use warnings;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

{
    my $s = "";
    vec($s, 1, 8) = 0x42;
    check('vec8_offset1_len', length($s) == 2);
    check('vec8_offset1_val', vec($s, 1, 8) == 0x42 && vec($s, 0, 8) == 0);
}

{
    my $s = "";
    vec($s, 0, 4) = 0xA;
    vec($s, 1, 4) = 0xC;
    check('vec4_low', vec($s, 0, 4) == 0xA);
    check('vec4_high', vec($s, 1, 4) == 0xC);
}

{
    my $s = "";
    vec($s, 0, 16) = 0x1234;
    check('vec16', vec($s, 0, 16) == 0x1234);
}

{
    my $s = "\0\0";
    check('vec_get_missing', vec($s, 9, 8) == 0);
}

{
    my ($r, $w);
    pipe($r, $w);
    my $rin = "";
    vec($rin, fileno($r), 1) = 1;
    my $win = "";
    vec($win, fileno($w), 1) = 1;
    my $rout = $rin;
    my $wout = $win;
    my $n = select($rout, $wout, undef, 0);
    check('select_write_ready', $n >= 1 && vec($wout, fileno($w), 1) == 1);
    close $r; close $w;
}

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "sys_vec_select_done\n";
