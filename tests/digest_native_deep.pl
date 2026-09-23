use Digest::MD5 qw(md5 md5_hex md5_base64);
use Digest::SHA qw(sha1_hex sha256_hex sha512_hex sha256_base64);

print "md5_empty=", md5_hex(""), "\n";
print "md5_abc=", md5_hex("abc"), "\n";
print "md5_rawlen=", length(md5("abc")), "\n";
print "md5_b64=", md5_base64("abc"), "\n";

my $m = Digest::MD5->new;
$m->add("a");
$m->add("bc");
my $md5_oo = $m->hexdigest;
print "md5_oo=$md5_oo\n";
my $md5_reset = $m->hexdigest;
print "md5_reset=$md5_reset\n";
$m->add("abc");
my $md5_b64oo = $m->b64digest;
print "md5_b64oo=$md5_b64oo\n";

print "sha1_abc=", sha1_hex("abc"), "\n";
print "sha256_abc=", sha256_hex("abc"), "\n";
print "sha512_abc=", sha512_hex("abc"), "\n";
print "sha256_b64=", sha256_base64("abc"), "\n";

my $s = Digest::SHA->new(256);
$s->add("abc");
my $sha_oo = $s->hexdigest;
print "sha_oo=$sha_oo\n";
print "sha_class=", ref($s), "\n";
my $md5n = Digest::MD5->new;
print "md5_class=", ref($md5n), "\n";
print "done\n";
