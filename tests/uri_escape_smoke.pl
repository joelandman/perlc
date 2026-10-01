use URI::Escape;
print uri_escape("hello world!"), "\n";
print uri_unescape("hello%20world%21"), "\n";
