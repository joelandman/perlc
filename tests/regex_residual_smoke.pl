#!/usr/bin/perl
# Regex residual: void $count++ (file-scope boxed) and match-subject
# promote skip. Do not print ARRAY(0x...).
use strict;
use warnings;

my @fail;
sub check {
    my ( $n, $ok ) = @_;
    print "$n=", ( $ok ? "ok" : "FAIL" ), "\n";
    push @fail, $n unless $ok;
}

my $count = 0;
$count++ if "item_3_match" =~ /item_\d+_match/;
$count++ if "item_1" =~ /item_\d+_match/;
$count++ if "item_6_match" =~ /item_\d+_match/;
check( 'void_inc', $count == 2 );

my $old = $count++;
check( 'post_old', $old == 2 );
check( 'post_new', $count == 3 );

++$count if 1;
check( 'void_pre', $count == 4 );

my $item = "item_9_match";
my $hits = 0;
$hits++ if $item =~ /item_\d+_match/;
check( 'subj_var', $hits == 1 );

my @data = ( "item_1", "item_3_match", "item_6_match" );
my $fc = 0;
for my $x (@data) {
    $fc++ if $x =~ /item_\d+_match/;
}
check( 'foreach_match', $fc == 2 );

{
    my $row = [ 1, 2 ];
    my $m = $row =~ /ARRAY/;
    check( 'pair_match', $m );
    $row->[0] = 9;
    check( 'pair_write', $row->[0] == 9 && $row->[1] == 2 );
}

if (@fail) { print "UNEXPECTED_FAILURES=", join( ",", @fail ), "\n"; die "fail\n"; }
print "regex_residual_smoke_done\n";
