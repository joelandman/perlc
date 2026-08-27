#!/usr/bin/perl
# Deep: $SIG handler arg, IGNORE, ALRM interrupting sleep.
use strict;
use warnings;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

{
    my $name = "";
    $SIG{USR2} = sub { $name = $_[0] };
    kill 'USR2', $$;
    check('handler_arg', $name eq "USR2");
    $SIG{USR2} = 'DEFAULT';
}

{
    my $n = 0;
    $SIG{USR1} = sub { $n++ };
    kill 'USR1', $$;
    kill 'USR1', $$;
    check('handler_twice', $n == 2);
    $SIG{USR1} = 'IGNORE';
    kill 'USR1', $$;
    check('ignore', $n == 2);
    $SIG{USR1} = 'DEFAULT';
}

{
    my $a = 0;
    $SIG{ALRM} = sub { $a = 1 };
    alarm(1);
    sleep(3);
    alarm(0);
    check('alrm_during_sleep', $a == 1);
    $SIG{ALRM} = 'DEFAULT';
}

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "sys_sig_done\n";
