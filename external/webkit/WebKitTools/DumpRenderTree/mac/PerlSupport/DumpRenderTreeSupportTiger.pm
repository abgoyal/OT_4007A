package DumpRenderTreeSupport;
require Exporter;
require DynaLoader;
@ISA = qw(Exporter DynaLoader);
package DumpRenderTreeSupportc;
bootstrap DumpRenderTreeSupport;
package DumpRenderTreeSupport;
@EXPORT = qw( );

# ---------- BASE METHODS -------------

package DumpRenderTreeSupport;

sub TIEHASH {
    my ($classname,$obj) = @_;
    return bless $obj, $classname;
}

sub CLEAR { }

sub FIRSTKEY { }

sub NEXTKEY { }

sub FETCH {
    my ($self,$field) = @_;
    my $member_func = "swig_${field}_get";
    $self->$member_func();
}

sub STORE {
    my ($self,$field,$newval) = @_;
    my $member_func = "swig_${field}_set";
    $self->$member_func($newval);
}

sub this {
    my $ptr = shift;
    return tied(%$ptr);
}


# ------- FUNCTION WRAPPERS --------

package DumpRenderTreeSupport;

*processIsCrashing = *DumpRenderTreeSupportc::processIsCrashing;

# ------- VARIABLE STUBS --------

package DumpRenderTreeSupport;

1;