#!/usr/bin/perl
# In-depth test suite for D67: pack/unpack were completely non-functional.
#
# Root cause: the parser already built NK::PackFunc/NK::UnpackFunc AST
# nodes, and runtime.c already implemented perl_pack/perl_unpack/
# perl_unpack_to_array, but codegen.cpp had zero `case` for either node
# kind at all — every pack()/unpack() call silently compiled to a no-op
# evaluating to empty/undef, with no compile- or run-time error.
#
# Fixed by adding the missing codegen dispatch (mirroring the existing
# SprintfFunc/JoinFunc/SplitFunc patterns) plus three missing RT() runtime
# registrations. Since the runtime functions had never actually been
# reachable before, several of their own latent bugs were also found and
# fixed in the process (all covered below): '*' as a count specifier was
# not recognized at all (silently meant "count of 1"); pack()'s a/A/h/H/b/B
# handling treated its count as "repeat over N args" instead of Perl's
# actual field-width-on-one-arg semantics; unpack()'s "A" didn't strip
# trailing spaces/NULs; unpack()'s scalar-context path returned a whole
# ARRAY ref instead of the first scalar value; pack()'s array-argument
# handling didn't flatten array variables into their elements; and an
# 8-byte (Q) unsigned-mask computation was undefined behavior (shift-by-64)
# that silently zeroed every unpacked Q value.
#
# NOTE: every value/format combination below is deliberately chosen to
# avoid producing an embedded NUL byte anywhere in the packed binary data.
# perlc's PerlValue string representation has no explicit byte length and
# relies on NUL-termination throughout the runtime (perl_alloc_string uses
# strdup()) — packed data containing a NUL byte (e.g. pack("N", small_int),
# since network-byte-order integers commonly have a leading zero byte, or
# "a"'s NUL-padding) is silently truncated at that byte. This is a real,
# separate, deep architectural limitation, NOT fixed as part of this
# defect — logged as TESTS.md D85. Every check here uses values/formats
# that don't hit that gap, so this suite exercises pack/unpack's actual
# format-handling logic cleanly, without conflating it with D85.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: basic pack/unpack round trip, single-byte type ──────────────
{
    my $packed = pack("C4", 65, 66, 67, 68);
    check('pack_basic_c4', $packed eq "ABCD");
    my @vals = unpack("C4", $packed);
    check('unpack_basic_c4', "@vals" eq "65 66 67 68");
}

# ── Section 2: '*' count specifier (pack: all remaining args; unpack: as
#    many as fit) — previously silently meant "count of 1" ────────────────
{
    my @vals = unpack("C*", "AB");
    check('unpack_star_count', "@vals" eq "65 66");
}
{
    my $packed = pack("C*", 65, 66, 67);
    check('pack_star_count', $packed eq "ABC");
}

# ── Section 3: 'A' string type — field width applies to ONE arg (pad with
#    spaces, truncate if longer), not "repeat over N args" ────────────────
{
    my $p = pack("A5", "hi");
    check('pack_A_pads_with_spaces', $p eq "hi   ");
}
{
    my $p = pack("A2", "hello");
    check('pack_A_truncates', $p eq "he");
}
{
    my @u = unpack("A5", "hi   ");
    check('unpack_A_strips_trailing_spaces', "@u" eq "hi");
}
{
    my $p = pack("A*", "exact");
    check('pack_A_star_uses_arg_length', $p eq "exact");
}

# ── Section 4: 2-byte and 4-byte numeric types, values chosen so no byte
#    in their representation is zero (avoiding the D85 NUL-truncation gap
#    — this suite is about format-handling correctness, not D85) ──────────
{
    my $v = 0x4142;  # bytes 0x41,0x42 — both nonzero in either endianness
    for my $fmt (qw(n v S s)) {
        my $p = pack($fmt, $v);
        check("pack_unpack_roundtrip_$fmt", length($p) == 2);
        my ($back) = unpack($fmt, $p);
        # signed 's' at 0x4142 is still positive (< 0x8000), so no sign
        # difference to account for.
        check("roundtrip_value_$fmt", $back == $v);
    }
}
{
    my $v = 0x41424344;  # bytes 0x41,0x42,0x43,0x44 — all nonzero
    for my $fmt (qw(N V L l I i)) {
        my $p = pack($fmt, $v);
        check("pack_unpack_roundtrip_$fmt", length($p) == 4);
        my ($back) = unpack($fmt, $p);
        check("roundtrip_value_$fmt", $back == $v);
    }
}

# ── Section 5: 8-byte type (Q/q) — exercises the fixed shift-by-64 mask
#    bug found while writing this test ──────────────────────────────────────
{
    my $v = 72623859790382856;  # == 0x0102030405060708, every byte nonzero
                                 # (written as decimal, not hex()/hex-literal,
                                 # to avoid real Perl's harmless "Hexadecimal
                                 # number > 0xffffffff non-portable" warning,
                                 # which would break this file's exact-match
                                 # comparison since perlc has no `no warnings`
                                 # support yet to suppress it — see D48)
    my $p = pack("Q", $v);
    check('pack_Q_length', length($p) == 8);
    my ($back) = unpack("Q", $p);
    check('unpack_Q_roundtrip', $back == $v);
}

# ── Section 6: signed types with negative values ────────────────────────────
{
    my $p = pack("c", -100);
    my ($back) = unpack("c", $p);
    check('signed_c_negative', $back == -100);
}
{
    my $p = pack("l", -1094861636);
    my ($back) = unpack("l", $p);
    check('signed_l_negative', $back == -1094861636);
}

# ── Section 7: array-argument flattening (pack("C4", @arr) needs @arr's
#    elements individually, not scalar(@arr)) ──────────────────────────────
{
    my @orig = (1, 100, 200, 255);
    my $p = pack("C4", @orig);
    my @back = unpack("C4", $p);
    check('array_arg_flattening', "@back" eq "@orig");
}

# ── Section 8: multiple format codes combined in one pack/unpack call ──────
{
    my $combo = pack("A3C2", "abc", 65, 66);
    check('combo_format_length', length($combo) == 5);
    my @cu = unpack("A3C2", $combo);
    check('combo_format_unpack', "@cu" eq "abc 65 66");
}

# ── Section 9: scalar vs list context ───────────────────────────────────────
{
    my $packed = pack("C4", 10, 20, 30, 40);
    my $first = unpack("C4", $packed);  # scalar context: just the first value
    check('unpack_scalar_context', $first == 10);
    my @all = unpack("C4", $packed);    # list context: all four
    check('unpack_list_context', "@all" eq "10 20 30 40");
}

# ── Section 10: pack with no args beyond the format (defaults to undef ->
#    0, shouldn't crash). NOTE: deliberately does NOT check length($p) here
#    — missing args pack as all-zero bytes, which hits the separate D85
#    NUL-truncation gap (real Perl: length 4; perlc: length 0, since the
#    first byte is already NUL) — not what this section is testing. ───────
{
    my $p = pack("C4");
    check('pack_missing_args_no_crash', defined($p));
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "d67_pack_unpack_done\n";
