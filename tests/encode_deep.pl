use Encode qw(encode decode encode_utf8 decode_utf8 from_to encodings
              find_encoding is_utf8 FB_CROAK);

print "ascii=", encode("ascii", "ABC"), "\n";
my $latin = encode("iso-8859-1", "ABC");
print "latin_hex=", join("", map { sprintf("%02x", ord($_)) } split(//, $latin)), "\n";
print "round=", decode("iso-8859-1", $latin), "\n";

my $u = encode_utf8("ABC");
print "utf8=", $u, " flag=", (is_utf8($u) ? 1 : 0), "\n";
my $ch = decode_utf8("ABC");
print "dec_utf8=$ch is_utf8=", (is_utf8($ch) ? 1 : 0), "\n";

my $s = "ABC";
my $n = from_to($s, "ascii", "iso-8859-1");
print "from_to_len=$n s=$s\n";

my @e = encodings();
print "has_utf8=", ((grep { $_ eq "utf8" || $_ eq "UTF-8" } @e) ? 1 : 0), "\n";

my $enc = find_encoding("utf8");
print "find=", $enc->name, "\n";
print "via=", $enc->decode($enc->encode("Hi")), "\n";

print "FB_CROAK=", 0+FB_CROAK, "\n";

eval { encode("ascii", chr(233), FB_CROAK) };
print "croak=", ($@ ne "" ? 1 : 0), "\n";

print "done\n";
