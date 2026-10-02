open(my $fh1, '>', 'glob_brace_smoke_a.gz') or die; close $fh1;
open(my $fh2, '>', 'glob_brace_smoke_b.txt') or die; close $fh2;
print join(",", sort <glob_brace_smoke_*.{gz,txt}>), "\n";
unlink 'glob_brace_smoke_a.gz', 'glob_brace_smoke_b.txt';
