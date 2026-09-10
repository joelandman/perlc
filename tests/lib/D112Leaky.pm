package D112Leaky;

# D112: this file-scope `my $counter` must NOT be visible to (or clobbered
# by) a same-named `my $counter` declared in the main script or in any
# other inlined module — each file-scope `my` needs its own storage.
my $counter = 42;

sub get { return $counter; }
sub bump { $counter++; return $counter; }

1;
