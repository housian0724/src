#include <math.h>

#include <rsf.h>

#include "int1.h"
#include "interp_spline.h"
#include "prefilter.h"
#include "cosft.h"

int main(int argc, char* argv[])
{
    int nt,nx,ny, iw,ix,iy, nf;
    float dw, dt, dx,dy, t0, vel, x,y, w,st,sq, *str, *trace2, *trace;
    sf_file in, out;

    sf_init (argc,argv);
    in = sf_input("in");
    out = sf_output("out");

    if (!sf_histint(in,"n1",&nt)) sf_error("No n1= in input");
    if (!sf_histint(in,"n2",&nx)) nx=1;
    if (!sf_histint(in,"n3",&ny)) ny=1;

    if (!sf_getfloat("vel",&vel)) sf_error("Need vel=");
    if (!sf_histfloat(in,"d1",&dt)) sf_error("No d1= in input");
    if (!sf_histfloat(in,"o1",&t0)) sf_error("No o1= in input");

    cosft_init(nt, t0, dt);
    dw = SF_PI/(sf_npfar(2*(nt-1))*dt);

    if (!sf_histfloat(in,"d2",&dx)) sf_error("No d2= in input");
    if (!sf_histfloat(in,"d3",&dy)) dy=dx;
    dx *= SF_PI * fabsf (vel) * 0.5;
    dy *= SF_PI * fabsf (vel) * 0.5;	

    if (!sf_getfloat("stretch", &st)) st=1.;
    if (vel < 0) st = 2.-st;

    if (!sf_getint("nf",&nf)) nf=2;

    trace = sf_floatalloc(nt);
    trace2 = sf_floatalloc(nt);
    str = sf_floatalloc(nt);

    prefilter_init (nf, nt, 3*nt);
    for (iy = 0; iy < ny; iy++) {
	y = iy*dy;
	y *= y;
	for (ix = 0; ix < nx; ix++) {
	    x = ix*dx;
	    x = st*(x*x + y);  
	    for (iw = 0; iw < nt; iw++) {
		w = iw*dw;
		sq = (vel < 0)? w*w - x: w*w + x;
		str[iw] = (sq > 0.)? w*(1.-1./st) + sqrtf(sq)/st : - 2.*dw;
	    }
       
	    int1_init (str, 0., dw, nt, spline_int, nt, nf);

	    sf_read(trace,sizeof(float),nt,in);
	    cosft_frw (trace,0,1);
	    prefilter_apply (nt, trace);
	    int1_lop (false,false,nt,nt,trace,trace2);
	    cosft_inv (trace2,0,1);
	    sf_read(trace2,sizeof(float),nt,out);
	}
    }

    exit (0);
}

