use strict;
use warnings;
sub hexbytes {
    my ($s) = @_;
    my $h = "";
    for (my $i = 0; $i < length($s); $i++) { $h .= sprintf("%02x", ord(substr($s, $i, 1))); }
    return $h;
}
my $bom = "\x{FEFF}";
utf8::encode($bom);
print "bom=", hexbytes($bom), " len=", length($bom), "\n";
my $again = $bom;
my $ok = utf8::decode($again);
print "dec=", ($ok ? 1 : 0), " clen=", length($again), "\n";
utf8::encode($again);
print "re=", hexbytes($again), " blen=", length($again), "\n";
print "nat=", utf8::unicode_to_native(0xA0), "\n";
print "uni=", utf8::native_to_unicode(0xAD), "\n";
print "ord=", ord(chr(utf8::unicode_to_native(0xA0))), "\n";

my $ascii = "abc";
print "da=", (utf8::decode($ascii) ? 1 : 0), " alen=", length($ascii), "\n";
utf8::encode($ascii);
print "ae=", length($ascii), "\n";

my $bad = "\xFF";
print "bad=", (utf8::decode($bad) ? 1 : 0), " bl=", length($bad), "\n";
my $over = "\xC0\x80";
print "over=", (utf8::decode($over) ? 1 : 0), " ol=", length($over), "\n";
