#!/usr/bin/perl
# Smoke: :encoding(UTF-8) / :utf8 open layers mark strings UTF-8 (length in chars).
use strict;
use warnings;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

my $path = "tests/_utf8_open.tmp";
{
    open my $w, ">:utf8", $path or die $!;
    print $w (chr(0xE9) . "\n");
    close $w;
}
{
    open my $r, "<:encoding(UTF-8)", $path or die $!;
    my $s = <$r>;
    chomp $s;
    close $r;
    check('len_char', length($s) == 1);
}

unlink $path;
if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "utf8_open_smoke_done\n";
