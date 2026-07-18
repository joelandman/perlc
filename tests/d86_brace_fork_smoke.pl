# D86: print/say {EXPR} filehandle form smoke test
my $tmp = "/tmp/d86_smoke.txt";
open(my $fh_out, ">", $tmp) or die "open: $!";

# print {EXPR} LIST
print {$fh_out} "line1\n";

# say {EXPR} LIST  
say {$fh_out} "line2";

# print {$fh} LIST (scalar variable, not the actual fh)
open(my $fh2, ">", "/tmp/d86_smoke2.txt") or die "open: $!";
print {$fh2} "line3\n";
close($fh2);

close($fh_out);

# Verify with shell
my $expected = "line1\nline2\n";
open(my $fh_in, "<", $tmp) or die "open: $!";
my $got = "";
while (my $line = <$fh_in>) {
    $got .= $line;
}
close($fh_in);

print $got;
print "ok\n";
