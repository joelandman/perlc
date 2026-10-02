my @names = ('unlink_list_smoke_a.tmp', 'unlink_list_smoke_b.tmp');
for my $n (@names) { open(my $fh, '>', $n) or die; close $fh; }
unlink @names;
print join(",", map { -e $_ ? "yes" : "no" } @names), "\n";
