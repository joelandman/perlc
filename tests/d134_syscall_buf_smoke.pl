#!/usr/bin/perl
# Smoke test for D134: syscall() arguments used to be pushed into the args
# array via perl_array_push, which CLONES each argument — so a syscall that
# writes through a pointer argument (SYS_clock_gettime's struct timespec
# buffer) wrote into the clone and the caller's $buf never changed.
# Arguments are now pushed by reference so kernel writes land in the
# caller's own buffer, matching real perl.
use strict;
use warnings;

# CLOCK_MONOTONIC = 4, SYS_clock_gettime = 228 on Linux x86_64
my $buf = "\0" x 16;
my $ret = syscall(228, 4, $buf);
print "ret_zero=", ($ret == 0 ? "ok" : "FAIL"), "\n";
my ($tv_sec, $tv_nsec) = unpack("LL", $buf);
print "sec_positive=", ($tv_sec > 0 ? "ok" : "FAIL"), "\n";
print "nsec_range=", ($tv_nsec >= 0 && $tv_nsec < 1000000000 ? "ok" : "FAIL"), "\n";
print "d134_syscall_buf_done\n";