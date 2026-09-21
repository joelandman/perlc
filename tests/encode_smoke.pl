use Encode qw(encode decode encode_utf8 decode_utf8);
my $b = encode("iso-8859-1", "A");
print "enc=", sprintf("%02x", ord($b)), " len=", length($b), "\n";
print "dec=", decode("iso-8859-1", $b), "\n";
print "done\n";
