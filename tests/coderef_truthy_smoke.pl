my $cr = sub { 1 };
print(($cr) ? "truthy\n" : "falsy\n");
use JSON::PP;
my $json = JSON::PP->new->utf8;
$json->canonical;
print ref($json), "\n";
