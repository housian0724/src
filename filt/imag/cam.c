/* 3-D prestack migration/modeling by split-step common-azimuth */
/*
  Copyright (C) 2004 University of Texas at Austin
  
  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.
  
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
  
  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include <math.h>

#include <rsf.h>
/*^*/

#include "cam.h"
#include "fft3.h"
#include "taper.h"
#include "slowref.h"

#include "slice.h"
/*^*/

#define LOOPmmh(a) for(imy=0;imy<amy.n;imy++){ for(imx=0;imx<amx.n;imx++){ for(ihx=0;ihx<ahx.n;ihx++){ {a} }}}
#define KOOPmmh(a) for(imy=0;imy<bmy.n;imy++){ for(imx=0;imx<bmx.n;imx++){ for(ihx=0;ihx<bhx.n;ihx++){ {a} }}}
#define LOOPllh(a) for(ily=0;ily<aly.n;ily++){ for(ilx=0;ilx<alx.n;ilx++){ for(ihx=0;ihx<ahx.n;ihx++){ {a} }}}

#define BOUND(i,n) (i<0) ? 0 : ( (i>=n) ? n : i );
#define  KMAP(i,n) (i<n/2.) ? (i+n/2.) : (i-n/2.);

static axa az,amx,amy,aw,alx,aly,ahx;
static axa    bmx,bmy,           bhx;
static bool verb;
static float eps;
static float  dt;
static int nrmax;

static float         ***qq; /* image */
static float complex ***pk; /* wavefield k */
static float complex ***wk; /* wavefield k */
static float complex ***wx; /* wavefield x */
static float         ***tt; /* taper */
static int           ***ms; /* multi-reference slowness map  */
static int           ***mr; /* multi-reference slowness map  */
static bool          ***skip;
static float          **ksx;/* source   wavenumber */
static float          **krx;/* receiver wavenumber */
static float          **sz; /* reference slowness */
static float          **sm; /* reference slowness squared */
static float          **ss; /* slowness */
static int            **is; /* source   index */
static int            **ir; /* receiver index */
static int             *jx; /* i-line index */
static int             *jy; /* x-line index */
static int             *nr; /* number of references */

static fslice mms, mmr;

void cam_init(bool verb_,
	      float eps_,
	      float  dt_,
	      axa az_   /* depth */,
	      axa aw_   /* frequency */,
	      axa ahx_  /* half-offset */,
	      axa amx_  /* i-line (data) */,
	      axa amy_  /* x-line (data) */,
	      axa alx_  /* i-line (slowness/image) */,
	      axa aly_  /* x-line (slowness/image) */,
	      int ntx, int nty, int nth      /* taper size */,
	      int nr1                        /* maximum number of references */,
	      int npad                       /* padding on nh */)
/*< initialize >*/
{
    int   imy, imx, ihx, ilx, ily;
    int        jmx, jhx;
    float  my,  mx,  hx,    k;
    float      kmx, khx;

    verb=verb_;
    eps = eps_;
    dt  =  dt_;

    az = az_;
    aw = aw_;
    amx= amx_;
    amy= amy_;
    alx= alx_;
    aly= aly_;
    ahx= ahx_;

    bmy.n = amy.n;
    bmy.d =           2.0*SF_PI/(bmy.n*amy.d);
    bmy.o = (1==bmy.n)?0:-SF_PI/       amy.d ;

    bmx.n = amx.n;
    bmx.d =           2.0*SF_PI/(bmx.n*amx.d);
    bmx.o = (1==bmx.n)?0:-SF_PI/      amx.d ;

    bhx.n = ahx.n + npad;
    bhx.d =           2.0*SF_PI/(bhx.n*ahx.d);
    bhx.d = (1==bhx.n)?0:-SF_PI/       ahx.d ;

    nrmax = nr1;

    fft3_init(bhx.n,bmx.n,bmy.n);

    /* allocate workspace */
    qq = sf_floatalloc3   (ahx.n,alx.n,aly.n);  /* image */
    ss = sf_floatalloc2         (alx.n,aly.n);  /* slowness */
    sz = sf_floatalloc2          (nrmax,az.n);  /* reference slowness */
    sm = sf_floatalloc2          (nrmax,az.n);  /* reference slowness squared*/
    nr = sf_intalloc                   (az.n);  /* number of reference slownesses */

    pk = sf_complexalloc3 (bhx.n,bmx.n,bmy.n);  /* padded wavefield */ 
    wk = sf_complexalloc3 (bhx.n,bmx.n,bmy.n);  /* k wavefield */
    wx = sf_complexalloc3 (ahx.n,amx.n,amy.n);  /* x wavefield */
    tt = sf_floatalloc3   (ahx.n,amx.n,amy.n);  /* taper */
    ms = sf_intalloc3     (ahx.n,amx.n,amy.n);  /* MRS map */
    mr = sf_intalloc3     (ahx.n,amx.n,amy.n);  /* MRS map */

    ksx= sf_floatalloc2   (bhx.n,amx.n);       /* source   wavenumber */
    krx= sf_floatalloc2   (bhx.n,amx.n);       /* receiver wavenumber */
    is = sf_intalloc2     (ahx.n,amx.n);       /* source   index */
    ir = sf_intalloc2     (ahx.n,amx.n);       /* receiver index */

    jy = sf_intalloc            (amy.n);       /* midpoint index */
    jx = sf_intalloc            (amx.n);       /* midpoint index */

    skip = sf_boolalloc3 (nrmax,nrmax,az.n); /* skip S-R reference slowness combination */

    /* precompute indices */
    for (imy=0; imy<amy.n; imy++) {
	my = amy.o + imy*amy.d;

	ily = 0.5+(my-aly.o)/aly.d;
	ily = BOUND(ily,aly.n);
	jy[imy] = ily;                     /* x-line index */
    }
    for (imx=0; imx<amx.n; imx++) {
	mx = amx.o + imx*amx.d;
    
	ilx = 0.5+(mx-alx.o)/alx.d;
	ilx = BOUND(ilx,alx.n);
	jx[imx] = ilx;                     /* i-line index */

	for (ihx=0; ihx<ahx.n; ihx++) {
	    hx = ahx.d + ihx*ahx.d;
	    
	    ilx = 0.5+(mx-hx-alx.o)/alx.d;
	    ilx = BOUND(ilx,alx.n);
	    is[imx][ihx] = ilx;             /* source index */
	    
	    ilx = 0.5+(mx+hx-alx.o)/alx.d;
	    ilx = BOUND(ilx,alx.n);
	    ir[imx][ihx] = ilx;             /* receiver index */
	}
    }

    /* precompute wavenumbers */
    for (imx=0; imx<bmx.n; imx++) {
	jmx = KMAP(imx,bmx.n);
	kmx = bmx.o + jmx*bmx.d;
	
	for (ihx=0; ihx<bhx.n; ihx++) {
	    jhx = KMAP(ihx,bhx.n);
	    khx = bhx.o + jhx*bhx.d;

	    k = 0.5*(kmx-khx);
	    ksx[imx][ihx] = k*k; /* ksx^2 */

	    k = 0.5*(kmx+khx);
	    krx[imx][ihx] = k*k; /* krx^2 */
	}
    }    

    /* precompute taper array */
    LOOPmmh(tt[imy][imx][ihx]=1.;);
    taper3(nty,ntx,nth,true,true,false,amy.n,amx.n,ahx.n,tt);

    mms = fslice_init(ahx.n,amy.n*amx.n,az.n,sizeof(int));
    mmr = fslice_init(ahx.n,amy.n*amx.n,az.n,sizeof(int));    
}

void cam_close(void)
/*< free allocated storage >*/
{
    free(**qq); free( *qq); free( qq);
    free(**pk); free( *pk); free( pk);
    free(**wk); free( *wk); free( wk);
    free(**wx); free( *wx); free( wx);
    ;           free( *ss); free( ss);
    ;           free( *sz); free( sz);
    ;           free( *sm); free( sm);
    ;           ;           free( nr);
    ;           free( *ksx);free( ksx);
    ;           free( *krx);free( krx);
    ;           free( *is); free( is);
    ;           free( *ir); free( ir);
    free(**tt); free( *tt); free( tt);
    free(**ms); free( *ms); free( ms);
    free(**mr); free( *mr); free( mr);
    
    free(**skip); free( *skip); free(skip);

    fslice_close(mms);
    fslice_close(mmr);
    
    fft3_close();
}

void cam( bool inv   /* migration/modeling flag */, 
	  slice data /* data       [nw][nx][ny][nh] */,
	  slice imag /* image file [nz][nv][nu][nh] */,
	  slice slow /* slowness   [nz][nv][nu]     */)
/*< Apply migration/modeling >*/
{
    int iz,iw,imy,imx,ihx,ilx,ily, j,js,jr;
    int       jmy;
    float sy, kmy;
    float complex cshift=0, cref=0, w, w2, cs, cr, khy, kss, krr;

    if (!inv) { /* prepare image for migration */
	LOOPllh( qq[ily][ilx][ihx] = 0.0; );
	for (iz=0; iz<az.n; iz++) {
	    slice_put(imag,iz,qq[0][0]);
	}
    }

    /* compute reference slowness */
    for (iz=0; iz<az.n; iz++) {
	slice_get(slow,iz,ss[0]);
	nr[iz] = slowref(nrmax,dt/az.d,alx.n*aly.n,ss[0],sz[iz],sm[iz]);
	if (verb) sf_warning("nr[%d]=%d",iz,nr[iz]);

	/* create MRS map */
	LOOPmmh( ms[imy][imx][ihx] = 0; 
		 mr[imy][imx][ihx] = 0.; );
	for (j=0; j<nr[iz]-1; j++) {
	    sy = 0.5*(sz[iz][j]+sz[iz][j+1]);
	    LOOPmmh( if(ss[ jy[imy] ][ is[imx][ihx] ] > sy) ms[imy][imx][ihx]++;
		     if(ss[ jy[imy] ][ ir[imx][ihx] ] > sy) mr[imy][imx][ihx]++; );
	}
	fslice_put(mms,iz,ms[0][0]);
	fslice_put(mmr,iz,mr[0][0]);
	for (js=0; js<nr[iz]; js++) {
	    for (jr=0; jr<nr[iz]; jr++) {
		skip[iz][js][jr] = true;
	    }
	}
	LOOPmmh( skip[iz][ ms[imy][imx][ihx] ][ mr[imy][imx][ihx] ] = false; );
    }
    for (iz=0; iz<az.n-1; iz++) {
	for (j=0; j<nr[iz]; j++) {
	    sm[iz][j] = 0.5*(sm[iz][j]+sm[iz+1][j]);
	}
    }
    
    /* loop over frequencies w */
    for (iw=0; iw<aw.n; iw++) {
	if (verb) sf_warning ("frequency %d of %d",iw+1,aw.n);

	w = eps*aw.d + I*(aw.o+iw*aw.d);
	w2 = w*w;

	if (inv) { /* MODELING */
	    slice_get(slow,az.n-1,ss[0]);              /* slowness at bottom */

	    /* imaging condition @ bottom */
	    slice_get(imag,az.n-1,qq[0][0]);
	    LOOPmmh( wx[imy][imx][ihx] = qq[ jy[imy] ][ jx[imx] ][ihx]; );
	  
	    /* loop over migrated depths z */
	    for (iz=az.n-2; iz>=0; iz--) {

		/* w-x @ bottom */
		LOOPmmh( sy = 0.5*(ss[ jy[imy] ][ is[imx][ihx] ] + 
				   ss[ jy[imy] ][ ir[imx][ihx] ]);
			 cshift = cexpf(-w*sy*az.d);
			 wx[imy][imx][ihx] *= cshift; );
		LOOPmmh( pk[imy][imx][ihx] *= tt[imy][imx][ihx] ; );

		/* FFT */
		KOOPmmh( pk[imy][imx][ihx] = 0.;);
		LOOPmmh( pk[imy][imx][ihx] = wx[imy][imx][ihx]; );
		fft3(false,pk);		
		
		fslice_get(mms,iz,ms[0][0]);
		fslice_get(mmr,iz,mr[0][0]);

		LOOPmmh( wx[imy][imx][ihx] = 0; );
		for (js=0; js<nr[iz]; js++) {
		    for (jr=0; jr<nr[iz]; jr++) {
			if (skip[iz][js][jr]) continue; /* skip S-R reference combinations */

			/* w-k phase shift */
			cref = csqrtf(w2*sm[iz][js]) + csqrtf(w2*sm[iz][jr]); /* w so */
			KOOPmmh( jmy = KMAP(imy,bmy.n);
				 kmy = bmy.o + jmy*bmy.d; 
				 cs = csqrtf(w2*sm[iz][js]+ksx[imx][ihx]);
				 cr = csqrtf(w2*sm[iz][jr]+krx[imx][ihx]);
				 khy= kmy*(cr-cs)/(cr+cs); /* comaz approximation */
				 kss = 0.5*(kmy-khy);
				 krr = 0.5*(kmy+khy);
				 kss = kss*kss + ksx[imx][ihx];
				 krr = krr*krr + krx[imx][ihx];
				 cs = csqrtf(w2*sm[iz][js] + kss);
				 cr = csqrtf(w2*sm[iz][jr] + krr);
				 cshift = cexpf((cref-cs-cr)*az.d);  /* w so - kzs - kzr */
				 wk[imy][imx][ihx] = 
				 pk[imy][imx][ihx] * cshift; ); 
			
			/* IFT */
			fft3(true,wk);

			/* accumulate wavefield */
			LOOPmmh( if (ms[imy][imx][ihx]==js && 
				     mr[imy][imx][ihx]==jr ) 
				 wx[imy][imx][ihx] += wk[imy][imx][ihx]; );
		    } /* jr loop */
		} /* js loop */

		/* w-x at top */
		slice_get(slow,iz,ss[0]);
		LOOPmmh( sy = 0.5*(ss[ jy[imy] ][ is[imx][ihx] ] + 
				   ss[ jy[imy] ][ ir[imx][ihx] ]);
			 cshift = cexpf(-w*sy*az.d);
			 wx[imy][imx][ihx] *= cshift; ); 

		/* imaging condition */
		slice_get(imag,iz,qq[0][0]);
		LOOPmmh( wx   [imy]    [imx] [ihx] +=
			 qq[jy[imy]][jx[imx]][ihx]; );

	    } /* iz */
	    
	    LOOPmmh( wx[imy][imx][ihx] *= tt[imy][imx][ihx]; ); /* taper wavefield */
	    cslice_put(data,iw,wx[0][0]);    /* put wavefield = data on file */
	    
	} else { /* MIGRATION */
	    slice_get(slow,0,ss[0]);                      /* slowness at top */

	    cslice_get(data,iw,wx[0][0]);    /* get wavefield = data on file */
	    LOOPmmh( wx[imy][imx][ihx] *= tt[imy][imx][ihx]; ); /* taper wavefield */

	    /* loop over migrated depths z */
	    for (iz=0; iz< az.n-1; iz++) {

		/* imaging condition */
		slice_get(imag,iz,qq[0][0]);
		LOOPmmh(        qq[jy[imy]][jx[imx]][ihx] += 
			 crealf(wx   [imy]    [imx] [ihx] ); );
		slice_put(imag,iz,qq[0][0]);

		/* w-x @ top */
		LOOPmmh( sy = 0.5*(ss[ jy[imy] ][ is[imx][ihx] ] + 
				   ss[ jy[imy] ][ ir[imx][ihx] ]);
			 cshift = conjf(cexpf(-w*sy*az.d));
			 wx[imy][imx][ihx] *= cshift; );

		/* FFT */
		KOOPmmh( pk[imy][imx][ihx] = 0.;);
		LOOPmmh( pk[imy][imx][ihx] = wx[imy][imx][ihx]; );
		fft3(false,pk);

		fslice_get(mms,iz,ms[0][0]);
		fslice_get(mmr,iz,mr[0][0]);
		
		LOOPmmh( wx[imy][imx][ihx] = 0; );
		for (js=0; js<nr[iz]; js++) {
		    for (jr=0; jr<nr[iz]; jr++) {
			if (skip[iz][js][jr]) continue;
		
			/* w-k phase shift */
			cref= csqrtf(w2*sm[iz][js]) 
			    + csqrtf(w2*sm[iz][jr]); /* w so */
			KOOPmmh( jmy = KMAP(imy,bmy.n);
				 kmy = bmy.o + jmy*bmy.d; 
				 cs = csqrtf(w2*sm[iz][js] + ksx[imx][ihx]);
				 cr = csqrtf(w2*sm[iz][jr] + krx[imx][ihx]);
				 khy= kmy*(cr-cs)/(cr+cs); /* comaz approximation */
				 kss = 0.5*(kmy-khy);
				 krr = 0.5*(kmy+khy);
				 kss = kss*kss + ksx[imx][ihx];
				 krr = krr*krr + krx[imx][ihx];
				 cs = csqrtf(w2*sm[iz][js] + kss);
				 cr = csqrtf(w2*sm[iz][jr] + krr);
				 cshift = conjf(cexpf((cref-cs-cr)*az.d)); /* w so - kzs - kzr */
				 wk[imy][imx][ihx] = 
				 pk[imy][imx][ihx] * cshift; );
 
			/* IFT */
			fft3(true,wk);

			/* accumulate wavefield */
			LOOPmmh( if (ms[imy][imx][ihx]==js && 
				     mr[imy][imx][ihx]==jr ) 
				 wx[imy][imx][ihx] += wk[imy][imx][ihx]; );
		    } /* jr loop */
		} /* js loop */

		/* w-x @ bottom */
		slice_get(slow,iz+1,ss[0]);
		LOOPmmh( sy = 0.5*(ss[ jy[imy] ][ is[imx][ihx] ] + 
				   ss[ jy[imy] ][ ir[imx][ihx] ]);
			 cshift = conjf(cexpf(-w*sy*az.d));
			 wx[imy][imx][ihx] *= cshift; );
		LOOPmmh( wx[imy][imx][ihx] *= tt[imy][imx][ihx]; );
	    } /* iz */
	    
	    /* imaging condition @ bottom */
	    slice_get(imag,az.n-1,qq[0][0]);
	    LOOPmmh(        qq[jy[imy]][jx[imx]][ihx] += 
		     crealf(wx   [imy]    [imx] [ihx]); );
	    slice_put(imag,az.n-1,qq[0][0]);
	} /* else */
    } /* iw */
}

