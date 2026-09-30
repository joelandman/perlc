#!/usr/bin/perl
# Deep test for the native Cpanel::JSON::XS alias.
#
# Cpanel::JSON::XS is NOT installed on this dev machine (no cpanm
# available here to install it) and real Cpanel::JSON::XS is a drop-in
# API-compatible replacement for JSON::PP/JSON::XS (that is its entire
# purpose), so this is skipped by default in tests/harness.sh
# (SKIP_BY_DEFAULT) rather than diffed against a real Perl run that
# would itself fail to even load the module — and is self-checking
# (asserts hardcoded expected values, verified two ways: (1) manually
# cross-checked that this exact script's logic, run through the
# already-real-Perl-verified native JSON::PP implementation instead,
# produces byte-identical output in perlc; (2) the expected values
# below are exactly what real installed JSON::PP produces for the same
# operations) rather than diffing against a live real-Perl run of this
# specific module name.
#
# Implementation: src/main.cpp/src/runtime.c alias "Cpanel::JSON::XS"
# onto the exact same perl_json_encode/perl_json_decode/
# perl_dispatch_method code paths JSON::PP already uses — blessing a
# constructed object as the class actually invoked (so `ref()` reports
# "Cpanel::JSON::XS", not "JSON::PP") except for the historical "JSON"
# proxy name, which real JSON.pm always blesses as "JSON::PP" (verified
# against real Perl).
use strict;
use warnings;
use Cpanel::JSON::XS qw(encode_json decode_json);

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# functional form round-trip
{
    my $data = { name => "Joe", nums => [1, 2, 3], nested => { a => 1 } };
    my $back = decode_json(encode_json($data));
    check('roundtrip_scalar', $back->{name} eq "Joe");
    check('roundtrip_array', join(",", @{$back->{nums}}) eq "1,2,3");
    check('roundtrip_nested', $back->{nested}{a} == 1);
}

# OO form: new/canonical/pretty chain, ref() reports the real class
{
    my $obj = Cpanel::JSON::XS->new->canonical->pretty;
    check('ref_reports_own_class', ref($obj) eq "Cpanel::JSON::XS");
    my $encoded = $obj->encode({ b => 2, a => 1 });
    check('canonical_pretty_encode', $encoded eq "{\n   \"a\" : 1,\n   \"b\" : 2\n}\n");
    my $decoded = $obj->decode('{"x":10,"y":20}');
    check('oo_decode', $decoded->{x} + $decoded->{y} == 30);
}

# booleans round-trip as real JSON::PP::Boolean-shaped values
{
    my $encoded = encode_json({ ok => Cpanel::JSON::XS::true(), bad => Cpanel::JSON::XS::false() });
    my $decoded = decode_json($encoded);
    check('boolean_true_roundtrip', $decoded->{ok} ? 1 : 0);
    check('boolean_false_roundtrip', !$decoded->{bad});
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "cpanel_json_xs_done\n";
