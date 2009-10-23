#!/usr/bin/env perl
use strict;
use warnings;
use feature 'say';

# TODO add event forwarding

package HoNTalker;

use Attribute::Memoize;
use Data::HexDump;
use Digest::MD5 qw(md5_hex);
use IO::Socket::INET;
use List::MoreUtils qw(mesh uniq);
use LWP::UserAgent;
use Sub::Install;
use YAML qw(Dump);

use POE qw(Component::Client::TCP Filter::Stream);

use lib ".";
use ParserWrapper qw(hd2yaml);

################################################################################

my $base = qq(http://masterserver.hon.s2games.com);
my $cr_url = qq($base/client_requester.php);

################################################################################

my %actions = (
    0x00 => 'login_success',
    0x01 => 'server_ping',
    0x03 => 'channel_traffic',
    0x04 => 'channel_presence',
    0x05 => 'channel_join_notice',
    0x06 => 'channel_part_notice',
    0x08 => 'received_whisper',
    0x0B => 'my_presence',
    0x12 => 'notification',
    0x20 => 'received_whisper_all',
    0x2D => 'whois_response',

#    0x05 => 'ignore_message',
#    0x06 => 'ignore_message',
#    0x0C => 'ignore_message',
#    0x18 => 'ignore_message',
);

######################### P O E   E N T R Y   P O I N T ########################

unless (caller) {
    my ($user, $pass, $chan) = @ARGV;
    $user and $pass or die "Supply username and password";

    my $client = HoNTalker->new(
            user     => $user,
            password => $pass,
            abide    => [ $chan || () ],
        );
    POE::Kernel->run();
    exit;
}

################################# M E T H O D S ################################

sub new
{
    my ($class, %args) = @_;
    #my $external_handler = delete $args{event_obj};
    my $self = bless {
        abide      => { map { lc $_ => 1 } @{ delete $args{abide} || [] } },
        keepalive  => delete $args{keepalive} || 30,  # seconds
        recv_count => 0,
        ua         => LWP::UserAgent->new,
        %args,
    } => $class;

    my $data = $self->{me} = $self->_rpc(auth =>
            login    => $self->{user},
            password => md5_hex($self->{password}),
        );

    die "Error: no data\n" unless keys %$data;
    die "Error: $data->{auth}\n" if $data->{auth};

    my $chatter = $data->{chat_url};
    my $port = 11031;

    say "got chat server address $chatter";
    say "connecting to chat server at $chatter:$port";

    POE::Component::Client::TCP->new(
        RemoteAddress => $chatter,
        RemotePort    => $port,
        # Prevent input buffering
        Filter        => "POE::Filter::Stream",
        Disconnected  => sub { die "disconnected"; },
        Connected     => sub { $_[KERNEL]->yield(connected    => @_[ARG0 .. $#_]) },
        ServerInput   => sub { $_[KERNEL]->yield(server_input => @_[ARG0 .. $#_]) },
        #ServerFlushed => sub { say "server flushed"; },
        ServerError   => sub { say "server error"; },
        ObjectStates  => [
            #($external_handler ? ($external_handler => [ qw(
            #    _default
            #) ]) : ()),
            @{ $self->{object_states} || [] },
            $self => $self->{actions} = [ uniq qw(
                _process_message
                _dispatch
                _action_someone

                channel_presence
                channel_traffic
                check_user
                connected
                ignore_message
                join_channel
                keepalive
                login_success
                my_presence
                new_banned
                new_buddy
                new_ignored
                part_channel
                remove_banned
                remove_buddy
                remove_ignored
                say_in_channel
                server_input
                whisper_all_friends
                whisper_user
                whois_response
            ), values %actions ],
        ],
    );

    return $self;
}

sub actions
{
    return @{ shift->{actions} };
}

sub id2fullnick : Memoize
{
    my ($self, $userid) = @_;
    my $nick = $self->id2nick($userid);
    my $user = $self->id2info($userid);
    my $tag = $user->{clan_info}{$userid}{tag};
    my $who = $tag ? "[$tag]$nick" : $nick;

    return $who;
}

sub nick2id : Memoize
{
    my $self = shift;
    my $i = 0;
    my %results = $self->_rpc(nick2id => map { "nickname[" . $i++ . "]" => $_ } @_);

    # TODO use Contextual::Return here
    return \%results;
}

# Not memoized : needs to be fresh data
sub id2info
{
    my $self = shift;
    my $i = 0;
    my %results = $self->_rpc(get_all_stats => map { "account_id[" . $i++ . "]" => $_ } @_);

    # TODO use Contextual::Return here
    return \%results;
}

sub id2nick : Memoize
{
    my ($self, $id) = @_;
    return $self->id2info($id)->{all_stats}->{$id}->{nickname};
}

sub friends
{
    my ($self) = @_;
    my $me = $self->{me};
    my $hash = $me->{buddy_list}{$me->{account_id}};
    my @buddies = @$hash{sort { $a <=> $b } keys %$hash};
    # TODO use Contextual::Return here
    return @buddies;
}

################################## E V E N T S #################################

# TODO move newly-minted events to their own section, away from these so-called
# "utility subs"
# eats a message from a string and calls the passed sub on it
sub _process_message
{
    my ($self, $kernel, $msgref, $code) = @_[OBJECT, KERNEL, ARG0, ARG1];

    die "Login failed" if $self->{recv_count} > 1 && !$self->{logged_in};

    my ($len) = unpack "V", $$msgref;
    my $this = substr($$msgref, 4, $len);
    $$msgref = substr($$msgref, $len + 4);

    $kernel->yield($code => $this);
}

sub my_presence
{
    say "chat connection confirmed";
    my ($self, $kernel, $message) = @_[OBJECT, KERNEL, ARG0];
    my ($code, $count) = unpack "C V", $message;
    for my $i (0 .. $count - 1) {
        my ($id, $flags) = unpack "V v", substr($message, 5 + ($i * 6), 6);
        printf "    id %u, flags %#x\n", $id, $flags;
    }
}

sub channel_presence
{
    my ($self, $kernel, $message) = @_[OBJECT, KERNEL, ARG0];
    my $pos = 1; # skip code
    my ($channel, $chanid, undef, $welcome, $specials) = unpack "Z* V C Z* V", substr($message, 1);

    $self->{chan2id}{$channel} = $chanid;
    $self->{id2chan}{$chanid}  = $channel;

    $pos += length($channel) + 1 + 4 + 1 + length($welcome) + 1 + 4;
    say "I ($self->{user}) am in channel '$channel'";
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

        $self->{nick2id}{$name} = $id;
        $self->{id2nick}{$id}   = $name;

        printf "        name=%-16s, id=%7d, flags=0x%04x\n", $name, $id, $flags;
        $pos += length($name) + 1 + 4 + 2;
        #$kernel->yield(h2i_user_join_channel => $name, $channel);
        $kernel->post($self->{bridge}, h2i_user_join_channel => $name, $channel);
    }

    #$kernel->yield(part_channel => $channel) unless $self->{abide}{lc $channel}
    $kernel->delay(part_channel => 10, $channel) unless $self->{abide}{lc $channel}
}

sub server_ping
{
    my ($self, $kernel, $message) = @_[OBJECT, KERNEL, ARG0];
    my ($code, $val) = unpack "C V", $message;
    if ($val == 1) {
        $kernel->yield('keepalive');
    } else {
        say "unknown value $val received in ping packet";
    }
}

sub channel_traffic
{
    my ($self, $kernel, $message) = @_[OBJECT, KERNEL, ARG0];
    my ($userid, $chanid, $saying) = unpack "x V V Z*", $message;
    my $who = $self->id2fullnick($userid);
    # TODO make id2chan method like id2nick
    my $where = $self->{id2chan}{$chanid} || "id $chanid";
    say "user $who said in channel $where : '$saying'";
    $kernel->post($self->{bridge}, h2i_user_said_in_channel => $who, $where, $saying);
}

sub received_whisper
{
    my ($self, $kernel, $data) = @_[OBJECT, KERNEL, ARG0];
    my ($code, $speaker, $message) = unpack "C Z* Z*", $data;
    say "user $speaker whispered to me: $message";
    $kernel->post($self->{bridge}, h2i_user_whispered_to_me => $speaker, $message);
}

sub received_whisper_all
{
    my ($self, $kernel, $data) = @_[OBJECT, KERNEL, ARG0];
    my ($code, $speaker, $message) = unpack "C Z* Z*", $data;
    say "user $speaker whispered to all his friends: $message";
    $kernel->post($self->{bridge}, h2i_user_whispered_to_friends => $speaker, $message);
}

sub whois_response
{
    say "whois reponse";
    my ($self, $kernel, $message) = @_[OBJECT, KERNEL, ARG0];
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
    my ($self, $kernel, $message) = @_[OBJECT, KERNEL, ARG0];
    my $key = ord $message;
    if (my $code = $actions{$key}) {
        $kernel->yield($code, $message);
    } else {
        print HexDump($message);
    }
}

sub login_success
{
    my ($self, $kernel) = @_[OBJECT, KERNEL];
    say "login successful";
    $self->{logged_in} = 1;
    my @friends = map {
        my $tag  = $_->{clan_tag};
        my $nick = $_->{nickname};
        $tag ? "[$tag]$nick" : $nick
    } $self->friends;
    # TODO accurate status
    $kernel->post($self->{bridge}, h2i_friend_status => $_, 1) for @friends;
}

sub connected
{
    my ($self, $kernel, $heap) = @_[OBJECT, KERNEL, HEAP];
    say "authenticating";
    my $auth = pack "C V Z* V", 0xff, $self->{me}{account_id}, $self->{me}{cookie}, 2;
    ($self->{tcp} ||= $heap->{server})->put($auth);
}

sub server_input
{
    my ($self, $kernel, $input) = @_[OBJECT, KERNEL, ARG0];
    #say "packet received ($self->{recv_count})";

    if ($self->{recv_count}++ == 0) {
        $kernel->delay(keepalive => $self->{keepalive});
        $kernel->yield(join_channel => $_) for keys %{ $self->{abide} };
    }

    while (length($input) > 0) {
        $kernel->call($_[SESSION], _process_message => \$input, '_dispatch');
        die $! if $!;
    }
}

sub check_user
{
    my ($self, $kernel, $user) = @_[OBJECT, KERNEL, ARG0];
    my $server = $self->{tcp};

    say "checking user $user";

    $server->put("*$user\0");
}

sub keepalive
{
    my ($self, $kernel) = @_[OBJECT, KERNEL];
    say "keepalive";
    $self->{tcp}->put(chr 2);
    $kernel->delay(keepalive => $self->{keepalive});
}

# works for $type in qw(buddy ignored banned)
sub _action_someone
{
    my ($self, $kernel, $action, $type, $who) = @_[OBJECT, KERNEL, ARG0, ARG1, ARG2];
    my $verb = "${action}_${type}";
    my $server = $self->{tcp};

    say "performing $action on $who";

    my $id = $self->nick2id($who)->{$who};
    warn $id;
    my $me = $self->{me};
    warn $me->{account_id};
    warn $me->{cookie};
    my %h = $self->_rpc($verb =>
            account_id   => $me->{account_id},
            "${type}_id" => $id,
            cookie       => $me->{cookie},
        );

    if ($h{$verb} ne "OK") {
        say "Unexpected response $h{$action} while performing $action on $type $who";
        return;
    }

    # TODO determine if the response to the chat server is
    my ($n1, $n2) = @{ $h{notification} }{qw(1 2)};
    # TODO determine whether the 0x0d changes per $action or $type
    my $packed = pack "C V V V", 0x0d, $id, $n1, $n2;
    warn HexDump($packed);
    # TODO
    $server->put($packed);
}

for my $action (qw(new remove)) {
    for my $type (qw(buddy ignored banned)) {
        Sub::Install::install_sub({
                code => sub {
                    my ($self, $kernel, $who) = @_[OBJECT, KERNEL, ARG0];
                    $kernel->yield(_action_someone => $action => $type => $who);
                },
                as => "${action}_${type}",
            });
    }
}

sub join_channel
{
    my ($self, $kernel, $chan) = @_[OBJECT, KERNEL, ARG0];
    return if $self->{inchans}{lc $chan};
    say "joining channel '$chan'";
    $self->{abide  }{lc $chan} = 1;
    $self->{inchans}{lc $chan} = 1;
    $self->{tcp}->put(pack "C Z*", 0x1E, $chan);
}

sub part_channel
{
    my ($self, $kernel, $chan) = @_[OBJECT, KERNEL, ARG0];
    say "leaving channel '$chan'";
    $self->{inchans}{lc $chan} = 0;
    $self->{tcp}->put(pack "C Z*", 0x22, $chan);
}

sub channel_join_notice
{
    my ($self, $kernel, $message) = @_[OBJECT, KERNEL, ARG0];
    my ($code, $user, $userid, $chanid, $flags) = unpack "C Z* V V v", $message;
    #$kernel->yield(hon_joined_channel => $userid, $chanid);
    my $chan = $self->{id2chan}{$chanid};
    $kernel->post($self->{bridge}, h2i_user_join_channel => $user, $chan);

    say "User $user joined channel $chan";
}

sub channel_part_notice
{
    my ($self, $kernel, $message) = @_[OBJECT, KERNEL, ARG0];
    my ($code, $userid, $chanid) = unpack "C V V", $message;
    #$kernel->yield(hon_parted_channel => $userid, $chanid);

    my $user = $self->id2nick($userid);
    say "User $user left channel $self->{id2chan}{$chanid}";
    my $chan = $self->{id2chan}{$chanid};
    $kernel->post($self->{bridge}, h2i_user_part_channel => $user, $chan);
}

sub say_in_channel
{
    my ($self, $kernel, $chan, $message) = @_[OBJECT, KERNEL, ARG0, ARG1];
    say "looking up chanid for $chan";
    my $chanid = $self->{chan2id}{$chan} || return;
    say "putting message $message to chanid $chanid";
    $self->{tcp}->put(pack "C Z* V", 0x03, $message, $chanid);
}

sub whisper_user
{
    my ($self, $kernel, $user, $message) = @_[OBJECT, KERNEL, ARG0, ARG1];
    say "whispering message $message to user $user";
    $self->{tcp}->put(pack "C Z* Z*", 0x08, $user, $message);
}

sub whisper_all_friends
{
    my ($self, $kernel, $message) = @_[OBJECT, KERNEL, ARG0];
    say "whispering message $message to all friends";
    $self->{tcp}->put(pack "C Z*", 0x20, $message);
}

sub notification
{
    my ($self, $kernel, $message) = @_[OBJECT, KERNEL, ARG0];
    my ($code, $yaubs, $string) = unpack "C C Z*", $message;
    say "received notification '$string'";
    $kernel->post($self->{bridge}, h2i_general_notice => $string);
}

############################ U T I L I T Y   S U B S ###########################

# TODO convert to an event ?
sub _rpc
{
    my ($self, $method, @args) = @_;
    my $response = $self->{ua}->post($cr_url, {
        f => $method,
        @args,
    });

    my $data = hd2yaml($response->content);
    delete $data->{0}; # I don't understand this extra top-level key
    return wantarray ? %$data : $data;
}

1;

