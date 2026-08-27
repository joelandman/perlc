#!/usr/bin/perl
# Deep: core process/IPC builtins — byte-for-byte vs real Perl.
# getuid/getgid live in POSIX, not perlfunc core; covered via syscall tests.
use strict;
use warnings;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

check('getppid_positive', getppid() > 1);
check('getpgrp_defined', defined(getpgrp()) && getpgrp() >= 0);
check('umask_roundtrip', do {
    my $old = umask(077);
    my $now = umask($old);
    $now == 077
});

{
    my $pid = fork();
    if (!defined $pid) {
        check('wait_fork', 0);
    } elsif ($pid == 0) {
        exit 42;
    } else {
        my $w = waitpid($pid, 0);
        check('wait_pid_match', $w == $pid);
        check('wait_status_exit42', ($? >> 8) == 42);
        check('kill0_gone', kill(0, $pid) == 0);
    }
}

{
    my ($r, $w);
    check('pipe2', pipe($r, $w));
    my $pid = fork();
    if (!defined $pid) {
        check('sys_fork', 0);
    } elsif ($pid == 0) {
        close($r);
        syswrite($w, "ABCDEF", 6);
        close($w);
        exit 0;
    } else {
        close($w);
        my $buf = "";
        my $n = sysread($r, $buf, 6);
        close($r);
        waitpid($pid, 0);
        check('sysread_n', $n == 6);
        check('sysread_buf', $buf eq "ABCDEF");
    }
}

{
    my $path = "/tmp/perlc_ipc_sysopen_$$";
    unlink $path;
    my $fh;
    # Linux: O_WRONLY=1 O_CREAT=64 O_TRUNC=512 O_RDONLY=0
    my $okw = sysopen($fh, $path, 1 | 64 | 512, 0666);
    check('sysopen_write', $okw);
    my $wn = syswrite($fh, "xyz123");
    check('syswrite_n', $wn == 6);
    close($fh);
    my $rh;
    check('sysopen_read', sysopen($rh, $path, 0));
    my $buf = "";
    my $rn = sysread($rh, $buf, 6);
    check('file_sysread_n', $rn == 6);
    check('file_sysread_buf', $buf eq "xyz123");
    check('flock_ex', flock($rh, 2));
    check('flock_un', flock($rh, 8));
    close($rh);
    unlink $path;
}

check('kill0_self', kill(0, $$) == 1);

if (@fail) {
    print "UNEXPECTED_FAILURES=", join(",", @fail), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @fail) . "\n";
}
print "ipc_process_done\n";
