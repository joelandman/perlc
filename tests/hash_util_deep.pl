use Hash::Util qw(lock_keys unlock_keys lock_hash unlock_hash lock_value
                   unlock_value hash_locked hash_unlocked legal_keys
                   hidden_keys lock_keys_plus);

# lock_keys: disallow new keys, allow existing-key writes
my %h = (a => 1, b => 2);
lock_keys(%h);
eval { $h{c} = 3; };
print "add_new_key_died=", ($@ ne "" ? 1 : 0), "\n";
print "add_new_key_msg_ok=", ($@ =~ /^Attempt to access disallowed key 'c' in a restricted hash/ ? 1 : 0), "\n";
$h{a} = 99;
print "existing_key_write_ok=$h{a}\n";

# reading a disallowed key dies; exists() on it does not
eval { my $x = $h{zz}; };
print "read_missing_died=", ($@ ne "" ? 1 : 0), "\n";
print "exists_missing=", (exists $h{zz} ? 1 : 0), "\n";

# delete succeeds even though keys are locked; the key becomes "hidden"
# (still legal, absent) rather than fully forgotten, and can be re-added
delete $h{a};
print "delete_removes_from_keys=", (exists $h{a} ? 1 : 0), "\n";
print "legal_after_delete=", join(",", sort(legal_keys(%h))), "\n";
print "hidden_after_delete=", join(",", sort(hidden_keys(%h))), "\n";
$h{a} = 5;
print "readd_after_delete=$h{a}\n";

# unlock_keys fully lifts the restriction
unlock_keys(%h);
$h{new_key} = 7;
print "after_unlock_keys=$h{new_key}\n";

# lock_keys(%h, LIST) replaces the legal set; must be a superset of
# current keys (checked separately below) or real Perl dies immediately
my %h2 = (x => 1, y => 2);
lock_keys(%h2, qw(x y z w));
eval { $h2{z} = 1; };
print "preauthorized_key_ok=", ($@ eq "" ? 1 : 0), " val=$h2{z}\n";
print "legal_h2=", join(",", sort(legal_keys(%h2))), "\n";

# lock_hash: locks keys AND existing values; new-key vs existing-value
# writes fail with two DIFFERENT messages
my %h3 = (p => 1, q => 2);
lock_hash(%h3);
eval { $h3{p} = 100; };
print "lock_hash_write_existing_died=", ($@ =~ /^Modification of a read-only value attempted/ ? 1 : 0), "\n";
eval { $h3{new} = 1; };
print "lock_hash_write_new_died=", ($@ =~ /^Attempt to access disallowed key 'new'/ ? 1 : 0), "\n";
print "hash_locked_h3=", (hash_locked(%h3) ? 1 : 0), "\n";
unlock_hash(%h3);
$h3{p} = 42;
print "after_unlock_hash=$h3{p}\n";

# lock_value: only the named key's value is read-only; siblings unaffected
my %h4 = (m => 1, n => 2);
lock_value(%h4, "m");
eval { $h4{m} = 5; };
print "lock_value_died=", ($@ =~ /^Modification of a read-only value attempted/ ? 1 : 0), "\n";
$h4{n} = 20;
print "sibling_key_ok=$h4{n}\n";
# lock_value alone does NOT count as hash_locked (real Perl: it only
# tracks whether KEYS are restricted, not individual values)
print "hash_locked_after_lock_value=", (hash_locked(%h4) ? 1 : 0), "\n";
unlock_value(%h4, "m");
$h4{m} = 9;
print "after_unlock_value=$h4{m}\n";

# hash_unlocked is the logical negation
my %h5 = (z => 1);
print "hash_unlocked_fresh=", (hash_unlocked(%h5) ? 1 : 0), "\n";
lock_keys(%h5);
print "hash_unlocked_after_lock=", (hash_unlocked(%h5) ? 1 : 0), "\n";

# locking an empty hash: every subsequent write dies
my %empty;
lock_keys(%empty);
eval { $empty{a} = 1; };
print "empty_locked_write_died=", ($@ ne "" ? 1 : 0), "\n";

# lock_keys_plus adds to (not replaces) the existing legal set, including
# a multi-word qw() list
my %h6 = (a => 1);
lock_keys_plus(%h6, qw(b c));
$h6{b} = 2;
print "plus_write=$h6{b} legal_h6=", join(",", sort(legal_keys(%h6))), "\n";

# legal_keys/hidden_keys on a never-locked hash: legal = current keys,
# hidden = empty
my %h7 = (p => 1, q => 2);
print "legal_unrestricted=", join(",", sort(legal_keys(%h7))), "\n";
print "hidden_unrestricted=", join(",", sort(hidden_keys(%h7))), "\n";

# cloning a value read from a locked hash drops the read-only-ness (it's
# a plain independent scalar now), and copying the whole hash produces
# an unlocked copy
my %h8 = (a => 1);
lock_hash(%h8);
my $copy = $h8{a};
$copy = 12345;
print "clone_independent=$copy orig_unchanged=$h8{a}\n";
my %h8copy = %h8;
print "hash_copy_unlocked=", (hash_locked(%h8copy) ? 1 : 0), "\n";
$h8copy{a} = 999;
print "hash_copy_writable=$h8copy{a}\n";

print "done\n";
