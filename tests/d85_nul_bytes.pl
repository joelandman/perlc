#!/usr/bin/perl
# In-depth test suite for D85: PerlValue's string representation (a plain
# heap-allocated, NUL-terminated `char *sval`) had no explicit length
# field, so any string containing an embedded NUL byte was silently
# truncated wherever the runtime derived its length via strlen() instead
# of tracking it explicitly — which was nearly everywhere: allocation,
# perl_clone, perl_assign, local(), concatenation, string comparisons,
# sort's default string comparator, tr///, print/say (to STDOUT and to a
# filehandle), length(), chr(0), and pack()/unpack() themselves (the
# original repro: pack("N", 1234567) has a leading 0x00 byte, so
# perl_alloc_string(out) — which used strdup(out) internally — silently
# stopped right there, discarding 3 of the packed value's 4 bytes).
#
# Fixed by adding an explicit `slen` byte-length field to PerlValue
# (runtime.h) and threading it through every allocation/copy/consumer
# site that used to derive length via strlen()/strcpy()/strcat()/strcmp():
# perl_alloc_string_len (new), perl_to_string_dup_len (new), perl_clone,
# perl_assign, perl_local_save, perl_is_true, perl_concat,
# perl_repeat_str, perl_str_eq/ne/lt/gt/le/ge, perl_str_spaceship,
# cmp_str_pv/cmp_str_asc (sort comparators), perl_print/perl_say/
# perl_print_fh/perl_say_fh (now fwrite-based), perl_length (via a new
# bounded utf8_strlen_n), perl_chr_val, perl_pack, perl_unpack_to_array
# (both the format-driven 'a' field extraction and the overall string
# argument read), tr///, and perl_readline (both slurp mode and
# line-delimited mode) — the read half of the pack -> file -> unpack
# binary round trip. Also fixed perl_is_true, which previously treated
# ANY string starting with a NUL byte as false regardless of length (a
# genuine 1-byte string holding just chr(0) is neither "" nor "0" and
# must be true in real Perl).
#
# `sizeof(PerlValue)` grew from 32 to 48 bytes to keep it a 16-byte
# multiple — required for the existing lock-free 16-byte CAS
# (cmpxchg16b/ldxp+stxp) used by threads::shared scalar RMW, which needs
# every slab-allocated PerlValue to stay 16-byte aligned; verified with
# `make test-tsan` (zero race reports, unchanged from before this fix).
#
# Documented, deliberately NOT-fixed remaining gaps (out of scope — see
# TESTS.md): substr/chomp/chop/string-increment still derive positions
# via strlen()/UTF-8 byte-scanning internally, not NUL-safe on a string
# with an embedded NUL; sprintf/printf's %s conversion still truncates a
# NUL-containing argument (a libc snprintf(..., "%s", ...) limitation);
# PCRE2 regex matching against a NUL-containing subject is still
# strlen()-bounded. A further, separate, narrower defect found while
# writing this test is logged as new defect D90 (not fixed here):
# utf8_strlen's byte-scanning heuristic can misidentify two consecutive
# raw bytes in packed binary data as a single UTF-8 continuation
# sequence (e.g. 0xD6 followed by 0x87 both look like a valid 2-byte
# UTF-8 character to the heuristic), undercounting length() by one for
# such byte patterns — this test deliberately avoids asserting length()
# on a byte sequence known to trigger it.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: the original repro — pack/unpack round trip ───────────────
{
    my $packed = pack("N", 1234567);
    my ($n) = unpack("N", $packed);
    check('pack_unpack_roundtrip', $n == 1234567);
    my @bytes = unpack("C4", $packed);
    check('pack_produces_correct_bytes', "@bytes" eq "0 18 214 135");
}

# ── Section 2: concatenation preserves embedded NUL bytes ────────────────
{
    my $p = pack("N", 1234567);
    my $cat = $p . "TAIL";
    my @b = unpack("C7", $cat);
    check('concat_preserves_bytes', "@b" eq "0 18 214 135 84 65 73");
}

# ── Section 3: plain assignment preserves embedded NUL bytes ─────────────
{
    my $p = pack("N", 1234567);
    my $copy = $p;
    my @b = unpack("C4", $copy);
    check('assign_preserves_bytes', "@b" eq "0 18 214 135");
}

# ── Section 4: string equality/inequality on NUL-containing strings ──────
{
    my $p1 = pack("N", 1234567);
    my $p2 = pack("N", 1234567);
    my $p3 = pack("N", 1234568);
    check('eq_with_embedded_nul', $p1 eq $p2);
    check('ne_with_embedded_nul', $p1 ne $p3);
    check('lt_with_embedded_nul', ($p1 lt $p3) ? 1 : 0);
    check('cmp_with_embedded_nul', ($p1 cmp $p3) == -1);
}

# ── Section 5: sort's default string comparator on NUL-containing data ───
{
    my $p1 = pack("N", 300);
    my $p2 = pack("N", 100);
    my $p3 = pack("N", 200);
    my @sorted = sort ($p1, $p2, $p3);
    my @n = map { my ($v) = unpack("N", $_); $v } @sorted;
    check('sort_default_comparator_nul_safe', "@n" eq "100 200 300");
}

# ── Section 6: tr/// counts and passes through embedded NUL correctly.
#    Reads the tail back via unpack's 'a' field (fixed, length-aware)
#    rather than substr($upper, 4) — substr's own byte-offset math is a
#    documented, deliberately-not-fixed remaining gap (still strlen()/
#    UTF-8-byte-scan based internally on the source string, so an offset
#    *past* an embedded NUL is not reliably correct yet; see TESTS.md).
#    Uses a throwaway `$head` rather than `my (undef, $tail) = ...` — the
#    latter is a separate, unrelated, pre-existing gap found while
#    writing this test: `my (undef, $x) = LIST` does not correctly skip
#    the first element under perlc (assigns LIST's first element to $x
#    instead of its second) — out of scope here, not logged as its own
#    defect since it wasn't investigated further. ─────────────────────────
{
    my $p = pack("N", 1234567) . "abc";
    (my $upper = $p) =~ tr/a-z/A-Z/;
    my @b = unpack("C4", $upper);
    check('tr_preserves_leading_nul_bytes', "@b" eq "0 18 214 135");
    my ($head, $tail) = unpack("a4a3", $upper);
    check('tr_still_transliterates_tail', $tail eq "ABC");
}

# ── Section 7: is_true — a lone NUL-byte string is TRUE (not "" or "0"),
#    while genuinely empty string and "0" remain FALSE ─────────────────
{
    my $lone_nul = chr(0);
    check('lone_nul_is_true', $lone_nul ? 1 : 0);
    check('empty_string_is_false', "" ? 0 : 1);
    check('zero_string_is_false', "0" ? 0 : 1);
}

# ── Section 8: chr(0) — both truthiness and byte length ──────────────────
{
    my $z = chr(0);
    check('chr0_length_is_one', length($z) == 1);
}

# ── Section 9: passing a NUL-containing value through a sub call,
#    an array element, and a hash element all preserve it ───────────────
{
    my $p = pack("N", 1234567);
    sub identity { return $_[0]; }
    my @b1 = unpack("C4", identity($p));
    check('sub_arg_preserves_bytes', "@b1" eq "0 18 214 135");

    my @arr = ($p);
    my @b2 = unpack("C4", $arr[0]);
    check('array_elem_preserves_bytes', "@b2" eq "0 18 214 135");

    my %h = (k => $p);
    my @b3 = unpack("C4", $h{k});
    check('hash_elem_preserves_bytes', "@b3" eq "0 18 214 135");
}

# ── Section 10: local() save/restore preserves embedded NUL bytes ────────
{
    our $g = pack("N", 1234567);
    sub uses_local { local $g = pack("N", 99); return $g; }
    my $inner = uses_local();
    my @b_inner = unpack("C4", $inner);
    check('local_inner_value_correct', "@b_inner" eq "0 0 0 99");
    my @b_outer = unpack("C4", $g);
    check('local_restores_outer_bytes', "@b_outer" eq "0 18 214 135");
}

# ── Section 11: file write + read round trip preserves embedded NUL
#    bytes (the pack -> file -> unpack binary workflow this fix exists
#    for) ─────────────────────────────────────────────────────────────
{
    my $p = pack("N", 1234567);
    my $tmpfile = "/tmp/d85_fileio_test_$$.bin";
    open(my $fh, '>', $tmpfile) or die "open write: $!";
    binmode $fh;
    print $fh $p;
    close $fh;

    open(my $rfh, '<', $tmpfile) or die "open read: $!";
    binmode $rfh;
    local $/;
    my $readback = <$rfh>;
    close $rfh;
    unlink $tmpfile;

    my @b = unpack("C4", $readback);
    check('file_roundtrip_preserves_bytes', "@b" eq "0 18 214 135");
}

# ── Section 12: regressions — plain ASCII string operations, sort, tr,
#    closures, and repeat (x) all still work correctly with no embedded
#    NUL bytes anywhere involved ─────────────────────────────────────────
{
    check('regression_concat', ("foo" . "bar") eq "foobar");
    check('regression_eq', ("abc" eq "abc") ? 1 : 0);
    check('regression_cmp', ("abc" cmp "abd") == -1);
    my @s = sort ("banana", "apple", "cherry");
    check('regression_sort', "@s" eq "apple banana cherry");
    my $t = "hello";
    (my $u = $t) =~ tr/a-z/A-Z/;
    check('regression_tr', $u eq "HELLO");
    check('regression_repeat', ("ab" x 3) eq "ababab");
    my $c = 1;
    my $inc = sub { $c++; };
    $inc->(); $inc->();
    check('regression_closure', $c == 3);
    check('regression_length_ascii', length("hello") == 5);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "d85_nul_bytes_done\n";
