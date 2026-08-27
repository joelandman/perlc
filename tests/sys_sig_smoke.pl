#!/usr/bin/perl
# Smoke: live $SIG{USR1} via kill-to-self.
use strict;
use warnings;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

my $got = 0;
$SIG{USR1} = sub { $got = 1 };
kill 'USR1', $$;
check('usr1_handler', $got == 1);

$SIG{USR1} = 'DEFAULT';
check('usr1_reset', 1);

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "sys_sig_smoke_done\n";
