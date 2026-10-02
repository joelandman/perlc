# Deep test for D171: two defects found investigating a real
# /usr/bin/deb-systemd-helper script's `$opts{'create_links'} //= 1;`.
#
# 1. A genuine COMPILER CRASH: `||=`/`&&=`/`//=` (short-circuit
#    compound assignment) on a hash element, array element, deref'd
#    hash/array element (`$ref->{k}`/`$ref->[i]`), or plain scalar
#    deref (`$$ref`) as the LHS -- emitLValue() had no case at all for
#    any of these node kinds, so its `default: return nullptr;` fired;
#    NK::CompoundAssign's short-circuit branch created its two LLVM
#    basic blocks *before* checking whether emitLValue succeeded, so
#    the null-return early-exit left both blocks permanently
#    registered with no instructions and no terminator -- a hard
#    "Basic Block ... does not have a terminator!" LLVM verify-error
#    crash on ANY of these common LHS shapes. Fixed by adding proper
#    emitLValue cases for all four, AND reordering so lvalue
#    resolution happens before block creation (a general defensive fix
#    for any future unsupported lvalue kind, not just these four).
# 2. Found while verifying (1)'s fix: a SEPARATE, pre-existing
#    interpolation bug -- a QUOTED hash key inside string
#    interpolation ("$h{'key'}", "$h{\"key\"}") was being used as the
#    literal, quote-included text (the 3-character string "'key'")
#    instead of the quoted string's actual VALUE, so
#    "$opts{'create_links'}" silently interpolated as empty/undef
#    instead of the real value. Fixed by stripping one matching layer
#    of quotes from an interpolated subscript's key text before
#    treating it as a bareword string (applied at all 4 duplicate
#    scanner sites: plain $name{key}, $Pkg::name{key}, $::name{key},
#    and the $$name{key}/${name}{key} deref forms).
my %opts = ();

# ||= / &&= / //= on a plain named-hash element
$opts{a} //= 10;
print "hash_defor_first=$opts{a}\n";
$opts{a} //= 20;
print "hash_defor_second=$opts{a}\n";  # unchanged, already defined
$opts{b} = 0;
$opts{b} ||= 5;
print "hash_or_zero=$opts{b}\n";       # 0 is false, so assigned
$opts{c} = 1;
$opts{c} &&= 99;
print "hash_and=$opts{c}\n";           # true, so assigned

# //= / ||= on a named-array element
my @arr = (0, undef, 3);
$arr[0] ||= 7;
print "arr_or=$arr[0]\n";
$arr[1] //= 8;
print "arr_defor=$arr[1]\n";
$arr[2] //= 999;
print "arr_defor_unchanged=$arr[2]\n";

# //= on a ref-deref'd hash/array element (the exact real-world shape)
my $href = {};
$href->{x} //= 42;
print "ref_hash_defor=$href->{x}\n";
my $aref = [0, 0];
$aref->[1] ||= 77;
print "ref_arr_or=$aref->[1]\n";

# //= on a plain scalar deref ($$ref)
my $inner = undef;
my $sref = \$inner;
$$sref //= 55;
print "scalar_deref_defor=$$sref\n";

# regression: non-short-circuit compound ops on the same lvalue shapes
# (+= etc.) must be unaffected
my %nums = (n => 10);
$nums{n} += 5;
print "hash_plusassign=$nums{n}\n";

# (2): quoted-key interpolation, single and double quotes, bareword
# regression, and a deref form
my %h2 = ('create_links' => 1, plain => 2);
print "quoted_single=$h2{'create_links'}\n";
print "quoted_double=$h2{\"create_links\"}\n";
print "bareword_regression=$h2{plain}\n";
my $h2ref = \%h2;
print "deref_quoted=$h2ref->{'create_links'}\n";

print "done\n";
