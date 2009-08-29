#!/usr/bin/env perl
use strict;

use Parse::RecDescent;
use Perl6::Slurp;
use YAML qw(Dump);
use WWW::Mechanize;

my $grammar = q(
<autotree>

top: val { $return = $item[1]; 1 }

key: val { $return = $item[1]; 1 }

val: hash    { $return = $item[1]; 1 }
   | boolean { $return = $item[1]; 1 }
   | string  { $return = $item[1]; 1 }
   | integer { $return = $item[1]; 1 }

digit: /\d+/ { $return = $item[1]; 1 }

integer: 'i' ':' digit { $return = $item[3]; 1 }

boolean: 'b' ':' digit { $return = $item[3]; 1 }

string: 's' ':' length ':' '"' /[^"]*/ '"' { $return = $item[6]; 1 }

length: digit { $item[1] }

hash: 'a' ':' length ':' '{' (key ';' val)(s? /;?/) (';')(?) '}' { $return = +{ map { @$_{qw(key val)} } grep ref, @{ $item[6] } }; 10 }

);

my $text = slurp;

my $parser = Parse::RecDescent->new($grammar);
my $data = $parser->top($text);
#print Dump $data;

