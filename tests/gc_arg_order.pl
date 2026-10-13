#!/usr/bin/perl
# Report calls that pass a heap value next to an allocating call in the same
# argument list, e.g.
#
#     form = cl_cons(SYM_LAMBDA, cl_cons(params, body));
#
# C leaves the order in which arguments are evaluated unspecified.  clang
# reads SYM_LAMBDA first, keeps it in a register, and then runs the inner
# cl_cons -- which can collect and move the LAMBDA symbol (a compaction
# slides old objects too), so the outer cons stores an offset that no longer
# points at LAMBDA.  GCC tends to do it the other way round, which makes
# `cl_cons(cl_cons(a, b), tail)` the dangerous shape there.  CL_GC_PROTECT
# does not help: it updates the variable, not the copy already loaded as an
# argument.  Build the list inside out instead, one allocation per statement,
# or use cl_list2/3/4 and cl_list_star3 (mem.h).
#
# Which arguments count: a parameter declared `CL_Obj name` (by value) in a
# prototype or definition under src/, whose argument is anything but a
# constant (CL_NIL, CL_T, a fixnum or character immediate, a literal).
# Which calls allocate: the names in $ALLOC below -- a heuristic, kept
# deliberately broad.
#
# Usage: perl tests/gc_arg_order.pl [ROOT]   -- prints file:line: call, one
# per finding, and exits 1 if there is any.
use strict;
use warnings;

my $root = shift // '.';
chdir $root or die "cannot chdir to $root: $!\n";

my $ALLOC = qr/\b(?:cl_cons|cl_list\w*|cl_make_\w+|cl_intern\w*|cl_alloc\w*|
                  cl_vm_apply|cl_funcall\w*|cl_copy\w*|cl_n?reverse|cl_append\w*|
                  cl_bignum_\w+|cl_arith_\w+|cl_build_\w+|cl_macroexpand\w*|
                  cl_eval\w*|cl_gensym\w*|cl_setf_store_symbol|
                  make_\w+|build_\w+|\w*intern\w*|\w+_keyword)\s*\(/x;
my $CONST = qr/^\s*(?:CL_NIL|CL_T|CL_UNBOUND|CL_MAKE_FIXNUM\s*\(.*\)|
                  CL_MAKE_CHAR\s*\(.*\)|-?\d+[uUlL]*|0x[0-9a-fA-F]+)\s*$/xs;
my $PARENS = qr/(\((?:[^()]++|(?-1))*+\))/;

sub strip {                     # comments and literals -> blanks, lines kept
    my ($s) = @_;
    $s =~ s{/\*.*?\*/|//[^\n]*|"(?:[^"\\\n]|\\.)*"|'(?:[^'\\\n]|\\.)*'}{
        my $m = $&; my $q = substr($m, 0, 1);
        ($q eq '"' || $q eq "'") ? $q . ($m =~ s/[^\n]/ /gr =~ s/^ | $//gr) . $q
                                 : $m =~ s/[^\n]/ /gr
    }gse;
    return $s;
}

sub split_args {                # "(a, f(b, c), d)" -> ("a", " f(b, c)", " d")
    my ($p) = @_;
    my ($depth, $cur, @args) = (0, '');
    for my $ch (split //, substr($p, 1, -1)) {
        if ($ch =~ /[(\[{]/) { $depth++ }
        elsif ($ch =~ /[)\]}]/) { $depth-- }
        elsif ($ch eq ',' && $depth == 0) { push @args, $cur; $cur = ''; next }
        $cur .= $ch;
    }
    push @args, $cur if $cur =~ /\S/ || @args;
    return @args;
}

my @files = sort split /\n/, `find src -name '*.c' -o -name '*.h'`;
my (%raw, %text);               # strip() keeps offsets, so both line up
for my $f (@files) {
    $raw{$f} = do { local (@ARGV, $/) = ($f); scalar <> };
    $text{$f} = strip($raw{$f});
}

# name -> [ is-by-value-CL_Obj per parameter ]; a name declared with
# different shapes (static helpers in two files) is dropped.
my (%sig, %clash);
for my $f (@files) {
    while ($text{$f} =~ /(?:^|[;}\n])\s*(?:(?:static|extern|inline|const|
                          unsigned|struct)\s+)*[A-Za-z_]\w*[\s*]+
                          ([A-Za-z_]\w*)\s*\(([^()]*)\)\s*(?=[;{])/gx) {
        my ($name, $params) = ($1, $2);
        next if $params =~ /^\s*(?:void)?\s*$/;
        my @kinds = map { /^\s*(?:const\s+)?CL_Obj\s+\w+\s*$/ ? 1 : 0 }
                    split /,/, $params;
        next unless grep { $_ } @kinds;
        my $key = join '', @kinds;
        if (exists $sig{$name} && join('', @{$sig{$name}}) ne $key) {
            $clash{$name} = 1;
        }
        $sig{$name} = \@kinds;
    }
}
delete @sig{keys %clash};

my $names = join '|', map { quotemeta } sort keys %sig;
my @hits;
for my $f (@files) {
    my $t = $text{$f};
    my ($line, $counted) = (1, 0);
    while ($t =~ /\b($names)\s*\(/g) {
        my ($name, $at, $open) = ($1, $-[0], $+[0] - 1);
        pos($t) = $open;
        my $call = $t =~ /\G$PARENS/ ? $1 : undef;
        pos($t) = $open + 1;    # nested calls are checked on their own
        next unless defined $call;
        my @args = split_args($call);
        my @kinds = @{$sig{$name}};
        next unless @args == @kinds;
        my @alloc = grep { $args[$_] =~ $ALLOC } 0 .. $#args;
        next unless @alloc;
        my %alloc = map { $_ => 1 } @alloc;
        my @moving = grep { $kinds[$_] && !$alloc{$_} && $args[$_] !~ $CONST }
                     0 .. $#args;
        next unless @moving;
        $line += substr($t, $counted, $at - $counted) =~ tr/\n//;
        $counted = $at;
        (my $shown = substr($raw{$f}, $at, $open - $at + length $call))
            =~ s/\s+/ /g;
        push @hits, "$f:$line: $shown";
    }
}
print "$_\n" for @hits;
exit(@hits ? 1 : 0);
