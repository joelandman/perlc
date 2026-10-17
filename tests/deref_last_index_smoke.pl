D196: indirect array last-index — #$ref and $#{$ref} (last index of an
# array reference). The lexer used to eat the '#' after '$#' as a comment,
# leaving a bare '$' that crashed the parser; a bare expression now returns
# scalar(@$ref) - 1 and interpolation interpolates it the same way.
my @a = (10, 20, 30);
my $ref = \@a;
print "direct: $#$ref\n";
print "braces: $#{$ref}\n";
my @empty = ();
my $er = \@empty;
print "empty: $#$er\n";
print "range: @[0..$#$ref]\n";
