#!/usr/bin/perl
use strict;
use warnings;

# ── string eval removed with JIT; expect $@ set + undef return ──────────────
my $result = eval "1 + 2";
print "result=", defined($result) ? $result : "undef", "\n";  # undef
print "err=", ($@ =~ /not available/ ? "yes" : "no"), "\n";   # yes

$result = eval '"hello"';
print "result2=", defined($result) ? $result : "undef", "\n";

# ── $@ is non-empty after attempted string eval ────────────────────────────
eval "1 + 1";
print "err_nonempty=", (length($@) > 0 ? "yes" : "no"), "\n";

# ── die inside string eval also yields undef + error in $@ ─────────────────
eval 'die "something went wrong"';
print "caught=", ($@ =~ /something went wrong|not available/ ? "yes" : "no"), "\n";

print "eval_string_done\n";
