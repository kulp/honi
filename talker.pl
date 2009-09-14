#!/usr/bin/env perl
use strict;
use warnings;
use feature 'say';

use Data::HexDump;
use Digest::MD5 qw(md5_hex);
use IO::Socket::INET;
use List::MoreUtils qw(mesh);
use LWP::UserAgent;
use YAML qw(Dump);

use POE qw(Component::Client::TCP Filter::Stream);

use lib ".";
use LexerWrapper qw(lex);

################################################################################

my $base = qq(http://masterserver.hon.s2games.com);
my $cr_url = qq($base/client_requester.php);

my $keepalive_period = 30;  # seconds
my $home_channel = "UWEC";

my ($user, $pass) = @ARGV;
$user and $pass or die "Supply username and password";

my $ua = LWP::UserAgent->new;

################################################################################

my %actions = (
    0x00 => sub { say "login successful" },
    0x04 => \&channel_presence,
    0x0B => \&my_presence,
    0x2D => \&whois_response,
    0x03 => \&channel_traffic,

    0x05 => \&ignore_message,
    0x06 => \&ignore_message,
    0x0C => \&ignore_message,
    0x18 => \&ignore_message,
);

################################################################################

my $data = _rpc(auth => login => $user, password => md5_hex($pass));

die "Error: $data->{auth}\n" if $data->{auth};

my $chatter = $data->{chat_url};
my $port = 11031;

say "Got chat server address $chatter:$port";
#say "Sleeping a few seconds to allow database quiescence";
#sleep 1;
say "Connecting to chat server at $chatter";

POE::Component::Client::TCP->new(
    RemoteAddress => $chatter,
    RemotePort    => $port,
    # Prevent input buffering
    Filter        => "POE::Filter::Stream",
    Disconnected  => sub { say "disconnected"; },
    Connected     => \&connected,
    ServerInput   => \&server_input,
    #ServerFlushed => sub { say "server flushed"; },
    ServerError   => sub { say "server error"; },
    InlineStates => {
        check_user    => \&check_user,
        add_buddy     => \&add_buddy,
        keepalive     => \&keepalive,
        join_channel  => \&join_channel,
        leave_channel => \&leave_channel,
    },
);

POE::Kernel->run();
exit;

############################ U T I L I T Y   S U B S ###########################

sub _rpc
{
    my ($method, @args) = @_;
    my $response = $ua->post($cr_url, {
        f => $method,
        @args,
    });

    my $data = lex($response->content);
    delete $data->{0}; # I don't understand this extra top-level key
    return wantarray ? %$data : $data;
}

# TODO replace with a regular handler for 0x00 command code
sub _check_login_response
{
    my ($data) = @_;
    if (length($data) == 5 && $data eq pack "H*", "0100000000") {
        return 0;
    } else {
        die "Unexpected login response packet";
    }
}

{ my %cache; # cached names
sub _nick2id
{
    my $i = 0;
    my @need = grep !$cache{$_}, @_;
    my @have = grep  $cache{$_}, @_;
    my %results = mesh @have, @{[ @cache{@have} ]};
    if (@need) {
        %results = (
            %results,
            _rpc(nick2id => map { "nickname[" . $i++ . "]" => $_ } @need)
        );
    }

    return \%results;
}
}

{ my $seq;
# I'm not sure what the purpose of this sequence number is, or even if it is
# a sequence number, but it appears to be a monotonically increasing
# non-time-linear sequence that is used at least for buddy management. We fake
# one.
sub _seq_num
{
    $seq ||= time - 1234567890;
    return $seq += 2;
}
}

# eats a message from a string and calls the passed sub on it
sub _process_message
{
    my ($messages, $code) = @_;
    my ($len) = unpack "V", $messages;
    my $this = substr($messages, 4, $len);
    $_[0]    = substr($messages, $len + 4);

    return $code->($this);
}

sub my_presence
{
    say "chat connection confirmed";
    my ($message) = @_;
    my ($code, $count) = unpack "C V", $message;
    for my $i (0 .. $count - 1) {
        my ($id, $flags) = unpack "V v", substr($message, 5 + ($i * 6), 6);
        printf "    id %u, flags %#x\n", $id, $flags;
    }
}

sub channel_presence
{
    my ($message) = @_;
    my $pos = 1; # skip code
    my ($channel, $chanid, undef, $welcome, $specials) = unpack "Z* V C Z* V", substr($message, 1);
    $pos += length($channel) + 1 + 4 + 1 + length($welcome) + 1 + 4;
    say "I ($user) am in channel '$channel'";
    say "    special user(s) registered in this channel ($specials):";

    for my $i (0 .. $specials - 1) {
        my ($id, $flags) = unpack "V C",
                                substr($message, $pos, 5);
        printf "        special user id %u has flags %#x\n", $id, $flags;
        $pos += 5;
    }

    my ($count) = unpack "V", substr($message, $pos, 4);
    $pos += 4;
    say "    there are $count users in channel";

    for my $i (0 .. $count - 1) {
        my ($name, $id, $flags) = unpack "Z* V v",
                                    substr($message, $pos);
        printf "        name=%-16s, id=%7d, flags=0x%04x\n", $name, $id, $flags;
        $pos += length($name) + 1 + 4 + 2;
    }
}

sub channel_traffic
{
    my ($message) = @_;
    my ($userid, $chanid, $saying) = unpack "x V V Z*", $message;
    say "user id $userid said in channel id $chanid : '$saying'";
}

sub whois_response
{
    say "whois reponse";
    my ($message) = @_;
    my $pos = 0;
    my ($code, $name) = unpack "C Z*", $message;
    $pos += 1 + length($name) + 1;
    if ($code == 0x2D) {
        say "user $name is in these channels:";
        my ($count) = unpack "V", substr($message, $pos, 4);
        $pos += 4;
        for my $i (0 .. $count - 1) {
            my ($chan) = unpack "Z*", substr($message, $pos);
            say "    '$chan'";
            $pos += length($chan) + 1;
        }
    }
}

sub ignore_message
{
    #printf "ignoring message code %#02x\n", ord shift;
}

sub _dispatch
{
    my ($message) = @_;
    my $key = ord $message;
    if (my $code = $actions{$key}) {
        return $code->($message);
    } else {
        print HexDump($message);
    }
}

######################### T C P   C L I E N T   S U B S ########################

sub connected
{
    say "authenticating";
    my $auth = pack "C V Z* V", 0xff, $data->{account_id}, $data->{cookie}, 2;
    $_[HEAP]{server}->put($auth);
}

sub server_input
{
    my $input = $_[ARG0];
    #say "packet received";

    if ($_[HEAP]{recv_count}++ == 0) {
        _check_login_response($input);

        $_[KERNEL]->delay(keepalive => $keepalive_period);
        $_[KERNEL]->yield(join_channel => $home_channel);
    }

    while (length($input) > 0) {
        _process_message($input, \&_dispatch);
    }
}

############################## P O E   E V E N T S #############################

sub check_user
{
    my ($heap, $user) = @_[HEAP, ARG0];
    my $server = $heap->{server};

    say "checking user $user";

    $server->put("*$user\0");
}

sub keepalive
{
    say "keepalive";
    $_[HEAP]{server}->put(chr 2);
    $_[KERNEL]->delay(keepalive => $keepalive_period);
}

sub add_buddy
{
    my ($heap, $buddy) = @_[HEAP, ARG0];
    my $server = $heap->{server};

    say "adding buddy $buddy";

    my $id = _nick2id($buddy)->{$buddy};
    my $seq = _seq_num();
    # YAUBS
    my $packed = pack "C V V V", 0x0d, $id, $seq, $seq + 1;
    warn HexDump($packed);
    # TODO
    $server->put($packed);
}

sub join_channel
{
    my ($heap, $chan) = @_[HEAP, ARG0];
    say "joining channel '$chan'";
    $_[HEAP]{server}->put(pack "C Z*", 0x1E, $chan);
}

sub leave_channel
{
    my ($heap, $chan) = @_[HEAP, ARG0];
    say "leaving channel '$chan'";
    $_[HEAP]{server}->put(pack "C Z*", 0x22, $chan);
}

