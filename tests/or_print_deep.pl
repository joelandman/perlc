# D194: || / && / or print, the value print returns, and a scalar filehandle.
sub drop { 0 }
sub keep { 1 }
my $buf;
open LOG, ">", \$buf;
drop() || print LOG "a";
keep() || print LOG "b";
keep() && print LOG "c";
drop() && print LOG "d";
drop() or print LOG "e";
my $r = drop() || print LOG "f";
print $buf, "[$r]\n";
*OUT = *STDOUT;
drop() || print OUT "std\n";
while (defined (my $error = <DATA>)) {
    ($error =~ /keep/) || print "row:$error";
}
__DATA__
drop
keep
