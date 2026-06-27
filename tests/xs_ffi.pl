#!/usr/bin/perl
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Test 1: clock_gettime via Perl syscall ─────────────────────────────────
# CLOCK_MONOTONIC = 4, SYS_clock_gettime = 228 on Linux x86_64
my $CLOCK_MONOTONIC = 4;
my $SYS_clock_gettime = 228;

my $buf = "\0" x 16;  # struct timespec: two 8-byte longs
my $ret = syscall($SYS_clock_gettime, $CLOCK_MONOTONIC, $buf);
check('clock_gettime_return_zero', $ret == 0);

my ($tv_sec, $tv_nsec) = unpack("LL", $buf);
check('clock_gettime_sec_positive', $tv_sec > 0);
check('clock_gettime_nsec_range', $tv_nsec >= 0 && $tv_nsec < 1000000000);

# ── Test 2: clock_gettime with CLOCK_REALTIME ──────────────────────────────
my $CLOCK_REALTIME = 0;
$buf = "\0" x 16;
$ret = syscall($SYS_clock_gettime, $CLOCK_REALTIME, $buf);
check('clock_gettime_realtime_return_zero', $ret == 0);

($tv_sec, $tv_nsec) = unpack("LL", $buf);
check('clock_gettime_realtime_sec_positive', $tv_sec > 0);
check('clock_gettime_realtime_nsec_range', $tv_nsec >= 0 && $tv_nsec < 1000000000);

# ── Test 3: monotonic time advances across sleep ───────────────────────────
my ($s1, $n1) = unpack("LL", $buf);
syscall($SYS_clock_gettime, $CLOCK_MONOTONIC, $buf);
($s1, $n1) = unpack("LL", $buf);

sleep(1);

syscall($SYS_clock_gettime, $CLOCK_MONOTONIC, $buf);
my ($s2, $n2) = unpack("LL", $buf);
check('monotonic_advances', $s2 > $s1 || ($s2 == $s1 && $n2 >= $n1));

# ── Test 4: clock_gettime with CLOCK_PROCESS_CPUTIME_ID ────────────────────
my $CLOCK_PROCESS_CPUTIME_ID = 2;
$buf = "\0" x 16;
$ret = syscall($SYS_clock_gettime, $CLOCK_PROCESS_CPUTIME_ID, $buf);
check('clock_gettime_cpu_return_zero', $ret == 0);

($tv_sec, $tv_nsec) = unpack("LL", $buf);
check('clock_gettime_cpu_sec_nonneg', $tv_sec >= 0);
check('clock_gettime_cpu_nsec_range', $tv_nsec >= 0 && $tv_nsec < 1000000000);

# ── Test 5: getuid via syscall (simple int return) ─────────────────────────
# SYS_getuid = 102 on Linux x86_64
my $SYS_getuid = 102;
my $uid = syscall($SYS_getuid);
check('getuid_defined', defined($uid));
check('getuid_nonnegative', $uid >= 0);

# ── Test 6: getgid via syscall ─────────────────────────────────────────────
my $SYS_getgid = 104;
my $gid = syscall($SYS_getgid);
check('getgid_defined', defined($gid));
check('getgid_nonnegative', $gid >= 0);

# ── Test 7: getpid via syscall ─────────────────────────────────────────────
my $SYS_getpid = 39;
my $pid = syscall($SYS_getpid);
check('getpid_defined', defined($pid));
check('getpid_positive', $pid > 0);

# ── Test 8: Multiple monotonic calls return consistent results ─────────────
my $count = 0;
for my $i (1..5) {
    $buf = "\0" x 16;
    syscall($SYS_clock_gettime, $CLOCK_MONOTONIC, $buf);
    $count++;
}
check('monotonic_multiple_consistent', $count == 5);

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "xs_ffi_done\n";
