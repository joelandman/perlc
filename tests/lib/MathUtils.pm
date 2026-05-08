package MathUtils;

sub new {
    my ($class) = @_;
    return bless {}, $class;
}

sub add {
    my ($self, $a, $b) = @_;
    return $a + $b;
}

sub multiply {
    my ($self, $a, $b) = @_;
    return $a * $b;
}

sub factorial {
    my ($self, $n) = @_;
    if ($n <= 1) { return 1; }
    return $n * $self->factorial($n - 1);
}

1;
