use File::Temp qw(tempfile);
use Pod::Usage qw(pod2usage);

my ($fh, $path) = tempfile();
print $fh "=head1 SYNOPSIS\n\nfoo --bar\n\n=head1 OPTIONS\n\n--bar  do bar\n\n=head1 DESCRIPTION\n\nhello\n\n=cut\n";
close $fh;

print "---v0---\n";
pod2usage(-verbose => 0, -exitval => 'noexit', -message => 'MSG', -input => $path);
print "---emptyin---\n";
pod2usage(-verbose => 0, -exitval => 'noexit', -message => 'ONLY', -input => '/dev/null');
print "---hash---\n";
pod2usage({ -verbose => 0, -exitval => 'noexit', -message => 'H', -input => $path });
print "done\n";
