# Deep test for D168: two separate, pre-existing defects found via a
# real /usr/bin/json_pp script (its 'json' encoder sub does
# `my $json = JSON::PP->new->utf8; ...; $json->canonical if
# $json_opt{pretty}; ... $json->encode($_);`).
#
# 1. A CODE reference (`sub {...}`, \&name, etc.) was unconditionally
#    FALSE in boolean context -- perl_is_true()'s switch over
#    PerlValue tags listed every other reference tag (REF_SCALAR/
#    REF_ARRAY/REF_HASH/FLAT_ARRAY/...) as always-true but omitted
#    PERL_CODE_REF entirely, falling through to the `default: return
#    0` case. Any `if ($coderef)`, `$coderef or ...`, `!$coderef`,
#    ternary, etc. on a plain, non-undef code reference was silently
#    wrong. Found via json_pp's hash-of-subs dispatch table pattern
#    (`$F{$opt_from} or die "...";` where %F's values are anon subs)
#    though the bug has nothing to do with hashes specifically -- a
#    bare `my $cr = sub {1}; if ($cr) {...}` reproduces it standalone.
# 2. A SEPARATE, more severe use-after-free/memory-corruption bug
#    found while verifying (1)'s fix let the script progress further:
#    several native OO "chainable setter" methods (JSON::PP's
#    canonical/pretty/indent/utf8/.../filter_json_object, Math::
#    BigInt's bneg) returned the literal `obj` pointer -- the EXACT
#    same PerlValue* the caller's own live variable (e.g. `$json`)
#    still points to -- instead of a clone. A void-context statement
#    call (`$json->canonical;`, result discarded, the common case for
#    a chainable setter) has codegen free whatever Value* a call
#    returns as an owned temporary, so this freed the live variable's
#    own object out from under it: `ref($json)` immediately afterward
#    returned garbage memory, and any subsequent method call on
#    $json died "Can't locate object method ... via package
#    "<garbage bytes>"". Fixed by returning perl_clone(obj) instead,
#    which wraps the SAME underlying refcounted PerlHash/mpz_t in a
#    fresh, independently-freeable PerlValue -- the chain
#    ($json->canonical->pretty->encode(...)) keeps working exactly as
#    before, and a discarded, assigned, or further-chained call are
#    all safe.
use JSON::PP;
use Math::BigInt;

# (1): plain code-ref truthiness, several shapes
my $cr1 = sub { 1 };
print "cr_if=", ($cr1 ? "true" : "false"), "\n";
print "cr_not=", (!$cr1 ? "true" : "false"), "\n";
print "cr_ternary=", ($cr1 ? "t" : "f"), "\n";
sub named_sub { 42 }
my $cr2 = \&named_sub;
print "coderef_ref_if=", ($cr2 ? "true" : "false"), "\n";
my %dispatch = ('a' => sub { "A" }, 'b' => sub { "B" });
my $key = 'a';
print "hash_coderef_or=", ($dispatch{$key} ? "true" : "unreachable"), "\n";
$dispatch{$key} or print "WRONG: should not print\n";
my @arr = (sub { "X" });
print "arr_coderef_if=", ($arr[0] ? "true" : "false"), "\n";

# (2): JSON::PP chainable setters -- void-context (discarded),
# assigned, and chained forms must all leave $json intact
my $json = JSON::PP->new->utf8;
$json->canonical;                       # void context -- the crash repro
print "ref_after_void_call=", ref($json), "\n";
print "encode_after_void_call=", $json->encode({b=>2,a=>1}), "\n";

my $json2 = JSON::PP->new->utf8;
my $r = $json2->canonical;              # assigned -- also must not corrupt $json2
print "ref_after_assigned_call=", ref($json2), "\n";
print "ref_of_assigned_result=", ref($r), "\n";

my $json3 = JSON::PP->new->canonical->pretty->utf8;  # chained
print "ref_after_chain=", ref($json3), "\n";
print "encode_after_chain=", $json3->encode({a=>1}), "\n";

# (2) regression: Math::BigInt's bneg (same self-aliasing shape)
my $bi = Math::BigInt->new(5);
$bi->bneg;                              # void context
print "bigint_after_bneg=$bi\n";
my $bi2 = Math::BigInt->new(9);
my $bi2c = $bi2->copy;
$bi2c->bneg;
print "bigint_orig_unaffected=$bi2 bigint_copy_negated=$bi2c\n";

print "done\n";
