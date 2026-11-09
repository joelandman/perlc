use strict;
use warnings;
my $h = { z => 1 };
%$h = (aa => "bb");
print "aa=", (defined $h->{aa} ? $h->{aa} : "U"),
      " z=", (defined $h->{z} ? $h->{z} : "U"), "\n";
my @extra = ("opt_name", "Widget");
%$h = (%$h, @extra);
my @ks = sort keys %$h;
print "keys=", join(",", @ks), "\n";
print "name=", $h->{opt_name}, " aa=", $h->{aa}, "\n";
