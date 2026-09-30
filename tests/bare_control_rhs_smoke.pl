# D155 smoke: bare `die` / `return` as the last thing before `}` on the RHS of
# or/||/&& (`open(...) or die }`, `$x || return }`) were parse errors.
sub a { open(my $f, "<", "/nonexistent") or return "ret" } print a(), "\n";
sub b { 0 || return } my @r = b(); print scalar(@r), "\n";
eval { 0 or die }; print $@ =~ /^Died/ ? "died\n" : "no\n";
eval { my $x = 0 || die }; print $@ =~ /^Died/ ? "died2\n" : "no\n";
sub c { return } print defined(c()) ? "d" : "u", "\n";
for my $i (1..3) { $i == 2 || next; print $i; } print "\n"; for my $i (1..3) { $i < 2 && next; $i == 3 && last; print $i; } print "\n";
sub opener { open(my $fh, "<", "/nonexistent") or return "no-file" } print opener(), "\n";
