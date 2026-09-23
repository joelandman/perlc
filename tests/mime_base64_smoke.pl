use MIME::Base64;
print "enc=[", encode_base64("foo"), "]\n";
print "enc0=[", encode_base64("foo", ""), "]\n";
print "dec=", decode_base64("Zm9v"), "\n";
print "done\n";
