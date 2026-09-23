use Digest::MD5 qw(md5_hex);
use Digest::SHA qw(sha256_hex);
print "md5=", md5_hex("abc"), "\n";
print "sha256=", sha256_hex("abc"), "\n";
my $d = Digest::MD5->new;
$d->add("abc");
my $oo = $d->hexdigest;
print "oo=$oo\n";
print "done\n";
