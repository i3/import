#!perl
# vim:ts=4:sw=4:expandtab
#
# Please read the following documents before working on tests:
# • https://build.i3wm.org/docs/testsuite.html
#   (or docs/testsuite)
#
# • https://build.i3wm.org/docs/lib-i3test.html
#   (alternatively: perldoc ./testcases/lib/i3test.pm)
#
# • https://build.i3wm.org/docs/ipc.html
#   (or docs/ipc)
#
# • http://onyxneon.com/books/modern_perl/modern_perl_a4.pdf
#   (unless you are already familiar with Perl)

use i3test;
use List::Util qw(first);

# to not depend on List::MoreUtils
sub all (&@) {
    my $cb = shift;
    for (@_) {
        return 0 unless $cb->();
    }
    return 1;
}

sub none (&@) {
    my $cb = shift;
    for (@_) {
        return 0 if $cb->();
    }
    return 1;
}

my $i3 = i3(get_socket_path());

####################
# Request tree
####################

my $tree = $i3->get_tree->recv;

# a unique value
my $ignore = \"";

my $expected = {
    fullscreen_mode => 0,
    sticky => $ignore,
    nodes => $ignore,
    window => undef,
    name => 'root',
    orientation => $ignore,
    type => 'root',
    window_type => undef,
    id => $ignore,
    rect => $ignore,
    deco_rect => $ignore,
    window_rect => $ignore,
    geometry => $ignore,
    swallows => $ignore,
    percent => undef,
    layout => 'splith',
    layout_fill_order => 'default',
    floating => 'auto_off',
    last_split_layout => 'splith',
    scratchpad_state => 'none',
    focus => $ignore,
    focused => JSON::XS::false,
    urgent => JSON::XS::false,
    border => 'normal',
    'floating_nodes' => $ignore,
    workspace_layout => 'default',
    current_border_width => -1,
    marks => $ignore,
};

# a shallow copy is sufficient, since we only ignore values at the root
my $tree_copy = { %$tree };

for (keys %$expected) {
    my $val = $expected->{$_};

    # delete unwanted keys, so we can use is_deeply()
    if (ref($val) eq 'SCALAR' and $val == $ignore) {
        delete $tree_copy->{$_};
        delete $expected->{$_};
    }
}

is_deeply($tree_copy, $expected, 'root node OK');

my @nodes = @{$tree->{nodes}};

ok(@nodes > 0, 'root node has at least one leaf');

ok((all { $_->{type} eq 'output' } @nodes), 'all nodes are of type CT_OUTPUT');
ok((none { defined($_->{window}) } @nodes), 'no CT_OUTPUT contains a window');
ok((all { @{$_->{nodes}} > 0 } @nodes), 'all nodes have at least one leaf (workspace)');
my @workspaces;
for my $ws (@nodes) {
    my $content = first { $_->{type} eq 'con' } @{$ws->{nodes}};
    @workspaces = (@workspaces, @{$content->{nodes}});
}

ok((all { $_->{type} eq 'workspace' } @workspaces), 'all workspaces are of type CT_WORKSPACE');
#ok((all { @{$_->{nodes}} == 0 } @workspaces), 'all workspaces are empty yet');
ok((none { defined($_->{window}) } @workspaces), 'no CT_OUTPUT contains a window');

# TODO: get the focused container

$i3->command('open')->recv;

# TODO: get the focused container, check if it changed.
# TODO: get the old focused container, check if there is a new child

#diag(Dumper(\@workspaces));

#diag(Dumper($tree));


done_testing;
