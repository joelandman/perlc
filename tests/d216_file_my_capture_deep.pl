#!/usr/bin/perl
# D216 deep: Pod::Man-shaped subst, nested closure, assignment order,
# and a second `do` of an `our` plus an initialized `my`.
use strict;
use warnings;

my $lib = 'd216_domy_deep_lib.pl';
open my $fh, '>', $lib or die "Cannot create $lib: $!";
print $fh <<'END';
package D216D;
my ($NBSP, $SHY);
sub scrub {
    my ($text) = @_;
    $text =~ s{$NBSP}{ }g if $NBSP;
    $text =~ s{$SHY}{}g if $SHY;
    return $text;
}
sub nested {
    my ($text) = @_;
    my $cb = sub {
        my ($s) = @_;
        $s =~ s{$NBSP}{ }g if $NBSP;
        return $s;
    };
    return $cb->($text);
}
sub manout {
    my ($text) = @_;
    $text =~ s{$NBSP}{\\ }xmsg;
    $text =~ s{$SHY}{\\%}xmsg;
    return $text;
}
sub set_pair {
    $NBSP = $_[0];
    $SHY  = $_[1];
}
print "before=[", scrub("aZb-c"), "]\n";
$NBSP = "Z";
$SHY  = "-";
print "after=[", scrub("aZb-c"), "]\n";
print "nest=[", nested("aZb"), "]\n";
print "man=[", manout("aZb-c"), "]\n";
set_pair("", "");
print "empty=[", scrub("aZb-c"), "]\n";
set_pair("Z", "-");

our $runs;
$runs++;
print "runs=$runs\n";
my $fresh = 1;
sub see_fresh { return $fresh++ }
print "fresh=", see_fresh(), "\n";
1;
END
close $fh;

my $ret = do './d216_domy_deep_lib.pl';
print "ret1=", (defined $ret ? $ret : "UNDEF"), "\n";
print "err1=", ($@ ? "ERR" : "ok"), "\n";
print "again=[", D216D::scrub("aZb-c"), "]\n";
print "man2=[", D216D::manout("xZy"), "]\n";
D216D::set_pair("Q", "!");
print "set=[", D216D::nested("aQb!c"), "]\n";
print "scrub2=[", D216D::scrub("aQb!c"), "]\n";

my $ret2 = do './d216_domy_deep_lib.pl';
print "ret2=", (defined $ret2 ? $ret2 : "UNDEF"), "\n";
print "err2=", ($@ ? "ERR" : "ok"), "\n";
print "post=[", D216D::scrub("aZb-c"), "]\n";

unlink $lib;
