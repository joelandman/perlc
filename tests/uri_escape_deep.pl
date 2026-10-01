use URI::Escape;

# default pattern: unreserved + reserved mixed with high-bit bytes
print uri_escape("abc-._~XYZ019 !@#\$%^&*()\x80\xff"), "\n";

# custom pattern: only escape lowercase letters
print uri_escape("AbC123", "a-z"), "\n";

# custom pattern: negated class -- escape everything that's NOT a letter
print uri_escape("ab 12", "^A-Za-z"), "\n";

# undef in, undef out
my $u = uri_escape(undef);
print defined($u) ? "defined" : "undef", "\n";

# uri_escape_utf8 on a non-utf8-flagged high-byte string
my $t1 = "caf\xe9";
print uri_escape_utf8($t1), "\n";

# uri_escape_utf8 on a genuinely multi-byte (use utf8) string
{
    use utf8;
    my $t2 = "café";
    print uri_escape_utf8($t2), "\n";
}

# round trip
my $orig = "hello world & more=stuff?";
my $enc = uri_escape($orig);
print $enc, "\n";
print uri_unescape($enc), "\n";

# %-not-followed-by-2-hex-digits should not be unescaped
print uri_unescape("100% done %2 %xy %41"), "\n";
