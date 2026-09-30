use Cpanel::JSON::XS qw(encode_json decode_json);
my $data = { name => "Joe", nums => [1, 2, 3] };
my $back = decode_json(encode_json($data));
print $back->{name}, "\n";
print join(",", @{$back->{nums}}), "\n";
