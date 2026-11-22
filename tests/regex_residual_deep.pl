#!/usr/bin/perl
# Deep residual: many void ++ hits, NUL subject, array-elem subject,
# post++ value vs void, later alias of a pair after a match.
use strict;
use warnings;

my @fail;
sub check {
    my ( $n, $ok ) = @_;
    print "$n=", ( $ok ? "ok" : "FAIL" ), "\n";
    push @fail, $n unless $ok;
}

my $count = 0;
my @data;
for my $i ( 1 .. 90 ) {
    push @data, "item_$i" . ( $i % 3 == 0 ? "_match" : "" );
}
for my $x (@data) {
    $count++ if $x =~ /item_\d+_match/;
}
check( 'loop_count', $count == 30 );

my $snap = $count++;
check( 'loop_post', $snap == 30 && $count == 31 );

$count-- if 1;
check( 'void_dec', $count == 30 );

{
    my $bin = "a" . chr(0) . "b_match";
    my $n = 0;
    $n++ if $bin =~ /b_match/;
    $n++ if $bin =~ /a.b/;
    check( 'nul_subj', $n == 2 );
}

{
    my @a = ( "no", "item_3_match", "no" );
    my $n = 0;
    $n++ if $a[1] =~ /item_\d+_match/;
    $n++ if $a[0] =~ /item_\d+_match/;
    check( 'aelem_subj', $n == 1 );
}

{
    my $row = [ 3, 4 ];
    check( 'pair_then_alias', do {
        my $m = $row =~ /ARRAY/;
        my $y = $row;
        $y->[1] = 8;
        $m && $row->[0] == 3 && $row->[1] == 8 && $y->[1] == 8;
    } );
}

{
    my $pat = "item_\\d+_match";
    my $n = 0;
    $n++ if "item_6_match" =~ /$pat/;
    check( 'interp_void', $n == 1 );
}

{
    my $re = qr/item_\d+_match/;
    my $n = 0;
    $n++ if "item_6_match" =~ $re;
    $n++ if "item_1" =~ $re;
    check( 'qr_void', $n == 1 );
}

# $1 after a boolean-context match still installs when the unit reads it
{
    my $c1;
    $c1 = $1 if "hello" =~ /(he)llo/;
    check( 'cap_after_bool', $c1 eq "he" );
}

if (@fail) { print "UNEXPECTED_FAILURES=", join( ",", @fail ), "\n"; die "fail\n"; }
print "regex_residual_deep_done\n";
