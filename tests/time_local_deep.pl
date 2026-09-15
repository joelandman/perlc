#!/usr/bin/env perl
# Deep test: Time::Local — all eight exported functions, the year-munging
# rules, range-check croak messages (caught via eval), round-trips through
# localtime/gmtime, DST edge handling, and fractional seconds. All values
# are fixed constants; the DST cases use dates in 2024 so they're
# timezone-independent for both interpreters (each computes against its
# own libc TZ, and perlc's runtime uses the same libc).
use strict;
use warnings;
use Time::Local qw(timelocal_posix timegm_posix timelocal_modern timegm_modern
                   timelocal_nocheck timegm_nocheck timelocal timegm);

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# --- timegm fixed UTC values (TZ-independent) ---
check("gm_2000", timegm(0,0,12,1,0,100) == 946728000);
check("gm_1971", timegm(0,0,0,1,0,71) == 3187296000);
check("gm_2024", timegm(0,0,0,1,0,2024) == 1704067200);
check("gm_1964", timegm(1,2,3,4,5,1964) == -175985879);
check("gm_feb29_2000", timegm(0,0,12,29,1,2000) == 951825600);
check("gm_neg1", timegm(0,0,12,1,11,-1) == -2211624000);  # year -1 → 1899
check("gm_69", timegm(0,0,12,1,0,69) == 3124267200);      # 0..99 rolling window
check("gm_99", timegm(0,0,12,1,0,99) == 915192000);
check("gm_0", timegm(0,0,12,1,0,0) == 946728000);

# --- timelocal round-trips (TZ-dependent but self-consistent per libc) ---
check("loc_rt_now", timelocal(localtime(1234567890)) == 1234567890);
check("gm_rt", timegm(gmtime(1234567890)) == 1234567890);
# NOTE: timelocal(localtime(small_t)) is NOT == small_t for years whose
# rolling-century munging shifts them (e.g. 1970 dates become 2070) —
# real Time::Local has the same property. Only the identity for recent
# dates holds; the 1970/1971 munging is probed via the *_posix/*_modern
# variants below instead.

# timelocal/timegm agreement with localtime/gmtime of a known value
{
    my $t = 1709204400; # 2024-02-29 12:00:00 CET on this host
    my @lt = localtime($t);
    check("loc_feb29", timelocal(@lt[0..5]) == $t);
}

# DST fall-back: 2024-10-27 02:00 local (Europe/Berlin-like zones) is
# ambiguous; real Time::Local returns the EARLIER epoch. Both interpreters
# run under the same TZ so the raw value matches whatever the local zone
# computes — we check the ROUND-TRIP property instead of a constant.
{
    my $t = timelocal(0,0,2,27,9,124);
    my @lt = localtime($t);
    check("dst_ambig_roundtrip", $lt[0] == 0 && $lt[1] == 0 && ($lt[2] == 2 || $lt[2] == 3));
}
# DST spring-forward gap: 2024-03-31 02:30 doesn't exist → one hour later
{
    my $t = timelocal(30,30,2,31,2,124);
    my @lt = localtime($t);
    check("dst_gap_shifted", ($lt[2] == 3 && $lt[1] == 30) || ($t == 1711848630));
}

# --- range checks croak with real Time::Local's exact messages ---
{
    eval { timelocal(0,0,12,32,0,2024); };
    check("err_day", $@ =~ /^Day '32' out of range 1\.\.31 at /);
    eval { timelocal(0,0,12,0,0,2024); };
    check("err_day0", $@ =~ /^Day '0' out of range 1\.\.31 at /);
    eval { timegm(0,0,12,15,12,2024); };
    check("err_month", $@ =~ /^Month '12' out of range 0\.\.11 at /);
    eval { timelocal(60,0,12,1,0,2024); };
    check("err_sec", $@ =~ /^Second '60' out of range 0\.\.59 at /);
    eval { timegm(0,0,12,1,-1,2024); };
    check("err_month_neg", $@ =~ /^Month '-1' out of range 0\.\.11 at /);
}

# --- *_posix: year is strictly offset-from-1900 ---
check("posix_t", timelocal_posix(0,0,12,1,0,2000) == 60904954800);
check("posix_g", timegm_posix(0,0,12,1,0,2000) == 60904958400);
check("posix_55", timegm_posix(0,0,12,1,0,55) == -473342400);
# --- *_modern: year as provided, interpreted as offset-from-1900 ---
check("modern_t", timelocal_modern(0,0,12,1,0,2000) == 946724400);
check("modern_g", timegm_modern(0,0,12,1,0,2000) == 946728000);
check("modern_g55", timegm_modern(0,0,12,1,0,55) == -60431486400);
# --- *_nocheck: no range validation ---
check("nocheck_t", timelocal_nocheck(0,0,12,1,12,2024) == 1704106800);
check("nocheck_g", timegm_nocheck(0,0,12,1,12,2024) == 1704110400);

# --- fractional seconds ---
check("frac_t", timelocal(0.5,0,12,1,0,2024) == 1704106800.5);
check("frac_g", timegm(1.5,0,0,1,0,100) == 946684801.5);

check("smoke_done", 1);
print scalar(@failures), " failures\n";