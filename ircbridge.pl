#!/usr/bin/env perl
use strict;
use warnings;
use feature 'say';

use YAML qw(Dump);

use POE;

use lib ".";
use HoNTalker;
use HoN2IRC;

my ($user, $pass, $chan) = @ARGV;
$user and $pass or die "Supply username and password";

my $h2i = HoN2IRC->new(
        user     => $user,
        password => $pass,
    );
my $tkr = HoNTalker->new(
        user      => $user,
        password  => $pass,
        abide     => [ $chan || () ],
        #object_states => [ $h2i => [ qw(_default) ] ],
    );

my $bridge = POE::Session->create(
        package_states => [
            main => [ qw(
                _start
            ) ],
        ],
        object_states => [
            $h2i => [ qw(
                ircd_daemon_join
                ircd_daemon_part
                ircd_daemon_public
                ircd_daemon_privmsg
                ircd_registered

                h2i_user_join_channel
                h2i_user_part_channel
                h2i_user_said_in_channel
                h2i_user_whispered_to_me
                h2i_user_whispered_to_friends
                h2i_general_notice
                h2i_friend_status

                dispatch_command
            ) ],
            $tkr => [ $tkr->actions ],
        ],
    );

$tkr->{bridge} = $bridge;

POE::Kernel->run;
exit;

sub _start
{
    my ($kernel) = $_[KERNEL];
    say "start";
    $h2i->{ircd}->yield('register');
    #$kernel->yield(h2i_user_join_channel => "myself_and_i", "hon_3");
    undef;
}

