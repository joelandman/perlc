# D151 smoke: open/close/closedir/chdir/opendir return values match perl.
sub show { my ($name, @v) = @_; print "$name: ", join("|", map { defined $_ ? "[$_]" : "u" } @v), "\n" }
my $bad = "/nonexistent/perlc";
open(my $fh, "<", "/etc/passwd"); my $c1 = close($fh); my $c2 = close($fh);
open(my $f2, "<", $bad); my $c4 = close($f2);
show("close ok/twice/failed-open", $c1, $c2, $c4);
show("chdir", chdir($bad), chdir("/tmp"));
show("opendir", opendir(my $d1, $bad), opendir(my $d2, "/tmp"));
show("closedir", closedir($d2), closedir($d2));
my $o = open(my $f3, "<", "/etc/passwd");
show("open", open(my $f4, "<", $bad), $o, open(my $f5, "<", "/etc/passwd"));
