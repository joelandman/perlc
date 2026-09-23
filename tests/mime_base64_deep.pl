use MIME::Base64;
use MIME::Base64 qw(encode_base64url decode_base64url);

print "empty=[", encode_base64(""), "]\n";
print "a=[", encode_base64("a", ""), "]\n";
print "ab=[", encode_base64("ab", ""), "]\n";
print "abc=[", encode_base64("abc", ""), "]\n";
print "abcd=[", encode_base64("abcd", ""), "]\n";
my $bin = join("", map { chr($_) } 0..255);
my $e = encode_base64($bin, "");
print "round256=", (decode_base64($e) eq $bin ? 1 : 0), "\n";
print "wrap_nl=", (encode_base64("x" x 80) =~ /\n/ ? 1 : 0), "\n";
print "ws_dec=", decode_base64("Zm 9v\n"), "\n";
print "url=", encode_base64url("\xfb\xff"), "\n";
print "url_round=", (decode_base64url(encode_base64url("hi+")) eq "hi+" ? 1 : 0), "\n";
print "done\n";
