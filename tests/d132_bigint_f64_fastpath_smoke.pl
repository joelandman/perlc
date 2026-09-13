#!/usr/bin/perl
# Smoke test for D132: emitBinOp's F64 "stay unboxed" fast path used to
# convert a BigInt-tagged scalar VARIABLE straight to double and add/sub/mul
# natively, bypassing perl_add/perl_sub/perl_mul's D103 BigInt-aware logic.
# A value sitting exactly at the UINT64_MAX literal boundary undergoing
# further arithmetic that crosses beyond it diverged by 1 ULP after
# stringification.
use strict;
use warnings;

my $chain = 18446744073709551615;   # UINT64_MAX literal, parses as auto-BigInt per D103
$chain = $chain + 1;
print "$chain\n";