use feature "say";
#!/usr/bin/perl
use strict;
use warnings;

# Heredocs
my $text = <<END;
Hello, World!
Line two
END
print $text;

# Single-quoted heredoc (no interpolation)
my $name = "Alice";
my $raw = <<'END';
No $name interpolation here
END
print $raw;

# Double-quoted heredoc with interpolation
my $greeting = <<"END";
Hello, $name!
END
print $greeting;

# $_ as default in foreach
my @nums = (1, 2, 3, 4, 5);
foreach (@nums) {
    print "$_\n";
}

# chomp with default $_
my $line = "hello\n";
$_ = $line;
chomp;
print "$_\n";

# Bare s/// on $_
$_ = "Hello World";
s/World/Perl/;
print "$_\n";

# Bare /regex/ match on $_
$_ = "foo bar baz";
if (/bar/) {
    print "matched\n";
}

# local
our $x = 10;
sub with_local {
    local $x = 99;
    return $x;
}
print with_local() . "\n";   # 99
print "$x\n";                 # 10 (restored)
