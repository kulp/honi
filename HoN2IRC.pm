#!/usr/bin/env perl
use strict;
use warnings;
use feature 'say';

package HoN2IRC;

#use Data::HexDump;
#use Digest::MD5 qw(md5_hex);
#use IO::Socket::INET;
#use List::MoreUtils qw(mesh);
#use LWP::UserAgent;
#use YAML qw(Dump);

use POE qw(
    Component::Server::IRC
    Filter::Stream
    );

#use lib ".";
#use LexerWrapper qw(lex);

our $my_name = q(_h2i);

######################### P O E   E N T R Y   P O I N T ########################

unless (caller) {
    my $client = HoN2IRC->new();
    POE::Kernel->run();
    exit;
}

################################# M E T H O D S ################################

sub new
{
    my ($class, %args) = @_;
    my $self = bless {
        %args,
    } => $class;

    my $ircd = POE::Component::Server::IRC->spawn(
            servername => 'hon2irc.kulp.ch',
            network    => 'HoN2IRCNet',
            antiflood  => 0,    # debugging only
            #auth       => 0,    # debugging only
        );

    POE::Session->create(
            object_states => [
                @{ $self->{object_states} || [] },
                $self => [ qw(
                    _start
                    _default
                    ircd_daemon_join
                    ircd_daemon_part
                    ircd_registered
                ) ] ],
            heap => { ircd => $ircd },
        );

    return $self;
}

sub _start
{
    my ($kernel, $heap) = @_[KERNEL, HEAP];
    say "start";
    $heap->{ircd}->yield('register');
    $heap->{ircd}->add_listener(port => 8889);
    undef;
}

sub ircd_registered
{
    my ($self, $kernel, $heap) = @_[OBJECT, KERNEL, HEAP];

    $heap->{ircd}->add_operator({
            username => $self->{user},
            password => $self->{password},
        });
    $heap->{ircd}->yield(add_spoofed_nick => {
            nick    => $my_name,
            umode   => 'Boi',
            ircname => __PACKAGE__." bot",
        });
    $heap->{ircd}->yield(daemon_cmd_join => $my_name => '&status');
}

sub ircd_daemon_join
{
    my ($kernel, $heap, $user, $chan) = @_[KERNEL, HEAP, ARG0, ARG1];
    my $nick = _nick($user);
    return if $nick eq $my_name;
    say "TODO: join HoN channel";
    $heap->{ircd}->yield(daemon_cmd_join => $my_name => $chan);
    (my $clean = $chan) =~ y/_&#+/ /d;
    $kernel->yield(join_channel => $clean);
}

sub ircd_daemon_part
{
    my ($kernel, $heap, $user, $chan) = @_[KERNEL, HEAP, ARG0, ARG1];
    my $nick = _nick($user);
    return if $nick eq $my_name;
    say "TODO: leave HoN channel";
    $heap->{ircd}->yield(daemon_cmd_part => $my_name => $chan);
}

# Taken from the POE::Component::Server::IRC documentation. For debugging only.
sub _default
{
    my ( $event, $args ) = @_[ ARG0 .. $#_ ];
    print "foo\n";
    print STDOUT "$event: ";
    foreach (@$args) {
        SWITCH: {
            if ( ref($_) eq 'ARRAY' ) {
                print STDOUT "[", join ( ", ", @$_ ), "] ";
                last SWITCH;
            }
            if ( ref($_) eq 'HASH' ) {
                print STDOUT "{", join ( ", ", %$_ ), "} ";
                last SWITCH;
            }
            print STDOUT "'$_' ";
        }
    }

    print STDOUT "\n";
    return 1;    # Don't handle signals.
}

sub _nick { (split /!/, $_[0], 2)[0] }

1;

