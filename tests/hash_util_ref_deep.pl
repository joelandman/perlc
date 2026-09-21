use Hash::Util qw(lock_ref_keys unlock_ref_keys lock_ref_keys_plus
                  lock_hashref unlock_hashref lock_ref_value unlock_ref_value
                  hashref_locked hashref_unlocked legal_ref_keys hidden_ref_keys
                  lock_hash_recurse unlock_hash_recurse
                  lock_hashref_recurse unlock_hashref_recurse
                  lock_keys);

my $h = {a => 1, b => 2};
lock_ref_keys($h);
eval { $h->{c} = 3; };
print "add_died=", ($@ ne "" ? 1 : 0), "\n";
$h->{a} = 9;
print "write_ok=$h->{a}\n";
print "legal=", join(",", sort(legal_ref_keys($h))), "\n";
unlock_ref_keys($h);
$h->{c} = 3;
print "after_unlock=$h->{c}\n";

my $h2 = {x => 1};
lock_ref_keys_plus($h2, qw(y z));
$h2->{y} = 2;
eval { $h2->{no} = 1; };
print "plus_ok=$h2->{y} plus_died=", ($@ ne "" ? 1 : 0), "\n";

my $h3 = {p => 1};
lock_ref_value($h3, "p");
eval { $h3->{p} = 2; };
print "lock_value_died=", ($@ ne "" ? 1 : 0), " hashref_locked=", (hashref_locked($h3) ? 1 : 0), "\n";
print "hashref_unlocked=", (hashref_unlocked($h3) ? 1 : 0), "\n";
unlock_ref_value($h3, "p");
$h3->{p} = 4;
print "after_unlock_value=$h3->{p}\n";

my $h4 = {a => 1};
lock_hashref($h4);
eval { $h4->{a} = 2; };
print "lock_ref_hash_died=", ($@ ne "" ? 1 : 0), "\n";
unlock_hashref($h4);
$h4->{a} = 5;
print "after_unlock_ref_hash=$h4->{a}\n";

my $nested = {outer => 1, inner => {k => 2, deeper => {z => 3}}};
lock_hashref_recurse($nested);
eval { $nested->{inner}{k} = 9; };
print "hashref_recurse_inner_died=", ($@ ne "" ? 1 : 0), "\n";
eval { $nested->{inner}{deeper}{z} = 9; };
print "hashref_recurse_deep_died=", ($@ ne "" ? 1 : 0), "\n";
unlock_hashref_recurse($nested);
$nested->{inner}{k} = 7;
print "after_unlock_recurse=$nested->{inner}{k}\n";

my %flat = (a => 1, b => {c => 2});
lock_hash_recurse(%flat);
eval { $flat{b}{c} = 1; };
print "flat_recurse_died=", ($@ ne "" ? 1 : 0), "\n";
unlock_hash_recurse(%flat);
$flat{b}{c} = 8;
print "flat_after=$flat{b}{c}\n";

# slices on a locked hash
my %s = (a => 1, b => 2);
lock_keys(%s);
eval { @s{qw(a c)} = (9, 10); };
print "slice_assign_died=", ($@ ne "" ? 1 : 0), "\n";
print "slice_a=$s{a}\n";

print "done\n";
