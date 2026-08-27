#!/usr/bin/perl
# Deep: labeled loops, forward/back goto, goto & with @_.
use strict;
use warnings;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

sub fwd {
    goto L;
    return "no";
    L: return "yes";
}
check('forward', fwd() eq "yes");

sub loop_goto {
    my $n = 0;
    my $i = 0;
    LOOP: while ($i < 10) {
        $i++;
        $n++;
        goto LOOP if $i < 3;
        last;
    }
    return $n;
}
check('loop_label', loop_goto() == 3);

sub tail_join {
    my @xs = @_;
    goto &join_them;
}
sub join_them { join "-", @_ }
check('goto_slurp', tail_join("a", "b", "c") eq "a-b-c");

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "goto_deep_done\n";
