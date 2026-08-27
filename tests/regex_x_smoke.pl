#!/usr/bin/perl
# Smoke: regex /x and m{}/x.
use strict;
use warnings;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

check('slash_x', "helloworld" =~ /hello world/x);
check('comment_x', "abc" =~ /a #cmt
 b
 c/x);
check('mbrace_x', "abc" =~ m{ a b c }x);
check('subst_x', do { my $s = "ab"; $s =~ s/ a b /XY/x; $s eq "XY" });
check('escaped_space', "a b" =~ /a\ b/x);
check('no_space_match', !("a b" =~ /a b/x));
check('m_fatcomma', do { my %h = (m => 7); $h{m} == 7 });

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "regex_x_smoke_done\n";
