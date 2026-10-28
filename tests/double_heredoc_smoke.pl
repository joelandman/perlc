# D193: two heredocs in one expression (perlbug / perlthanks).
my $m = 0 ? <<'EOT' : <<'EOT';
no
EOT
yes
EOT
print $m;
