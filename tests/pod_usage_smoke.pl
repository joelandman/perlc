use File::Temp qw(tempfile);
use Pod::Usage;
my ($fh, $path) = tempfile();
print $fh "=head1 SYNOPSIS\n\ndemo --help\n\n=cut\n";
close $fh;
pod2usage(-verbose => 0, -exitval => 'noexit', -message => 'MSG', -input => $path);
print "after\n";
