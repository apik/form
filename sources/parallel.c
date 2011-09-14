/** @file parallel.c
 *
 *  Message passing library independent functions of parform
 *
 *  This file contains functions needed for the parallel version of form3
 *  these functions need no real link to the message passing libraries, they
 *  only need some interface dependent preprocessor definitions (check
 *  parallel.h). So there still need two different objectfiles to be compiled
 *  for mpi and pvm!
 */
/* #[ License : */
/*
 *   Copyright (C) 1984-2010 J.A.M. Vermaseren
 *   When using this file you are requested to refer to the publication
 *   J.A.M.Vermaseren "New features of FORM" math-ph/0010025
 *   This is considered a matter of courtesy as the development was paid
 *   for by FOM the Dutch physics granting agency and we would like to
 *   be able to track its scientific use to convince FOM of its value
 *   for the community.
 *
 *   This file is part of FORM.
 *
 *   FORM is free software: you can redistribute it and/or modify it under the
 *   terms of the GNU General Public License as published by the Free Software
 *   Foundation, either version 3 of the License, or (at your option) any later
 *   version.
 *
 *   FORM is distributed in the hope that it will be useful, but WITHOUT ANY
 *   WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *   FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *   details.
 *
 *   You should have received a copy of the GNU General Public License along
 *   with FORM.  If not, see <http://www.gnu.org/licenses/>.
 */
/* #] License : */ 
/*
  	#[ includes :
*/
#include "form3.h"
#include "vector.h"

/* mpi.c */
LONG PF_RealTime(int);
int PF_LibInit(int*, char***);
int PF_Probe(int*);
int PF_RecvWbuf(WORD*,LONG*,int*);
int PF_IRecvRbuf(PF_BUFFER*,int,int);
int PF_WaitRbuf(PF_BUFFER *,int,LONG *);
int PF_Bcast(void *buffer, int count);
int PF_RawSend(int dest, void *buf, LONG l, int tag);
LONG PF_RawRecv(int *src,void *buf,LONG thesize,int *tag);
int PF_RawProbe(int *src, int *tag, int *bytesize);
int PF_PrintPackBuf(char*,int);
#ifdef SHMEM
int PF_Pack(VOID *,LONG,int);
int PF_UnPack(VOID*,LONG,int);
#elif defined(WITHMPI)
int PF_Pack(VOID *,LONG,MPI_Datatype);
int PF_UnPack(VOID*,LONG,MPI_Datatype);
#else
  #error "SHMEM or WITHMPI should be defined!"
#endif
int PF_PackString(UBYTE *);
int PF_UnPackString(UBYTE *);
int PF_Send(int,int,int);
int PF_Receive(int,int,int*,int*);
int PF_BroadCast(int);
int PF_longSingleReset(void);
int PF_longMultiReset(void);
int PF_longSinglePack(UBYTE *,int,MPI_Datatype);
int PF_longSingleUnPack(UBYTE*,LONG,MPI_Datatype);
int PF_longMultiPack(UBYTE *,int,int,MPI_Datatype);
int PF_longMultiUnPack(UBYTE*,int,int,MPI_Datatype);
int PF_longSingleSend(int,int);
int PF_longSingleReceive(int,int,int*,int*);
int PF_longBroadcast(void);

static int PF_WaitAllSlaves(void);

static int PF_Wait4MasterIP(int tag);
static int PF_DoOneExpr(void);
static int PF_ReadMaster(void);/*reads directly to its scratch!*/
static int PF_Slave2MasterIP(int src);/*both master and slave*/
static int PF_Master2SlaveIP(int dest, EXPRESSIONS e);
static int PF_WalkThrough(WORD *t, LONG l, LONG chunk, LONG *count);
static int PF_SendChunkIP(FILEHANDLE *curfile,  POSITION *position, int to, LONG thesize);
static int PF_RecvChunkIP(FILEHANDLE *curfile, int from, LONG thesize);

static void PF_ReceiveErrorMessage(int src, int tag);
static void PF_CatchErrorMessages(int src);
static void PF_CatchErrorMessagesForAll(void);
static int PF_ProbeWithCatchingErrorMessages(int *src);

PARALLELVARS PF;
#ifdef MPI2
 WORD *PF_shared_buff;
#endif

static LONG     PF_goutterms;  /* (master) Total out terms at PF_EndSort(), used in PF_Statistics(). */
static POSITION PF_exprsize;   /* (master) The size of the expression at PF_EndSort(), used in PF_Processor(). */

/*
	This will work well only under Linux, see
		#ifdef PF_WITH_SCHED_YIELD
	below in PF_WaitAllSlaves().
*/
#ifdef PF_WITH_SCHED_YIELD
 #include <sched.h>
#endif

#ifdef PF_WITHLOG
 #define PRINTFBUF(TEXT,TERM,SIZE)  { if(PF.log){ WORD iii;\
  fprintf(stderr,"[%d|%ld] %s : ",PF.me,AC.CModule,(char*)TEXT);\
  if(TERM){ fprintf(stderr,"[%d] ",(int)(*TERM));\
    if((SIZE)<500 && (SIZE)>0) for(iii=1;iii<(SIZE);iii++)\
      fprintf(stderr,"%d ",TERM[iii]); }\
  fprintf(stderr,"\n");\
  fflush(stderr); } }
#else
 #define PRINTFBUF(TEXT,TERM,SIZE) {}
#endif

/**
 * Swaps the variables \a x and \a y. If sizeof(x) != sizeof(y) then a compilation error
 * will occur. A set of memcpy calls is expected to be inlined by the optimisation.
 */
#define SWAP(x, y) \
	do { \
		char swap_tmp__[sizeof(x) == sizeof(y) ? (int)sizeof(x) : -1]; \
		memcpy(swap_tmp__, &y, sizeof(x)); \
		memcpy(&y, &x, sizeof(x)); \
		memcpy(&x, swap_tmp__, sizeof(x)); \
	} while (0)

/**
 * Packs a LONG value \a n to a WORD buffer \a p.
 */
#define PACK_LONG(p, n) \
	do { \
		*(p)++ = (UWORD)((ULONG)(n) & (ULONG)WORDMASK); \
		*(p)++ = (UWORD)(((ULONG)(n) >> BITSINWORD) & (ULONG)WORDMASK); \
	} while (0)

/**
 * Unpacks a LONG value \a n from a WORD buffer \a p.
 */
#define UNPACK_LONG(p, n) \
	do { \
		(n) = (LONG)((((ULONG)(p)[1] & (ULONG)WORDMASK) << BITSINWORD) | ((ULONG)(p)[0] & (ULONG)WORDMASK)); \
		(p) += 2; \
	} while (0)

/*
 * For debugging.
 */
#define DBGOUT(lv1, lv2, a) do { if ( lv1 >= lv2 ) { printf a; fflush(stdout); } } while (0)

/* (AN.ninterms of master) == max(AN.ninterms of slaves) == sum(PF_linterms of slaves) at EndSort(). */
#define DBGOUT_NINTERMS(lv, a)
/* #define DBGOUT_NINTERMS(lv, a) DBGOUT(1, lv, a) */

/*
  	#] includes : 
  	#[ statistics :
 		#[ variables : (should be part of a struct?)
*/
static LONG PF_maxinterms;   /* maximum number of terms in one inputpatch */
static LONG PF_linterms;     /* local interms on this proces: PF_Proces */
#define PF_STATS_SIZE 5
static LONG **PF_stats = NULL;/* space for collecting statistics of all procs */
static LONG PF_laststat;     /* last realtime when statistics were printed */
static LONG PF_statsinterval;/* timeinterval for printing statistics */
/*
 		#] variables : 
 		#[ PF_Statistics :
*/

/**
 * Prints statistics every PF_statinterval seconds.
 * For \a proc = 0 it prints final statistics for EndSort().
 *
 * @param  stats  the pointer to an array: LONG stats[proc][5] = {cpu,space,in,gen,left}.
 * @param  proc   the source process number.
 * @return        0 if OK, nonzero on error.
 */
static int PF_Statistics(LONG **stats, int proc)
{
	GETIDENTITY
	LONG real, cpu;
	WORD rpart, cpart;
	int i, j;

	if ( AT.SS == AM.S0 && PF.me == MASTER ) {
		real = PF_RealTime(PF_TIME); rpart = (WORD)(real%100); real /= 100;

		if ( PF_stats == NULL ) {
			PF_stats = (LONG**)Malloc1(PF.numtasks*sizeof(LONG*),"PF_stats 1");
			for ( i = 0; i < PF.numtasks; i++ ) {
				PF_stats[i] = (LONG*)Malloc1(PF_STATS_SIZE*sizeof(LONG),"PF_stats 2");
				for ( j = 0; j < PF_STATS_SIZE; j++ ) PF_stats[i][j] = 0;
			}
		}
		if ( proc > 0 ) for ( i = 0; i < PF_STATS_SIZE; i++ ) PF_stats[proc][i] = stats[0][i];

		if ( real >= PF_laststat + PF_statsinterval || proc == 0 ) {
			LONG sum[PF_STATS_SIZE];

			for ( i = 0; i < PF_STATS_SIZE; i++ ) sum[i] = 0;
			sum[0] = cpu = TimeCPU(1);
			cpart = (WORD)(cpu%1000);
			cpu /= 1000;
			cpart /= 10;
			if ( AC.OldParallelStats ) MesPrint("");
			if ( proc > 0 && AC.StatsFlag && AC.OldParallelStats ) {
				MesPrint("proc          CPU         in        gen       left       byte");
				MesPrint("%3d  : %7l.%2i %10l",0,cpu,cpart,AN.ninterms);
			}
			else if ( AC.StatsFlag && AC.OldParallelStats ) {
				MesPrint("proc          CPU         in        gen       out        byte");
				MesPrint("%3d  : %7l.%2i %10l %10l %10l",0,cpu,cpart,AN.ninterms,0,PF_goutterms);
			}

			for ( i = 1; i < PF.numtasks; i++ ) {
				cpart = (WORD)(PF_stats[i][0]%1000);
				cpu = PF_stats[i][0] / 1000;
				cpart /= 10;
				if ( AC.StatsFlag && AC.OldParallelStats )
					MesPrint("%3d  : %7l.%2i %10l %10l %10l",i,cpu,cpart,
							PF_stats[i][2],PF_stats[i][3],PF_stats[i][4]);
				for ( j = 0; j < PF_STATS_SIZE; j++ ) sum[j] += PF_stats[i][j];
			}
			cpart = (WORD)(sum[0]%1000);
			cpu = sum[0] / 1000;
			cpart /= 10;
			if ( AC.StatsFlag && AC.OldParallelStats ) {
				MesPrint("Sum  = %7l.%2i %10l %10l %10l",cpu,cpart,sum[2],sum[3],sum[4]);
				MesPrint("Real = %7l.%2i %20s (%l) %16s",
						real,rpart,AC.Commercial,AC.CModule,EXPRNAME(AR.CurExpr));
				MesPrint("");
			}
			PF_laststat = real;
		}
	}
	return(0);
}
/*
 		#] PF_Statistics : 
  	#] statistics : 
  	#[ sort.c :
 		#[ sort variables :
*/

/**
 * A node for the tree of losers in the final sorting on the master.
 */
typedef struct NoDe {
	struct NoDe *left;
	struct NoDe *rght;
	int lloser;
	int rloser;
	int lsrc;
	int rsrc;
} NODE;

/*
	should/could be put in one struct
*/
static  NODE *PF_root;			/* root of tree of losers */
static  WORD PF_loser;			/* this is the last loser */
static  WORD **PF_term;			/* these point to the active terms */
static  WORD **PF_newcpos;		/* new coeffs of merged terms */
static  WORD *PF_newclen;		/* length of new coefficients */

/*
	preliminary: could also write somewhere else?
*/

static  WORD *PF_WorkSpace;		/* used in PF_EndSort() */
static  UWORD *PF_ScratchSpace;	/* used in PF_GetLoser() */

/*
 		#] sort variables : 
 		#[ PF_AllocBuf :
*/

/**
 * Allocates one PF_BUFFER struct with \a nbuf cyclic buffers of size \a bsize.
 * For the first \a free buffers there is no space allocated.
 * For example, if \a free == 1 then for the first (index 0) buffer there is
 * no space allocated(!!!) (one can reuse existing space for it) and
 * actually buff[0], stop[0], fill[0] and full[0] in the returned
 * PF_BUFFER struct are undefined.
 *
 * @param  nbufs  the number of cyclic buffers for PF_BUFFER struct.
 * @param  bsize  the memory allocation size in bytes for each buffer.
 * @param  free   the number of the buffers without the memory allocation.
 * @return        the pointer to the PF_BUFFER struct if succeeded. NULL if failed.
 *
 * @todo Maybe this should be really hidden in the send/recv routines and pvm/mpi
 *       files, it is only complicated because of nonblocking send/receives!
 */
static PF_BUFFER *PF_AllocBuf(int nbufs, LONG bsize, WORD free)
{
	PF_BUFFER *buf;
	UBYTE *p, *stop;
	LONG allocsize;
	int i;

	allocsize =
		(LONG)(sizeof(PF_BUFFER) + 4*nbufs*sizeof(WORD*) + (nbufs-free)*bsize);

#ifdef WITHMPI
	allocsize +=
		(LONG)( nbufs * (  2 * sizeof(MPI_Status)
		                 +     sizeof(MPI_Request)
		                 +     sizeof(MPI_Datatype)
		                )  );
#endif
	allocsize += (LONG)( nbufs * 3 * sizeof(int) );

	if ( ( buf = (PF_BUFFER*)Malloc1(allocsize,"PF_AllocBuf") ) == NULL ) return(NULL);

	p = ((UBYTE *)buf) + sizeof(PF_BUFFER);
	stop = ((UBYTE *)buf) + allocsize;

	buf->numbufs = nbufs;
	buf->active = 0;

	buf->buff    = (WORD**)p;		  p += buf->numbufs*sizeof(WORD*);
	buf->fill    = (WORD**)p;		  p += buf->numbufs*sizeof(WORD*);
	buf->full    = (WORD**)p;		  p += buf->numbufs*sizeof(WORD*);
	buf->stop    = (WORD**)p;		  p += buf->numbufs*sizeof(WORD*);
#ifdef WITHMPI
	buf->status  = (MPI_Status *)p;	  p += buf->numbufs*sizeof(MPI_Status);
	buf->retstat = (MPI_Status *)p;	  p += buf->numbufs*sizeof(MPI_Status);
	buf->request = (MPI_Request *)p;  p += buf->numbufs*sizeof(MPI_Request);
	buf->type    = (MPI_Datatype *)p; p += buf->numbufs*sizeof(MPI_Datatype);
	buf->index   = (int *)p;		  p += buf->numbufs*sizeof(int);

	for ( i = 0; i < buf->numbufs; i++ ) buf->request[i] = MPI_REQUEST_NULL;
#endif
#ifdef PVM
	buf->type    = (int *)p;		  p += buf->numbufs*sizeof(int);
#endif
	buf->tag     = (int *)p;		  p += buf->numbufs*sizeof(int);
	buf->from    = (int *)p;		  p += buf->numbufs*sizeof(int);
/*
		and finally the real bufferspace
*/
	for ( i = free; i < buf->numbufs; i++ ) {
		buf->buff[i] = (WORD*)p; p += bsize;
		buf->stop[i] = (WORD*)p;
		buf->fill[i] = buf->full[i] = buf->buff[i];
	}
	if ( p != stop ) {
		MesPrint("Error in PF_AllocBuf p = %x stop = %x\n",p,stop);
		return(NULL);
	}
	return(buf);
}

/*
 		#] PF_AllocBuf : 
 		#[ PF_InitTree :
*/

/**
 * Initializes the sorting tree on the master.
 * It allocates bufferspace (if necessary) for
 *   \li  pointers to terms in the tree and their coefficients
 *   \li  the cyclic receive buffers for nonblocking receives (PF.rbufs)
 *   \li  the nodes of the actual tree
 *
 * and initializes these with (hopefully) correct values.
 *
 * @return  the number of nodes in the merge tree if succeeded. -1 if failed.
*/
static int PF_InitTree(void)
{
	GETIDENTITY
	PF_BUFFER **rbuf = PF.rbufs;
	UBYTE *p, *stop;
	int numrbufs,numtasks = PF.numtasks;
	int i, j, src, numnodes;
	int numslaves = numtasks - 1;
	long size;
/*
 		#[ the buffers : for the new coefficients and the terms
 		   we need one for each slave
*/
	if ( PF_term == NULL ) {
		size =  2*numtasks*sizeof(WORD*) + sizeof(WORD)*
			( numtasks*(1 + AM.MaxTal) + (AM.MaxTer/sizeof(WORD)+1) + 2*(AM.MaxTal+2));

		PF_term = (WORD **)Malloc1(size,"PF_term");
		stop = ((UBYTE*)PF_term) + size;
		p = ((UBYTE*)PF_term) + numtasks*sizeof(WORD*);

		PF_newcpos = (WORD **)p;  p += sizeof(WORD*) * numtasks;
		PF_newclen =  (WORD *)p;  p += sizeof(WORD)  * numtasks;
		for ( i = 0; i < numtasks; i++ ) {
			PF_newcpos[i] = (WORD *)p; p += sizeof(WORD)*AM.MaxTal;
			PF_newclen[i] = 0;
		}
		PF_WorkSpace = (WORD *)p;    p += AM.MaxTer+sizeof(WORD);
		PF_ScratchSpace = (UWORD*)p; p += 2*(AM.MaxTal+2)*sizeof(UWORD);

		if ( p != stop ) { MesPrint("error in PF_InitTree"); return(-1); }
	}
/*
 		#] the buffers : 
 		#[ the receive buffers :
*/
	numrbufs = PF.numrbufs;
/*
		this is the size we have in the combined sortbufs for one slave
*/
	size = (AT.SS->sTop2 - AT.SS->lBuffer - 1)/(PF.numtasks - 1);

	if ( rbuf == NULL ) {
		if ( ( rbuf = (PF_BUFFER**)Malloc1(numtasks*sizeof(PF_BUFFER*), "Master: rbufs") ) == NULL ) return(-1);
		if ( (rbuf[0] = PF_AllocBuf(1,0,1) ) == NULL ) return(-1);
		for ( i = 1; i < numtasks; i++ ) {
			if (!(rbuf[i] = PF_AllocBuf(numrbufs,sizeof(WORD)*size,1))) return(-1);
		}
	}
	rbuf[0]->buff[0] = AT.SS->lBuffer;
	rbuf[0]->full[0] = rbuf[0]->fill[0] = rbuf[0]->buff[0];
	rbuf[0]->stop[0] = rbuf[1]->buff[0] = rbuf[0]->buff[0] + 1;
	rbuf[1]->full[0] = rbuf[1]->fill[0] = rbuf[1]->buff[0];
	for ( i = 2; i < numtasks; i++ ) {
		rbuf[i-1]->stop[0] = rbuf[i]->buff[0] = rbuf[i-1]->buff[0] + size;
		rbuf[i]->full[0] = rbuf[i]->fill[0] = rbuf[i]->buff[0];
	}
	rbuf[numtasks-1]->stop[0] = rbuf[numtasks-1]->buff[0] + size;

	for ( i = 1; i < numtasks; i++ ) {
		for ( j = 0; j < rbuf[i]->numbufs; j++ ) {
			rbuf[i]->full[j] = rbuf[i]->fill[j] = rbuf[i]->buff[j] + AM.MaxTer/sizeof(WORD) + 2;
		}
		PF_term[i] = rbuf[i]->fill[rbuf[i]->active];
		*PF_term[i] = 0;
		PF_IRecvRbuf(rbuf[i],rbuf[i]->active,i);
	}
	rbuf[0]->active = 0;
	PF_term[0] = rbuf[0]->buff[0];
	PF_term[0][0] = 0;  /* PF_term[0] is used for a zero term. */
	PF.rbufs = rbuf;
/*
 		#] the receive buffers : 
 		#[ the actual tree :

	 calculate number of nodes in mergetree and allocate space for them
*/
	if ( numslaves < 3 ) numnodes = 1;
	else {
		numnodes = 2;
		while ( numnodes < numslaves ) numnodes *= 2;
		numnodes -= 1;
	}

	if ( PF_root == NULL )
	if ( ( PF_root = (NODE*)Malloc1(sizeof(NODE)*numnodes,"nodes in mergtree") ) == NULL )
		return(-1);
/*
		then initialize all the nodes
*/
	src = 1;
	for ( i = 0; i < numnodes; i++ ) {
		if ( 2*(i+1) <= numnodes ) {
			PF_root[i].left = &(PF_root[2*(i+1)-1]);
			PF_root[i].lsrc = 0;
		}
		else {
			PF_root[i].left = 0;
			if ( src < numtasks ) PF_root[i].lsrc = src++;
			else                  PF_root[i].lsrc = 0;
		}
		PF_root[i].lloser = 0;
	}
	for ( i = 0; i < numnodes; i++ ) {
		if ( 2*(i+1)+1 <= numnodes ) {
			PF_root[i].rght = &(PF_root[2*(i+1)]);
			PF_root[i].rsrc = 0;
		}
		else {
			PF_root[i].rght = 0;
			if (src<numtasks) PF_root[i].rsrc = src++;
			else              PF_root[i].rsrc = 0;
		}
		PF_root[i].rloser = 0;
	}
/*
 		#] the actual tree : 
*/
	return(numnodes);
}

/*
 		#] PF_InitTree : 
 		#[ PF_PutIn :
*/

/**
 * Replaces PutIn() on the master process and is used in PF_GetLoser().
 * It puts in the next term from slaveprocess \a src into the tree of losers
 * on the master and is a lot like GetTerm(). The main problems are:
 * buffering and decompression.
 *
 * If \a src == 0, it returns the zero term (PF_term[0]).
 *
 * If \a src != 0, it receives terms from another machine.
 * They are stored in the large sortbuffer which is divided into buff[i]
 * in the PF.rbufs[src], if PF.numrbufs > 1.
 *
 * @param  src  the source process.
 * @return      the next term.
 *
 * @remark  PF_term[0][0] == 0 (see InitTree()), so PF_term[0] can be used to be
 *          the returnvalue for a zero term (== no more terms).
 */
static WORD *PF_PutIn(int src)
{
	int tag;
	WORD im, r;
	WORD *m1, *m2;
	LONG size;
	PF_BUFFER *rbuf = PF.rbufs[src];
	int a = rbuf->active;
	int next = a+1 >= rbuf->numbufs ? 0 : a+1 ;
	WORD *lastterm = PF_term[src];
	WORD *term = rbuf->fill[a];

	if ( src <= 0 ) return(PF_term[0]);

	if ( rbuf->full[a] == rbuf->buff[a] + AM.MaxTer/sizeof(WORD) + 2 ) {
/*
			very first term from this src
*/
		tag = PF_WaitRbuf(rbuf,a,&size);
		rbuf->full[a] += size;
		if ( tag == PF_ENDBUFFER_MSGTAG ) *rbuf->full[a]++ = 0;
		else if ( rbuf->numbufs > 1 ) {
/*
				post a nonblock. recv. for the next buffer
*/
			rbuf->full[next] = rbuf->buff[next] + AM.MaxTer/sizeof(WORD) + 2;
			size = (LONG)(rbuf->stop[next] - rbuf->full[next]);
			PF_IRecvRbuf(rbuf,next,src);
		}
	}
	if ( *term == 0 && term != rbuf->full[a] ) return(PF_term[0]);
/*
		exception is for rare cases when the terms fitted exactly into buffer
*/
	if ( term + *term > rbuf->full[a] || term + 1 >= rbuf->full[a] ) {
newterms:
		m1 = rbuf->buff[next] + AM.MaxTer/sizeof(WORD) + 1;
		if ( *term < 0 || term == rbuf->full[a] ) {
/*
			copy term and lastterm to the new buffer, so that they end at m1
*/
			m2 = rbuf->full[a] - 1;
			while ( m2 >= term ) *m1-- = *m2--;
			rbuf->fill[next] = term = m1 + 1;
			m2 = lastterm + *lastterm - 1;
			while ( m2 >= lastterm ) *m1-- = *m2--;
			lastterm = m1 + 1;
		}
		else {
/*
			copy beginning of term to the next buffer so that it ends at m1
*/
			m2 = rbuf->full[a] - 1;
			while ( m2 >= term ) *m1-- = *m2--;
			rbuf->fill[next] = term = m1 + 1;
		}
		if ( rbuf->numbufs == 1 ) {
			rbuf->full[a] = rbuf->buff[a] + AM.MaxTer/sizeof(WORD) + 2;
			size = (LONG)(rbuf->stop[a] - rbuf->full[a]);
			PF_IRecvRbuf(rbuf,a,src);
		}
/*
			wait for new terms in the next buffer
*/
		rbuf->full[next] = rbuf->buff[next] + AM.MaxTer/sizeof(WORD) + 2;
		tag = PF_WaitRbuf(rbuf,next,&size);
		rbuf->full[next] += size;
		if ( tag == PF_ENDBUFFER_MSGTAG ) {
			*rbuf->full[next]++ = 0;
		}
		else if ( rbuf->numbufs > 1 ) {
/*
			post a nonblock. recv. for active buffer, it is not needed anymore
*/
			rbuf->full[a] = rbuf->buff[a] + AM.MaxTer/sizeof(WORD) + 2;
			size = (LONG)(rbuf->stop[a] - rbuf->full[a]);
			PF_IRecvRbuf(rbuf,a,src);
		}
/*
			now savely make next buffer active
*/
		a = rbuf->active = next;
	}

	if ( *term < 0 ) {
/*
			We need to decompress the term
*/
		im = *term;
		r = term[1] - im + 1;
		m1 = term + 2;
		m2 = lastterm - im + 1;
		while ( ++im <= 0 ) *--m1 = *--m2;
		*--m1 = r;
		rbuf->fill[a] = term = m1;
		if ( term + *term > rbuf->full[a] ) goto newterms;
	}
	rbuf->fill[a] += *term;
	return(term);
}

/*
 		#] PF_PutIn : 
 		#[ PF_GetLoser :
*/

/**
 * Finds the 'smallest' of all the PF_terms. Take also care of changing
 * coefficients and cancelling terms. When the coefficient changes, the new is
 * sitting in the array PF_newcpos, the length of the new coefficient in
 * PF_newclen. The original term will be untouched until it is copied to the
 * output buffer!
 *
 * Calling PF_GetLoser() with argument node will return the loser of the
 * subtree under node when the next term of the stream # PF_loser
 * (the last "loserstream") is filled into the tree.
 * PF_loser == 0 means we are just starting and should fill new terms into
 * all the leaves of the tree.
 *
 * @param  n  the node.
 * @return    the loser of the subtree under the node n.
 *            0 indicates there are no more terms.
 *            -1 indicates an error.
 */
static int PF_GetLoser(NODE *n)
{
	GETIDENTITY
	WORD comp;

	if ( PF_loser == 0 ) {
/*
			this is for the right initialization of the tree only
*/
		if ( n->left ) n->lloser = PF_GetLoser(n->left);
		else {
			n->lloser = n->lsrc;
			if ( *(PF_term[n->lsrc] = PF_PutIn(n->lsrc)) == 0) n->lloser = 0;
		}
		PF_loser = 0;
		if ( n->rght ) n->rloser = PF_GetLoser(n->rght);
		else{
			n->rloser = n->rsrc;
			if ( *(PF_term[n->rsrc] = PF_PutIn(n->rsrc)) == 0 ) n->rloser = 0;
		}
		PF_loser = 0;
	}
	else if ( PF_loser == n->lloser ) {
		if ( n->left ) n->lloser = PF_GetLoser(n->left);
		else {
			n->lloser = n->lsrc;
			if ( *(PF_term[n->lsrc] = PF_PutIn(n->lsrc)) == 0 ) n->lloser = 0;
		}
	}
	else if ( PF_loser == n->rloser ) {
newright:
		if ( n->rght ) n->rloser = PF_GetLoser(n->rght);
		else {
			n->rloser = n->rsrc;
			if ( *(PF_term[n->rsrc] = PF_PutIn(n->rsrc)) == 0 ) n->rloser = 0;
		}
	}
	if ( n->lloser > 0 && n->rloser > 0 ) {
		comp = CompareTerms(BHEAD PF_term[n->lloser],PF_term[n->rloser],(WORD)0);
		if ( comp > 0 )     return(n->lloser);
		else if (comp < 0 ) return(n->rloser);
		else {
/*
 		#[ terms are equal :
*/
			WORD *lcpos, *rcpos;
			UWORD *newcpos;
			WORD lclen, rclen, newclen, newnlen;

			if ( AT.SS->PolyWise ) {
/*
			#[ Here we work with PolyFun :
*/
				WORD *tt1, *w;
				WORD r1,r2;
				WORD *ml = PF_term[n->lloser];
				WORD *mr = PF_term[n->rloser];

				if ( ( r1 = (int)*PF_term[n->lloser] ) <= 0 ) r1 = 20;
				if ( ( r2 = (int)*PF_term[n->rloser] ) <= 0 ) r2 = 20;
				tt1 = ml;
				ml += AT.SS->PolyWise;
				mr += AT.SS->PolyWise;
				w = AT.WorkPointer;
				if ( w + ml[1] + mr[1] > AT.WorkTop ) {
					MesPrint("A WorkSpace of %10l is too small",AM.WorkSize);
					Terminate(-1);
				}
				AddArgs(ml,mr,w);
				r1 = w[1];
				if ( r1 <= FUNHEAD ) {
					goto cancelled;
				}
				if ( r1 == ml[1] ) {
					NCOPY(ml,w,r1);
				}
				else if ( r1 < ml[1] ) {
					r2 = ml[1] - r1;
					mr = w + r1;
					ml += ml[1];
					while ( --r1 >= 0 ) *--ml = *--mr;
					mr = ml - r2;
					r1 = AT.SS->PolyWise;
					while ( --r1 >= 0 ) *--ml = *--mr;
					*ml -= r2;
					PF_term[n->lloser] = ml;
				}
				else {
					r2 = r1 - ml[1];
					if ( r2 > 2*AM.MaxTal )
						MesPrint("warning: new term in polyfun is large");
					mr = tt1 - r2;
					r1 = AT.SS->PolyWise;
					ml = tt1;
					*ml += r2;
					PF_term[n->lloser] = mr;
					NCOPY(mr,ml,r1);
					r1 = w[1];
					NCOPY(mr,w,r1);
				}
				PF_newclen[n->rloser] = 0;
				PF_loser = n->rloser;
				goto newright;
		/*
			#] Here we work with PolyFun : 
*/
			}
/* Please verify that the = shouldn't have been == */
			if ( ( lclen = PF_newclen[n->lloser] ) != 0 ) lcpos = PF_newcpos[n->lloser];
			else {
				lcpos = PF_term[n->lloser];
				lclen = *(lcpos += *lcpos - 1);
				lcpos -= ABS(lclen) - 1;
			}
			if ( ( rclen = PF_newclen[n->rloser] ) != 0 ) rcpos = PF_newcpos[n->rloser];
			else {
				rcpos = PF_term[n->rloser];
				rclen = *(rcpos += *rcpos - 1);
				rcpos -= ABS(rclen) -1;
			}
			lclen = ( (lclen > 0) ? (lclen-1) : (lclen+1) ) >> 1;
			rclen = ( (rclen > 0) ? (rclen-1) : (rclen+1) ) >> 1;
			newcpos = PF_ScratchSpace;
			if ( AddRat(BHEAD (UWORD *)lcpos,lclen,(UWORD *)rcpos,rclen,newcpos,&newnlen) ) return(-1);
			if ( AN.ncmod != 0 ) {
				if ( ( AC.modmode & POSNEG ) != 0 ) {
					NormalModulus(newcpos,&newnlen);
				}
				if ( BigLong(newcpos,newnlen,(UWORD *)AC.cmod,ABS(AN.ncmod)) >=0 ) {
					WORD ii;
					SubPLon(newcpos,newnlen,(UWORD *)AC.cmod,ABS(AN.ncmod),newcpos,&newnlen);
					newcpos[newnlen] = 1;
					for ( ii = 1; ii < newnlen; ii++ ) newcpos[newnlen+ii] = 0;
				}
			}
			if ( newnlen == 0 ) {
/*
					terms cancel, get loser of left subtree and then of right subtree
*/
cancelled:
				PF_loser = n->lloser;
				PF_newclen[n->lloser] = 0;
				if ( n->left ) n->lloser = PF_GetLoser(n->left);
				else {
					n->lloser = n->lsrc;
					if ( *(PF_term[n->lsrc] = PF_PutIn(n->lsrc)) == 0 ) n->lloser = 0;
				}
				PF_loser = n->rloser;
				PF_newclen[n->rloser] = 0;
				goto newright;
			}
			else {
/*
					keep the left term and get the loser of right subtree
*/
				newnlen <<= 1;
				newclen = ( newnlen > 0 ) ? ( newnlen + 1 ) : ( newnlen - 1 );
				if ( newnlen < 0 ) newnlen = -newnlen;
				PF_newclen[n->lloser] = newclen;
				lcpos = PF_newcpos[n->lloser];
				if ( newclen < 0 ) newclen = -newclen;
				while ( newclen-- ) *lcpos++ = *newcpos++;
				PF_loser = n->rloser;
				PF_newclen[n->rloser] = 0;
				goto newright;
			}
/*
 		#] terms are equal : 
*/
		}
	}
	if (n->lloser > 0) return(n->lloser);
	if (n->rloser > 0) return(n->rloser);
	return(0);
}
/*
 		#] PF_GetLoser : 
 		#[ PF_EndSort :
*/

/**
 * Finishes a master sorting with collecting terms from slaves.
 * Called by EndSort().
 *
 * If this is not the masterprocess, just initialize the sendbuffers and
 * return 0, else PF_EndSort() sends the rest of the terms in the sendbuffer
 * to the next slave and a dummy message to all slaves with tag
 * PF_ENDSORT_MSGTAG. Then it receives the sorted terms, sorts them using a
 * recursive 'tree of losers' (PF_GetLoser()) and writes them to the
 * outputfile.
 *
 * @return   1  if the sorting on the master was done.
 *           0  if EndSort() still must perform a regular sorting becuase it is not
 *              at the ground level or not on the master or in the sequential mode
 *              or in the InParallel mode.
 *          -1  if an error occured.
 *
 * @remark  The slaves will send the sorted terms back to the master in the regular
 *          sorting (after the initialization of the send buffer in PF_EndSort()).
 *          See PutOut() and FlushOut().
 *
 * @remark  This function has been changed such that when it returns 1,
 *          AM.S0->TermsLeft is set correctly. But AM.S0->GenTerms is not set:
 *          it will be set after collecting the statistics from the slaves
 *          at the end of PF_Processor(). (TU 30 Jun 2011)
 */
int PF_EndSort(void)
{
	GETIDENTITY
	FILEHANDLE *fout = AR.outfile;
	PF_BUFFER *sbuf=PF.sbuf;
	SORTING *S = AT.SS;
	WORD *outterm,*pp;
	LONG size, noutterms;
	POSITION position;
	WORD i,cc;

	if ( AT.SS != AM.S0 || AC.mparallelflag != PARALLELFLAG || PF.exprtodo >= 0 ) return(0);

	if ( PF.me != MASTER ) {
/*
 		#[ the slaves have to initialize their sendbuffer :

		this is a slave and it's PObuffer should be the minimum of the
		sortiosize on the master and the POsize of our file.
		First save the original PObuffer and POstop of the outfile
*/
		size = (S->sTop2 - S->lBuffer - 1)/(PF.numtasks - 1);
		size -= (AM.MaxTer/sizeof(WORD) + 2);
		if ( fout->POsize < (LONG)(size*sizeof(WORD)) ) size = fout->POsize/sizeof(WORD);
		if ( sbuf == 0 ) {
			if ( (sbuf = PF_AllocBuf(PF.numsbufs,size*sizeof(WORD),1)) == 0 ) return(-1);
			sbuf->buff[0] = fout->PObuffer;
			sbuf->stop[0] = fout->PObuffer+size;
			if ( sbuf->stop[0] > fout->POstop ) return(-1);
			sbuf->active = 0;
		}
		for ( i = 0; i < PF.numsbufs; i++ )
			sbuf->fill[i] = sbuf->full[i] = sbuf->buff[i];

		PF.sbuf = sbuf;
		fout->PObuffer = sbuf->buff[sbuf->active];
		fout->POstop = sbuf->stop[sbuf->active];
		fout->POsize = size*sizeof(WORD);
		fout->POfill = fout->POfull = fout->PObuffer;
/*
 		#] the slaves have to initialize their sendbuffer : 
*/
		return(0);
	}
/*
		this waits for all slaves to be ready to send terms back
*/
	PF_WaitAllSlaves(); /* Note, the returned value should be 0 on success. */
/*
		Now collect the terms of all slaves and merge them.
		PF_GetLoser gives the position of the smallest term, which is the real
		work. The smallest term needs to be copied to the outbuf: use PutOut.
*/
	PF_InitTree();
	S->PolyFlag = AR.PolyFun ? AR.PolyFunType : 0;
	*AR.CompressPointer = 0;
	PUTZERO(position);

	noutterms = 0;

	while ( PF_loser >= 0 ) {
		if ( (PF_loser = PF_GetLoser(PF_root)) == 0 ) break;
		outterm = PF_term[PF_loser];
		noutterms++;

		if ( PF_newclen[PF_loser] != 0 ) {
/*
			#[ this is only when new coeff was too long :
*/
			outterm = PF_WorkSpace;
			pp = PF_term[PF_loser];
			cc = *pp;
			while ( cc-- ) *outterm++ = *pp++;
			outterm = (outterm[-1] > 0) ? outterm-outterm[-1] : outterm+outterm[-1];
			if ( PF_newclen[PF_loser] > 0 ) cc =  (WORD)PF_newclen[PF_loser] - 1;
			else                            cc = -(WORD)PF_newclen[PF_loser] - 1;
			pp =  PF_newcpos[PF_loser];
			while ( cc-- ) *outterm++ = *pp++;
			*outterm++ = PF_newclen[PF_loser];
			*PF_WorkSpace = outterm - PF_WorkSpace;
			outterm = PF_WorkSpace;
			*PF_newcpos[PF_loser] = 0;
			PF_newclen[PF_loser] = 0;
/*
			#] this is only when new coeff was too long : 
*/
		}
		PRINTFBUF("PF_EndSort to PutOut: ",outterm,*outterm);
		PutOut(BHEAD outterm,&position,fout,1);
	}
	if ( FlushOut(&position,fout,0) ) return(-1);
	S->TermsLeft = PF_goutterms = noutterms;
	PF_exprsize = position;
	return(1);
}

/*
 		#] PF_EndSort : 
  	#] sort.c : 
  	#[ proces.c :
 		#[ variables :
*/

static  WORD *PF_CurrentBracket;

/*
 		#] variables : 
 		#[ PF_GetTerm :
*/

/**
 * Replaces GetTerm() on the slaves, which get their terms from the master,
 * not the infile anymore, is nonblocking and buffered ...
 * use AR.infile->PObuffer as buffer. For the moment, don't care
 * about compression, since terms come uncompressed from master.
 *
 * To enable keep-brackets when AR.DeferFlag isset, we need to do some
 * preparation here:
 *   \li  copy the part ouside brackets to current_bracket
 *   \li  skip term if part outside brackets is same as for last term
 *   \li  if POfill >= POfull receive new terms as usual
 *
 * Different from GetTerm() we use an extra buffer for the part outside brackets:
 * PF_CurrentBracket().
 *
 * @param[out]  term  the buffer to store the next term.
 * @return            the length of the next term.
 */
static WORD PF_GetTerm(WORD *term)
{
	GETIDENTITY
	FILEHANDLE *fi;
	WORD i;
	WORD *next, *np, *last, *lp = 0, *nextstop, *tp=term;

	/* Only on the slaves. */

	if ( AC.NumberOfRhsExprInModule && PF.rhsInParallel )
		fi = &PF.slavebuf;
	else
		fi = AR.infile;

	AN.deferskipped = 0;
	if ( fi->POfill >= fi->POfull || fi->POfull == fi->PObuffer ) {
ReceiveNew:
	  {
/*
 		#[ receive new terms from master :
*/
		int src = MASTER, tag;
		int follow = 0;
		LONG size,cpu,space = 0;

		if ( PF.log ) {
			fprintf(stderr,"[%d] Starting to send to Master\n",PF.me);
			fflush(stderr);
		}

		PF_Send(MASTER,PF_READY_MSGTAG,0);
		cpu = TimeCPU(1);
		PF_Pack(&cpu               ,1,PF_LONG);
		PF_Pack(&space             ,1,PF_LONG);
		PF_Pack(&PF_linterms       ,1,PF_LONG);
		PF_Pack(&(AM.S0->GenTerms) ,1,PF_LONG);
		PF_Pack(&(AM.S0->TermsLeft),1,PF_LONG);
		PF_Pack(&follow            ,1,PF_INT );

		if ( PF.log ) {
			fprintf(stderr,"[%d] Now sending with tag = %d\n",PF.me,PF_READY_MSGTAG);
			fflush(stderr);
		}

		PF_Send(MASTER,PF_READY_MSGTAG,1);

		if ( PF.log ) {
			fprintf(stderr,"[%d] returning from send\n",PF.me);
			fflush(stderr);
		}

		size = fi->POstop - fi->PObuffer - 1;
#ifdef AbsolutelyExtra
		PF_Receive(MASTER,PF_ANY_MSGTAG,&src,&tag);
#ifdef MPI2
		if ( tag == PF_TERM_MSGTAG ) {
			PF_UnPack(&size, 1, PF_LONG);
			if ( PF_Put_target(src) == 0 ) {
				printf("PF_Put_target error ...\n");
			}
		}
		else {
			PF_RecvWbuf(fi->PObuffer,&size,&src);
		}
#else
		PF_RecvWbuf(fi->PObuffer,&size,&src);
#endif
#endif
		tag=PF_RecvWbuf(fi->PObuffer,&size,&src);

		fi->POfill = fi->PObuffer;
		/* Get AN.ninterms which sits in the first 2 WORDs. */
		{
			LONG ninterms;
			UNPACK_LONG(fi->POfill, ninterms);
			if ( *fi->POfill ) {
				DBGOUT_NINTERMS(2, ("PF.me=%d AN.ninterms=%d PF_linterms=%d ninterms=%d GET\n", (int)PF.me, (int)AN.ninterms, (int)PF_linterms, (int)ninterms));
				AN.ninterms = ninterms - 1;
			} else {
				DBGOUT_NINTERMS(2, ("PF.me=%d AN.ninterms=%d PF_linterms=%d ninterms=%d GETEND\n", (int)PF.me, (int)AN.ninterms, (int)PF_linterms, (int)ninterms));
			}
		}
		fi->POfull = fi->PObuffer + size;
		if ( tag == PF_ENDSORT_MSGTAG ) *fi->POfull++ = 0;
/*
 		#] receive new terms from master : 
*/
	  }
	  if ( PF_CurrentBracket ) *PF_CurrentBracket = 0;
	}
	if ( *fi->POfill == 0 ) {
		fi->POfill = fi->POfull = fi->PObuffer;
		*term = 0;
		goto RegRet;
	}
	if ( AR.DeferFlag ) {
		if ( !PF_CurrentBracket ) {
/*
 		#[ alloc space :
*/
			PF_CurrentBracket =
					(WORD*)Malloc1(AM.MaxTer,"PF_CurrentBracket");
			*PF_CurrentBracket = 0;
/*
 		#] alloc space : 
*/
		}
		while ( *PF_CurrentBracket ) {  /* "for each term in the buffer" */
/*
 		#[ test : bracket & skip if it's equal to the last in PF_CurrentBracket
*/
			next = fi->POfill;
			nextstop = next + *next; nextstop -= ABS(nextstop[-1]);
			next++;
			last = PF_CurrentBracket+1;
			while ( next < nextstop ) {
/*
					scan the next term and PF_CurrentBracket
*/
				if ( *last == HAAKJE && *next == HAAKJE ) {
/*
					the part outside brackets is equal => skip this term
*/
					PRINTFBUF("PF_GetTerm skips",fi->POfill,*fi->POfill);
					break;
				}
/*
					check if the current subterms are equal
*/
				np = next; next += next[1];
				lp = last; last += last[1];
				while ( np < next ) if ( *lp++ != *np++ ) goto strip;
			}
/*
				go on to next term
*/
			fi->POfill += *fi->POfill;
			AN.deferskipped++;
/*
				the usual checks
*/
			if ( fi->POfill >= fi->POfull || fi->POfull == fi->PObuffer )
				goto ReceiveNew;
			if ( *fi->POfill == 0 ) {
				fi->POfill = fi->POfull = fi->PObuffer;
				*term = 0;
				goto RegRet;
			}
/*
 		#] test : 
*/
		}
/*
 		#[ copy :

		this term to CurrentBracket and the part outside of bracket
		to WorkSpace at term
*/
strip:
		next = fi->POfill;
		nextstop = next + *next; nextstop -= ABS(nextstop[-1]);
		next++;
		tp++;
		lp = PF_CurrentBracket + 1;
		while ( next < nextstop ) {
			if ( *next == HAAKJE ) {
				fi->POfill += *fi->POfill;
				while ( next < fi->POfill ) *lp++ = *next++;
				*PF_CurrentBracket = lp - PF_CurrentBracket;
				*lp = 0;
				*tp++ = 1;
				*tp++ = 1;
				*tp++ = 3;
				*term = WORDDIF(tp,term);
				PRINTFBUF("PF_GetTerm new brack",PF_CurrentBracket,*PF_CurrentBracket);
				PRINTFBUF("PF_GetTerm POfill",fi->POfill,*fi->POfill);
				goto RegRet;
			}
			np = next; next += next[1];
			while ( np < next ) *tp++ = *lp++ = *np++;
		}
		tp = term;
/*
 		#] copy : 
*/
	}

	i = *fi->POfill;
	while ( i-- ) *tp++ = *fi->POfill++;
RegRet:
	PRINTFBUF("PF_GetTerm returns",term,*term);
	return(*term);
}

/*
 		#] PF_GetTerm : 
 		#[ PF_Deferred :
*/

/**
 * Replaces Deferred() on the slaves.
 *
 * @param  term   the term that must be multiplied by the contents of
 *                the current bracket.
 * @param  level  the compiler level.
 * @return        0 if OK, nonzero on error.
 */
WORD PF_Deferred(WORD *term, WORD level)
{
	GETIDENTITY
	WORD *bra, *bstop;
	WORD *tstart;
	WORD *next = AR.infile->POfill;
	WORD *termout = AT.WorkPointer;
	WORD *oldwork = AT.WorkPointer;

	AT.WorkPointer = (WORD *)((UBYTE *)(AT.WorkPointer) + AM.MaxTer);
	AR.DeferFlag = 0;

	PRINTFBUF("PF_Deferred (Term)   ",term,*term);
	PRINTFBUF("PF_Deferred (Bracket)",PF_CurrentBracket,*PF_CurrentBracket);

	bra = bstop = PF_CurrentBracket;
	if ( *bstop > 0 ) {
		bstop += *bstop;
		bstop -= ABS(bstop[-1]);
	}
	bra++;
	while ( *bra != HAAKJE && bra < bstop ) bra += bra[1];
	if ( bra >= bstop ) {	/* No deferred action! */
		AT.WorkPointer = term + *term;
		if ( Generator(BHEAD term,level) ) goto DefCall;
		AR.DeferFlag = 1;
		AT.WorkPointer = oldwork;
		return(0);
	}
	bstop = bra;
	tstart = bra + bra[1];
	bra = PF_CurrentBracket;
	tstart--;
	*tstart = bra + *bra - tstart;
	bra++;
/*
		Status of affairs:
		First bracket content starts at tstart.
		Next term starts at next.
		The outside of the bracket runs from bra = PF_CurrentBracket to bstop.
*/
	for(;;) {
		if ( InsertTerm(BHEAD term,0,AM.rbufnum,tstart,termout,0) < 0 ) {
			goto DefCall;
		}
/*
			call Generator with new composed term
*/
		AT.WorkPointer = termout + *termout;
		if ( Generator(BHEAD termout,level) ) goto DefCall;
		AT.WorkPointer = termout;
		tstart = next + 1;
		if ( tstart >= AR.infile->POfull ) goto ThatsIt;
		next += *next;
/*
			compare with current bracket
*/
		while ( bra <= bstop ) {
			if ( *bra != *tstart ) goto ThatsIt;
			bra++; tstart++;
		}
/*
			now bra and tstart should both be a HAAKJE
*/
		bra--; tstart--;
		if ( *bra != HAAKJE || *tstart != HAAKJE ) goto ThatsIt;
		tstart += tstart[1];
		tstart--;
		*tstart = next - tstart;
		bra = PF_CurrentBracket + 1;
	}

ThatsIt:
/*
	AT.WorkPointer = oldwork;
*/
	AR.DeferFlag = 1;
	return(0);
DefCall:
	MesCall("PF_Deferred");
	SETERROR(-1);
}

/*
 		#] PF_Deferred : 
 		#[ PF_Wait4Slave :
*/

static LONG **PF_W4Sstats = 0;

/**
 * Waits for the slave \a src to accept terms.
 *
 * @param  src  the slave for waiting (can be PF_ANY_SOURCE).
 * @return      the idle slave.
 */
static int PF_Wait4Slave(int src)
{
	int j, tag, next;

	PF_CatchErrorMessages(src);
	PF_Receive(src,PF_ANY_MSGTAG,&next,&tag);

	if ( tag != PF_READY_MSGTAG ) {
		MesPrint("[%d] PF_Wait4Slave: received MSGTAG %d",(WORD)PF.me,(WORD)tag);
		return(-1);
	}
	if ( PF_W4Sstats == 0 ) {
		PF_W4Sstats = (LONG**)Malloc1(sizeof(LONG*),"");
		PF_W4Sstats[0] = (LONG*)Malloc1(PF_STATS_SIZE*sizeof(LONG),"");
	}
	PF_UnPack(PF_W4Sstats[0],PF_STATS_SIZE,PF_LONG);
	PF_Statistics(PF_W4Sstats,next);

	PF_UnPack(&j,1,PF_INT);

	if ( j ) {
/*
		actions depending on rest of information in last message
*/
	}
	return(next);
}

/*
 		#] PF_Wait4Slave : 
 		#[ PF_Wait4SlaveIP :
*/
/*
	array of expression numbers for PF_InParallel processor.
	Each time the master sends expression "i" to the slave
	"next" it sets partodoexr[next]=i:
*/
static WORD *partodoexr=NULL;

/**
 * InParallel version of PF_Wait4Slave(). Returns tag as src.
 *
 * @param[in,out]  src  the slave for waiting (can be PF_ANY_SOURCE).
 *                      As output, the tag value of the idle slave.
 * @return              the idle slave.
 */
static int PF_Wait4SlaveIP(int *src)
{
	int j,tag,next;

	PF_CatchErrorMessages(*src);
	PF_Receive(*src,PF_ANY_MSGTAG,&next,&tag);
	*src=tag;
	if ( PF_W4Sstats == 0 ) {
		PF_W4Sstats = (LONG**)Malloc1(sizeof(LONG*),"");
		PF_W4Sstats[0] = (LONG*)Malloc1(PF_STATS_SIZE*sizeof(LONG),"");
	}

	PF_UnPack(PF_W4Sstats[0],PF_STATS_SIZE,PF_LONG);
	if ( tag == PF_DATA_MSGTAG )
		AR.CurExpr = partodoexr[next];
	PF_Statistics(PF_W4Sstats,next);

	PF_UnPack(&j,1,PF_INT);

	if ( j ) {
	/* actions depending on rest of information in last message */
	}

	return(next);
}
/*
 		#] PF_Wait4SlaveIP : 
 		#[ PF_WaitAllSlaves :
*/

/**
 * Waits until all slaves are ready to send terms back to the master.
 * If some slave is not working, it sends PF_ENDSORT_MSGTAG and waits for the answer.
 * Messages from slaves will be read only after all slaves are ready,
 * further in caller function.
 *
 * @return  0 if OK, nonzero on error.
 */
static int PF_WaitAllSlaves(void)
{
	int i, readySlaves, tag, next = PF_ANY_SOURCE;
	UBYTE *has_sent = 0;

	has_sent = (UBYTE*)Malloc1(sizeof(UBYTE)*(PF.numtasks + 1),"PF_WaitAllSlaves");
	for ( i = 0; i < PF.numtasks; i++ ) has_sent[i] = 0;

	for ( readySlaves = 1; readySlaves < PF.numtasks; ) {
		if ( next != PF_ANY_SOURCE) { /*Go to the next slave:*/
			do{ /*Note, here readySlaves<PF.numtasks, so this loop can't be infinite*/
				if ( ++next >= PF.numtasks ) next = 1;
			} while ( has_sent[next] == 1 );
		}
/*
			Here PF_ProbeWithCatchingErrorMessages() is BLOCKING function if next = PF_ANY_SOURCE:
*/
		tag = PF_ProbeWithCatchingErrorMessages(&next);
/*
			Here next != PF_ANY_SOURCE
*/
		switch ( tag ) {
			case PF_BUFFER_MSGTAG:
			case PF_ENDBUFFER_MSGTAG:
/*
					Slaves are ready to send their results back
*/
				if ( has_sent[next] == 0 ) {
					has_sent[next] = 1;
					readySlaves++;
				}
				else {  /*error?*/
					fprintf(stderr,"ERROR next=%d tag=%d\n",next,tag);
				}
/*
					Note, we do NOT read results here! Messages from these slaves will be read
					only after all slaves are ready, further in caller function
*/
				break;
			case 0:
/*
					The slave is not ready. Just go to  the next slave.
					It may appear that there are no more ready slaves, and the master
					will wait them in infinite loop. Stupid situation - the master can
					receive buffers from ready slaves!
*/
#ifdef PF_WITH_SCHED_YIELD
/*
						Relinquish the processor:
*/
					sched_yield();
#endif
				break;
			case PF_DATA_MSGTAG:
				tag=next;
				next=PF_Wait4SlaveIP(&tag);
/*
	tag must be == PF_DATA_MSGTAG!
*/
				PF_Statistics(PF_stats,0);
				PF_Slave2MasterIP(next);
				PF_Master2SlaveIP(next,NULL);
				if ( has_sent[next] == 0 ) {
					has_sent[next]=1;
					readySlaves++;
				}else{
					/*error?*/
					fprintf(stderr,"ERROR next=%d tag=%d\n",next,tag);
				}/*if ( has_sent[next] == 0 )*/
				break;
			case PF_EMPTY_MSGTAG:
				tag=next;
				next=PF_Wait4SlaveIP(&tag);
/*
	tag must be == PF_EMPTY_MSGTAG!
*/
				PF_Master2SlaveIP(next,NULL);
				if ( has_sent[next] == 0 ) {
					has_sent[next]=1;
					readySlaves++;
				}else{
					/*error?*/
					fprintf(stderr,"ERROR next=%d tag=%d\n",next,tag);
				}/*if ( has_sent[next] == 0 )*/
				break;
			case PF_READY_MSGTAG:
/*
					idle slave
					May be only PF_READY_MSGTAG:
*/
				next = PF_Wait4Slave(next);
				if ( next == -1 ) return(next); /*Cannot be!*/
				if ( has_sent[0] == 0 ) {  /*Send the last chunk to the slave*/
					PF.sbuf->active = 0;
					has_sent[0] = 1;
				}
				else {
/*
						Last chunk was sent, so just send to slave ENDSORT
						AN.ninterms must be sent because the slave expects it:
*/
					PACK_LONG(PF.sbuf->fill[next], AN.ninterms);
/*
						This will tell to the slave that there are no more terms:
*/
					*(PF.sbuf->fill[next])++ = 0;
					PF.sbuf->active = next;
				}
/*
					Send ENDSORT
*/
				PF_ISendSbuf(next,PF_ENDSORT_MSGTAG);
				break;
			default:
/*
					Error?
					Indicates the error. This will force exit from the main loop:
*/
				MesPrint("!!!Unexpected MPI message src=%d tag=%d.", next, tag);
				readySlaves = PF.numtasks+1;
				break;
		}
	}

	if ( has_sent ) M_free(has_sent,"PF_WaitAllSlaves");
/*
		0 on sucess (exit from the main loop by loop condition), or -1 if fails
		(exit from the main loop since readySlaves=PF.numtasks+1):
*/
	return(PF.numtasks-readySlaves);
}

/*
 		#] PF_WaitAllSlaves : 
 		#[ PF_Processor :
*/

/**
 * Replaces parts of Processor() on the masters and slaves.
 * On the master PF_Processor() is responsible for proper distribution of terms
 * from the input file to the slaves.
 * On the slaves it calls Generator() for all the terms that this process gets,
 * but PF_GetTerm() gets terms from the master (not directly from infile).
 *
 * @param  e               The pointer to the current expression.
 * @param  i               The index for the current expression.
 * @param  LastExpression  The flag indicating whether it is the last expression.
 * @return                 0 if OK, nonzero on error.
 */
int PF_Processor(EXPRESSIONS e, WORD i, WORD LastExpression)
{
	GETIDENTITY
	WORD *term = AT.WorkPointer;
	LONG dd = 0, ll;
	PF_BUFFER *sb = PF.sbuf;
	WORD j, *s, next;
	LONG size, cpu;
	POSITION position;
	int k, src, tag, attach;
	FILEHANDLE *oldoutfile = AR.outfile;

#ifdef MPI2
	if ( PF_shared_buff == NULL ) {
		if ( PF_SMWin_Init() == 0 ) {
			MesPrint("PF_SMWin_Init error");
			exit(-1);
		}
	}
#endif

	if ( ( (WORD *)(((UBYTE *)(AT.WorkPointer)) + AM.MaxTer ) ) > AT.WorkTop ) return(MesWork());
/*
		allocate and/or reset the variables used for the redefine
*/
	if ( !PF.redef || NumPre > (LONG)PF.numredefs ) {
		if (PF.redef) M_free(PF.redef,"resize PF.redef");
		PF.numredefs = (LONG)NumPre;
		PF.redef = (LONG*)Malloc1(PF.numredefs*sizeof(LONG),"PF.redef");
	}
	PF.mnumredefs = 0;
	for ( ll = 0; ll < PF.numredefs; ll++ ) PF.redef[ll] = 0;

	if ( AC.mparallelflag != PARALLELFLAG ) return(0);

	if ( PF.me == MASTER ) {
/*
 		#[ Master:
			#[ write prototype to outfile:
*/
		LONG maxinterms;    /* the maximum number of terms in the next patch */
		int cmaxinterms;    /* a variable controling the transition of maxinterms */
		LONG termsinpatch;  /* the number of filled terms in the current patch */

		if ( PF.log && AC.CModule >= PF.log )
			MesPrint("[%d] working on expression %s in module %l",PF.me,EXPRNAME(i),AC.CModule);
		if ( GetTerm(BHEAD term) <= 0 ) {
			MesPrint("[%d] Expression %d has problems in scratchfile",PF.me,i);
			return(-1);
		}
		if ( AC.bracketindexflag > 0 ) OpenBracketIndex(i);
		term[3] = i;
		AR.CurExpr = i;
		if ( AR.outtohide ) {
			SeekScratch(AR.hidefile,&position);
			e->onfile = position;
			if ( PutOut(BHEAD term,&position,AR.hidefile,0) < 0 ) return(-1);
		}
		else {
			SeekScratch(AR.outfile,&position);
			e->onfile = position;
			if ( PutOut(BHEAD term,&position,AR.outfile,0) < 0 ) return(-1);
		}
		AR.Eside = RHSIDE;
/*
			#] write prototype to outfile: 
			#[ initialize sendbuffer if necessary:

			the size of the sendbufs is:
			MIN(1/PF.numtasks*(AT.SS->sBufsize+AT.SS->lBufsize),AR.infile->POsize)
			No allocation for extra buffers necessary, just make sb->buf... point
			to the right places in the sortbuffers.
*/
		NewSort(BHEAD0);   /* we need AT.SS to be set for this!!! */
		if ( sb == 0 || sb->buff[0] != AT.SS->lBuffer ) {
			size = (LONG)((AT.SS->sTop2 - AT.SS->lBuffer)/(PF.numtasks));
			if ( size > (LONG)(AR.infile->POsize/sizeof(WORD) - 1) )
				size = AR.infile->POsize/sizeof(WORD) - 1;
			if ( sb == 0 ) {
				if ( ( sb = PF_AllocBuf(PF.numtasks,size*sizeof(WORD),PF.numtasks) ) == NULL )
					return(-1);
			}
			sb->buff[0] = AT.SS->lBuffer;
			sb->full[0] = sb->fill[0] = sb->buff[0];
			for ( j = 1; j < PF.numtasks; j++ ) {
				sb->stop[j-1] = sb->buff[j] = sb->buff[j-1] + size;
			}
			sb->stop[PF.numtasks-1] = sb->buff[PF.numtasks-1] + size;
			PF.sbuf = sb;
		}
		for ( j = 0; j < PF.numtasks; j++ ) {
			sb->full[j] = sb->fill[j] = sb->buff[j];
		}
/*
			#] initialize sendbuffer if necessary: 
			#[ loop for all terms in infile:

			copy them always to sb->buff[0], when that is full, wait for
			next slave to accept terms, exchange sb->buff[0] and
			sb->buff[next], send sb->buff[next] to next slave and go on
			filling the now empty sb->buff[0].
*/
		AR.DeferFlag = 0;  /* The master leave the brackets!!! */
		AN.ninterms = 0;
		termsinpatch = 0;
		maxinterms = PF_maxinterms / 100;
		if ( maxinterms < 2 ) maxinterms = 2;
		cmaxinterms = 0;
		PACK_LONG(sb->fill[0], 1);
		while ( GetTerm(BHEAD term) ) {
			AN.ninterms++; dd = AN.deferskipped;
			if ( AC.CollectFun && *term <= (LONG)(AM.MaxTer/(2*sizeof(WORD))) ) {
				if ( GetMoreTerms(term) < 0 ) {
					LowerSortLevel(); return(-1);
				}
			}
			PRINTFBUF("PF_Processor gets",term,*term);
			if ( termsinpatch >= maxinterms || sb->fill[0] + *term >= sb->stop[0] ) {
				if ( cmaxinterms >= PF.numtasks ) {
					maxinterms*=2;
					if ( maxinterms >= PF_maxinterms ) {
						cmaxinterms=-2;
						maxinterms = PF_maxinterms;
					}
				}/*if ( cmaxinterms >= PF.numtasks ) */
				else if ( cmaxinterms >= 0 )
					cmaxinterms++;
				next = PF_Wait4Slave(PF_ANY_SOURCE);

				sb->fill[next] = sb->fill[0];
				sb->full[next] = sb->full[0];
				SWAP(sb->stop[next], sb->stop[0]);
				SWAP(sb->buff[next], sb->buff[0]);
				sb->fill[0] = sb->full[0] = sb->buff[0];
				sb->active = next;

#ifdef MPI2
				if ( PF_Put_origin(next) == 0 ) {
					printf("PF_Put_origin error...\n");
				}
#else
				PF_ISendSbuf(next,PF_TERM_MSGTAG);
#endif

				PACK_LONG(sb->fill[0], AN.ninterms);
				termsinpatch = 0;
			}
			j = *(s = term);
			while ( j-- ) { *(sb->fill[0])++ = *s++; }
			termsinpatch++;
		}
		/* NOTE: The last chunk will be sent to a slave at EndSort() => PF_EndSort()
		 *       => PF_WaitAllSlaves(). */
		AN.ninterms += dd;
/*
			#] loop for all terms in infile: 
			#[ Clean up & EndSort:
*/
		if ( LastExpression ) {
			if ( AR.infile->handle >= 0 ) {
				CloseFile(AR.infile->handle);
				AR.infile->handle = -1;
				remove(AR.infile->name);
				PUTZERO(AR.infile->POposition);
				AR.infile->POfill = AR.infile->POfull = AR.infile->PObuffer;
			}
		}
		if ( AR.outtohide ) AR.outfile = AR.hidefile;
		if ( EndSort(BHEAD AM.S0->sBuffer,0,0) < 0 ) return(-1);
		if ( AR.outtohide ) {
			AR.outfile = oldoutfile;
			AR.hidefile->POfull = AR.hidefile->POfill;
		}
		if ( AM.S0->TermsLeft ) e->vflags &= ~ISZERO;
		else e->vflags |= ISZERO;
		/* FIXME: AR.expchanged doesn't reflect those of the slaves. (TU 30 Jun 2011) */
		if ( AR.expchanged == 0 ) e->vflags |= ISUNMODIFIED;
		if ( AM.S0->TermsLeft ) AR.expflags |= ISZERO;
		if ( AR.expchanged ) AR.expflags |= ISUNMODIFIED;
		AR.GetFile = 0;
		AR.outtohide = 0;
/*
			#] Clean up & EndSort: 
			#[ Collect (stats,prepro,...):
*/
		DBGOUT_NINTERMS(1, ("PF.me=%d AN.ninterms=%d ENDSORT\n", (int)PF.me, (int)AN.ninterms));
		PF_CatchErrorMessagesForAll();
		for ( k = 1; k < PF.numtasks; k++ ) {
			PF_Receive(PF_ANY_SOURCE,PF_ENDSORT_MSGTAG,&src,&tag);
			PF_UnPack(PF_stats[src],PF_STATS_SIZE,PF_LONG);
			PF_UnPack(&attach,1,PF_INT);
			if ( attach ) {
/*
					actions depending on rest of information in last message
*/
				switch ( attach ) {
					case PF_ATTACH_REDEF: {
						int ll, kk, ii;
						UBYTE *value = 0;
						LONG redef;
						PF_UnPack(&kk,1,PF_INT);
						while ( --kk >= 0 ) {
							PF_UnPack(&ii,1,PF_INT);
							PF_UnPack(&ll,1,PF_INT);
							value = (UBYTE*)Malloc1(ll,"redef value");
							PF_UnPack(value,ll,PF_BYTE);
							PF_UnPack(&redef,1,PF_LONG);
							if ( redef > PF.redef[ii] ) {
								if ( PF.redef[ii] == 0 ) /*This term was not counted yet*/
									PF.mnumredefs++;     /*Count it!*/
								PF.redef[ii] = redef;    /*Store the latest term number*/
								PutPreVar(PreVar[ii].name,value,0,1);
/*
									Redefine preVar
									I reduced the possibility to transfer prepro
									variables with args for the moment
*/
							}
						}
/*
							here we should free the allocated memory of value & name ??
*/
						if (value) M_free(value,"redef value");
						break;
					}
					default:
/*
						here should go an error message
*/
						break;
				}
			}
		}
		if ( ! AC.OldParallelStats ) {
			/* Now we can calculate AT.SS->GenTerms from the statistics of the slaves. */
			LONG genterms = 0;
			for ( k = 1; k < PF.numtasks; k++ ) {
				genterms += PF_stats[k][3];
			}
			AT.SS->GenTerms = genterms;
			WriteStats(&PF_exprsize, 2);
		}
		PF_Statistics(PF_stats,0);
/*
			#] Collect (stats,prepro,...): 

		This operation is moved to the beginning of each block, see PreProcessor
		in pre.c.

 		#] Master: 
*/
	}
	else {
/*
 		#[ Slave :
*/
/*
			#[ Generator Loop & EndSort :

			loop for all terms to get from master, call Generator for each of them
			then call EndSort and do cleanup (to be implemented)
*/
		SeekScratch(AR.outfile,&position);
		e->onfile = position;
		AR.DeferFlag = AC.ComDefer;
		AR.Eside = RHSIDE;
		NewSort(BHEAD0);
		AN.ninterms = 0;
		PF_linterms = 0;
		PF.parallel = 1;
#ifdef MPI2
		AR.infile->POfull = AR.infile->POfill = AR.infile->PObuffer = PF_shared_buff;
#endif
		{
			FILEHANDLE *fi;
			if ( AC.NumberOfRhsExprInModule && PF.rhsInParallel )
				fi = &PF.slavebuf;
			else
				fi = AR.infile;
			fi->POfull = fi->POfill = fi->PObuffer;
		}
		/* FIXME: AN.ninterms is still broken when AN.deferskipped is non-zero.
		 *        It still needs some work, also in PF_GetTerm(). (TU 30 Aug 2011) */
		while ( PF_GetTerm(term) ) {
			PF_linterms++; AN.ninterms++; dd = AN.deferskipped;
			AT.WorkPointer = term + *term;
			AN.RepPoint = AT.RepCount + 1;
			AR.CurDum = ReNumber(BHEAD term);
			if ( AC.SymChangeFlag ) MarkDirty(term,DIRTYSYMFLAG);
			if ( ( AR.PolyFunType == 2 ) && ( AC.PolyRatFunChanged == 0 )
				&& ( e->status == LOCALEXPRESSION || e->status == GLOBALEXPRESSION ) ) {
				PolyFunClean(BHEAD term);
			}
			if ( Generator(BHEAD term,0) ) {
				MesPrint("[%d] PF_Processor: Error in Generator",PF.me);
				LowerSortLevel(); return(-1);
			}
			PF_linterms += dd; AN.ninterms += dd;
		}
		PF_linterms += dd; AN.ninterms += dd;
		if ( EndSort(BHEAD AM.S0->sBuffer,0,0) < 0 ) return(-1);
		DBGOUT_NINTERMS(1, ("PF.me=%d AN.ninterms=%d PF_linterms=%d ENDSORT\n", (int)PF.me, (int)AN.ninterms, (int)PF_linterms));
/*
			#] Generator Loop & EndSort : 
			#[ Collect (stats,prepro...) :
*/
		PF_Send(MASTER,PF_ENDSORT_MSGTAG,0);
		cpu = TimeCPU(1);
		size = 0;
		PF_Pack(&cpu               ,1,PF_LONG);
		PF_Pack(&size              ,1,PF_LONG);
		PF_Pack(&PF_linterms       ,1,PF_LONG);
		PF_Pack(&(AM.S0->GenTerms) ,1,PF_LONG);
		PF_Pack(&(AM.S0->TermsLeft),1,PF_LONG);
/*
			now handle the redefined Preprovars
*/
		k = attach = 0;
		for ( ll = 0; ll < PF.numredefs; ll++ ) { if (PF.redef[ll]) k++; }

		if ( k ) attach = PF_ATTACH_REDEF;
		PF_Pack(&attach,1,PF_INT);
		if ( k ) {
			int l;
			UBYTE *value, *p;

			PF_Pack(&k,1,PF_INT);
			k = NumPre;
			while ( --k >= 0 ) {
				if ( PF.redef[k] ) {
					PF_Pack(&k,1,PF_INT);

					l = 1;
					p = value = PreVar[k].value;
					while ( *p++ ) l++;
					PF_Pack(&l,1,PF_INT);
					PF_Pack(value,l,PF_BYTE);
					PF_Pack(&(PF.redef[k]),1,PF_LONG);
				}
			}
		}
		PF_Send(MASTER,PF_ENDSORT_MSGTAG,1);
/*
			#] Collect (stats,prepro...) : 

		This operation is moved to the beginning of each block, see PreProcessor
		in pre.c.

 		#] Slave : 
*/
		if ( PF.log ) {
			fprintf(stderr,"[%d|%ld] Endsort,Collect,Broadcast done\n",PF.me,AC.CModule);
			fflush(stderr);
		}
	}
	return(0);
}

/*
 		#] PF_Processor : 
  	#] proces.c : 
  	#[ startup :, prepro & compile
 		#[ PF_Init :
*/

/**
 * All the library independent stuff.
 * PF_LibInit() should do all library dependent initializations.
 *
 * @param  argc  pointer to the number of arguments.
 * @param  argv  pointer to the arguments.
 * @return       0 if OK, nonzero on error.
 */
int PF_Init(int *argc, char ***argv)
{
	UBYTE *fp, *ubp;
	char *c;
	int fpsize = 0;
/*
		this should definitly be somewhere else ...
*/
	PF_CurrentBracket = 0;

	PF.numtasks = 0; /* number of tasks, is determined in PF_Lib_Init or must be set before! */
	PF.numsbufs = 2; /* might be changed by LibInit ! */
	PF.numrbufs = 2; /* might be changed by LibInit ! */

	PF.numredefs = 0;
	PF.redef = 0;
	PF.mnumredefs = 0;

	PF_LibInit(argc,argv);
	PF_RealTime(PF_RESET);

	PF_maxinterms = 1000;
	PF.log = 0;
	PF.parallel = 0;
	PF_statsinterval = 10;
	PF.rhsInParallel=1;
	PF.exprbufsize=4096;/*in WORDs*/

/*
		If !=0, start of each module will be synchronized between all slaves and master
*/
	PF.synchro = 0;

	if ( PF.me == MASTER ) {
#ifdef PF_WITHGETENV
		if ( getenv("PF_SYNC") !=NULL ) {
			PF.synchro = 1;
			fprintf(stderr,"Start of each module is synchronized\n");
			fflush(stderr);
		}
/*
			get these from the environment at the moment sould be in setfile/tail
*/
		if ( ( c = getenv("PF_LOG") ) != 0 ) {
			if ( *c ) PF.log = (int)atoi(c);
			else PF.log = 1;
			fprintf(stderr,"[%d] changing PF.log to %d\n",PF.me,PF.log);
			fflush(stderr);
		}
		if ( ( c = (char*)getenv("PF_RBUFS") ) != 0 ) {
			PF.numrbufs = (int)atoi(c);
			fprintf(stderr,"[%d] changing numrbufs to: %d\n",PF.me,PF.numrbufs);
			fflush(stderr);
		}
		if ( ( c = (char*)getenv("PF_SBUFS") ) != 0 ) {
			PF.numsbufs = (int)atoi(c);
			fprintf(stderr,"[%d] changing numsbufs to: %d\n",PF.me,PF.numsbufs);
			fflush(stderr);
		}
		if ( PF.numsbufs > 10 ) PF.numsbufs = 10;
		if ( PF.numsbufs <  1 ) PF.numsbufs = 1;
		if ( PF.numrbufs >  2 ) PF.numrbufs = 2;
		if ( PF.numrbufs <  1 ) PF.numrbufs = 1;

		if ( ( c = getenv("PF_MAXINTERMS") ) ) {
			PF_maxinterms = (LONG)atoi(c);
			fprintf(stderr,"[%d] changing PF_maxinterms to %ld\n",PF.me,PF_maxinterms);
			fflush(stderr);
		}
		if ( ( c = getenv("PF_STATS") ) ) {
			PF_statsinterval = (int)atoi(c);
			fprintf(stderr,"[%d] changing PF_statsinterval to %ld\n",PF.me,PF_statsinterval);
			fflush(stderr);
			if ( PF_statsinterval < 1 ) PF_statsinterval = 10;
		}
		fp = (UBYTE*)getenv("FORMPATH");
		if ( fp ) {
			ubp = fp;
			while ( *ubp++ ) fpsize++;
			if ( AC.OldParallelStats ) {
				fprintf(stderr,"[%d] changing Path to %s\n",PF.me,fp);
				fflush(stderr);
			}
		}
		else {
			fp = (UBYTE*)"";
			fpsize++;
		}
		fpsize++;
#endif
	}
/*
  	#[ BroadCast settings from getenv: could also be done in PF_DoSetup
*/
	if ( PF.me == MASTER ) {
		PF_BroadCast(0);
		PF_Pack(&PF.log,1,PF_INT);
		PF_Pack(&PF.synchro,1,PF_WORD);
		PF_Pack(&PF.numrbufs,1,PF_WORD);
		PF_Pack(&PF.numsbufs,1,PF_WORD);
		PF_Pack(&PF_maxinterms,1,PF_LONG);
		PF_Pack(&fpsize,1,PF_INT);
		PF_Pack(fp,(LONG)fpsize,PF_BYTE);
	}
	PF_BroadCast(1);
	if ( PF.me != MASTER ) {
		PF_UnPack(&PF.log,1,PF_INT);
		PF_UnPack(&PF.synchro,1,PF_WORD);
		PF_UnPack(&PF.numrbufs,1,PF_WORD);
		PF_UnPack(&PF.numsbufs,1,PF_WORD);
		PF_UnPack(&PF_maxinterms,1,PF_LONG);
		PF_UnPack(&fpsize,1,PF_INT);
		AM.Path = (UBYTE*)Malloc1(fpsize*sizeof(UBYTE),"Path");
		PF_UnPack(AM.Path,(LONG)fpsize,PF_BYTE);
		if ( PF.log ) {
			fprintf(stderr,"[%d] log=%d rbufs=%d sbufs=%d maxin=%ld path=%s\n",
					PF.me,PF.log,PF.numrbufs,PF.numsbufs,PF_maxinterms,AM.Path);
			fflush(stderr);
		}
	}
/*
  	#] BroadCast settings from getenv: 
*/
	return(0);
}
/*
 		#] PF_Init : 
  	#] startup : 
  	#[ PF_BroadcastNumberOfTerms :
*/

/**
 * Broadcasts a LONG value from the master to the all slaves.
 *
 * It is used to broadcast the number of terms in an expression for
 * preprocessor if-expression 'termsin', see pre.c. The procedure assumes that
 * \a x is the value obtained by the master and simply broadcasts it to all slaves.
 *
 * @param  x  the number to be broadcasted (set on the master).
 * @return    the broadcasted number.
 */
LONG PF_BroadcastNumberOfTerms(LONG x)
{
/*
		Note, compilation is performed INDEPENDENTLY on AC.mparallelflag!
		No if(AC.mparallelflag==PARALLELFLAG) !!
*/
	if ( MASTER == PF.me ) {        /* Pack the value of x */
		if ( PF_BroadCast(0) != 0 ) /* initialize buffers */
			Terminate(-1);
		if ( PF_Pack(&x,1,PF_LONG) != 0 ) Terminate(-1);
	}

	PF_BroadCast(1);  /*Broadcasting - no buffer initilisation for slaves! */

	if ( MASTER != PF.me ) {
/*
			Slave - unpack received x
			For slaves buffers are initialised automatically.
*/
		if ( PF_UnPack(&x,1,PF_LONG) != 0 ) Terminate(-1);
	}
	return (x);
}

/*
  	#] PF_BroadcastNumberOfTerms : 
  	#[ PF_InitRedefinedPreVars :
*/

/**
 * Broadcasts preprocessor variables, which were changed (by Redefine statement)
 * in the previous module, from the master to the all slaves.
 *
 * @return  0 if OK, nonzero on error.
 *
 * @remark  Limited by the size of the pack buffer.
 */
int PF_InitRedefinedPreVars(void)
{
/*
		Note, compilation is performed INDEPENDENTLY on AC.mparallelflag!
		No if(AC.mparallelflag==PARALLELFLAG) !!
*/
	UBYTE *value, *name, *p;
	int i, l;

	if ( MASTER == PF.me ) { /* Pack information about redefined PreVars */
		PF_BroadCast(0);     /* Initialize buffers */
		PF_Pack(&(PF.mnumredefs),1,PF_INT); /* Pack number of redefined variables:*/
/*
			now pack for each of the changed preprovariables the length of the
			name, the name, the length of the value and the value into the
			sendbuffer:
*/
		if ( 0 < PF.mnumredefs ) {
			for ( i = 0; i < NumPre; i++ ) {
				if ( PF.redef[i] ) {
					l = 1;
					p = name = PreVar[i].name;
					while ( *p++ ) l++;

					PF_Pack(&l,1,PF_INT);
					PF_Pack(name,l,PF_BYTE);
					l = 1;
					value = PreVar[i].value;
					while ( *p++ ) l++;

					PF_Pack(&l,1,PF_INT);
					PF_Pack(value,l,PF_BYTE);
				}
			}
		}
	}

	PF_BroadCast(1);

	if ( MASTER != PF.me ) { /*Unpack information about redefined PreVars*/
		int l, nl = 0, vl = 0;
/*
			Extract number of redefined variables:
*/
		PF_UnPack(&(PF.mnumredefs),1,PF_INT);
/*
			Initialize name and values by empty strings:
*/
		*( name = (UBYTE*)Malloc1(1,"PreVar name") ) = '\0';
		*( value = (UBYTE*)Malloc1(1,"PreVar value") ) = '\0';

		for ( i = 0; i < PF.mnumredefs; i++ ) {
/*
				extract name:
*/
			PF_UnPack(&l,1,PF_INT); /* Extract the name length */
			if ( l > nl ) {         /* Expand the buffer: */
				M_free(name,"PreVar name");
				name = (UBYTE*)Malloc1((int)l,"PreVar name");
				nl = l;
			}
/*
				extract the value of the name:
*/
			PF_UnPack(name,l,PF_BYTE); /* l >= 1 */
/*
				extract value:
*/
			PF_UnPack(&l,1,PF_INT);    /* Extract the value length */
			if ( l > vl ) {            /* Expand the buffer: */
				M_free(value,"PreVar val");
				value = (UBYTE*)Malloc1((int)l,"PreVar value");
				vl = l;
			}
/*
				extract the value of the value:
*/
			PF_UnPack(value,l,PF_BYTE);

			if ( PF.log ) {
				printf("[%d] module %ld: PutPreVar(\"%s\",\"%s\",1);\n",
						PF.me,AC.CModule,name,value);
			}
/*
				Re-define the variable:
*/
			PutPreVar(name,value,NULL,1);
/*
			mt: samebody made the following remark here:
			I reduced the possibility to transfer prepro variables
					with args for the moment
*/
		}
		M_free(name,"PreVar name");
		M_free(value,"PreVar value");
	}
	return (0);
}

/*
  	#] PF_InitRedefinedPreVars : 
  	#[ PF_BroadcastString :
*/

/**
 * Broadcasts a string from the master to all slaves.
 *
 * @param[in,out]  str  The pointer to a null-terminated string.
 * @return              0 if OK, nonzero on error.
 */
int PF_BroadcastString(UBYTE *str)
{
	int clength = 0;
/*
		If string does not fit to the PF_buffer, it
		will be split into chanks. Next chank is started at  str+clength
*/
		UBYTE *cstr=str;
/*
		Note, compilation is performed INDEPENDENTLY on AC.mparallelflag!
		No if ( AC.mparallelflag == PARALLELFLAG ) !!
*/
	do {
		cstr += clength; /*at each step for all slaves and master */

		if ( MASTER == PF.me ) { /*Pack str*/
/*
				initialize buffers
*/
			if ( PF_BroadCast(0) != 0 ) Terminate(-1);
			if ( ( clength = PF_PackString(cstr) ) <0  ) Terminate(-1);
		}
		PF_BroadCast(1);  /*Broadcasting - no buffer initilization for slaves!*/

		if ( MASTER != PF.me ) {
/*
				Slave - unpack received string
				For slaves buffers are initialised automatically.
*/
			if ( ( clength = PF_UnPackString(cstr) ) < 0 ) Terminate(-1);
		}
	} while ( cstr[clength-1] != '\0' );
	return (0);
}

/*
  	#] PF_BroadcastString : 
  	#[ PF_BroadcastPreDollar :
*/

/**
 * Broadcasts dollar variables set as a preprocessor variables.
 * Only the master is able to make an assignment like #$a=g; where g
 * is an expression: only the master has an access to the expression.
 * So, the master broadcasts the result to slaves.
 *
 * The result is in *dbuffer of the size is *newsize (in number of WORDs),
 * +1 for trailing zero. For slave newsize and numterms are output
 * parameters.
 *
 * @param[in,out]  dbuffer   the buffer for a dollar variable.
 * @param[in,out]  newsize   the size of the dollar variable in WORDs.
 * @param[in,out]  numterms  the number of terms in the dollar variable.
 * @return                   0 if OK, nonzero on error.
 */
int PF_BroadcastPreDollar(WORD **dbuffer, LONG *newsize, int *numterms)
{
	int err = 0;
	LONG i;
/*
		Note, compilation is performed INDEPENDENTLY on AC.mparallelflag!
		No if(AC.mparallelflag==PARALLELFLAG) !!
*/
	if ( MASTER == PF.me ) {
/*
			The problem is that sometimes dollar variables are longer
			than PF_packbuf! So we split long expression into chunks.
			There are n filled chunks and one portially filled chunk:
*/
		LONG n = ((*newsize)+1)/PF_maxDollarChunkSize;
/*
			...and one more chunk for the rest; if the expression fits to
			the buffer without splitting, the latter will be the only one.

			PF_maxDollarChunkSize is the maximal number of items fitted to
			the buffer. It is calculated in PF_LibInit() in mpi.c.
			PF_maxDollarChunkSize is calculated for the first step, when
			two fields (numterms and newsize, see below) are already packed.
			For simplicity, this value is used also for all steps, in
			despite  of it is	a bit less than maximally available space.
*/
		WORD *thechunk = *dbuffer;

		err = PF_BroadCast(0);             /* initialize buffers */
		err |= PF_Pack(numterms,1,PF_INT);
		err |= PF_Pack(newsize,1,PF_LONG); /* pack the size */
/*
			Pack and broadcast completely filled chunks.
			It may happen, this loop is not entered at all:
*/
		for ( i = 0; i < n; i++ ) {
			err |= PF_Pack(thechunk,PF_maxDollarChunkSize,PF_WORD);
			err |= PF_BroadCast(1);
			thechunk +=PF_maxDollarChunkSize;
			PF_BroadCast(0);
		}
/*
			Pack and broadcast the rest:
*/
		if ( ( n = ( (*newsize)+1)%PF_maxDollarChunkSize ) != 0 ) {
			err |= PF_Pack(thechunk,n,PF_WORD);
			err |= PF_BroadCast(1);
		}
	}
	if ( MASTER != PF.me ) {  /* Slave - unpack received buffer */
		WORD *thechunk;
		LONG n, therest, thesize;
		err |= PF_BroadCast(1);  /*Broadcasting - no buffer initilisation for slaves!*/
		err |=PF_UnPack(numterms,1,PF_INT);
		err |=PF_UnPack(newsize,1,PF_LONG);
/*
			Now we know the buffer size.
*/
		thesize = (*newsize)+1;
/*
			Evaluate the number of completely filled chunks. The last step must be
			treated separately, so -1:
*/
		n = (thesize/PF_maxDollarChunkSize) - 1;
/*
			Note, here n can be <0, this is ok.
*/
		therest = thesize % PF_maxDollarChunkSize;
		thechunk = *dbuffer =
			(WORD*)Malloc1( thesize * sizeof(WORD),"$-buffer slave");
		if ( thechunk == NULL ) return(err|4);
/*
			Unpack completely filled chunks and receive the next portion.
			It may happen, this loop is not entered at all:
*/
		for ( i = 0; i < n; i++ ) {
			err |= PF_UnPack(thechunk,PF_maxDollarChunkSize,PF_WORD);
			thechunk += PF_maxDollarChunkSize;
			err |= PF_BroadCast(1);
		}
/*
			Now the last completely filled chunk:
*/
		if ( n >= 0 ) {
			err |= PF_UnPack(thechunk,PF_maxDollarChunkSize,PF_WORD);
			thechunk += PF_maxDollarChunkSize;
			if ( therest != 0 ) err |= PF_BroadCast(1);
		}
/*
			Unpack the rest (it is already received!):
*/
		if ( therest != 0 ) err |= PF_UnPack(thechunk,therest,PF_WORD);
	}
	return (err);
}

/*
  	#] PF_BroadcastPreDollar : 
  	#[ PF_mkDollarsParallel :
*/

typedef struct {
	WORD **slavebuf;  /* array of slavebuffers for each dollar variable */
	WORD   type;      /* type of action on dollars: sum, maximum etc. */
	PADPOINTER(0,0,1,0);
} PFDOLLARS;

static PFDOLLARS *PFDollars = NULL;
/*
	Maximal number of PFDollars:
*/
static int MaxPFDollars = 0;

/*
 		#[ MinDollar :
*/

/**
 * Finds the minimum dollar variable among dollar variables
 * from different slaves and assigns the value obtained to
 * the dollar on the master.
 *
 * @param  index  the index of the dollar variable.
 * @return        0 if OK, nonzero on error.
 */
static int MinDollar(WORD index)
{
	int i;
	WORD *where, size, *r, *t;
	DOLLARS d;
	if ( PF.numtasks < 2 ) return -1;  /* Cannot be when AC.mparallelflag == PARALLELFLAG. */

	PFDollars[index].slavebuf[0] = PFDollars[index].slavebuf[1];
	for ( i = 2; i < PF.numtasks; i++ ){
		if ( TwoExprCompare(PFDollars[index].slavebuf[i], PFDollars[index].slavebuf[0], LESS) )
			PFDollars[index].slavebuf[0] = PFDollars[index].slavebuf[i];
	}

	where = NULL;
	size = 0;

	t = PFDollars[index].slavebuf[0];
	if ( t != NULL && t != &AM.dollarzero ) {
		r = t;
		while ( *r ) r += *r;
		size = r - t + 1;  /* Must be include the last zero. */
		where = (WORD *)Malloc1(size * sizeof(WORD), "dollar content");
		r = where;
		i = size;
		NCOPY(r, t, i);
	}

	d = Dollars + index;
	if ( d->where != NULL && d->where != &AM.dollarzero )
		M_free(d->where, "old content of dollar");
	d->where = where;
	if ( where == NULL || *where == 0 ) {
		d->type = DOLZERO;
		if ( where != NULL ) M_free(where, "buffer of dollar");
		d->where = &AM.dollarzero;
		d->size = 0;
	}
	else {
		d->type = DOLTERMS;
		d->size = size;
	}
	cbuf[AM.dbufnum].rhs[index] = d->where;

	return 0;
}

/*
 		#] MinDollar : 
 		#[ MaxDollar :
*/

/**
 * Finds the maximum dollar variable among dollar variables
 * from different slaves and assigns the value obtained to
 * the dollar on the master.
 *
 * @param  index  the index of the dollar variable.
 * @return        0 if OK, nonzero on error.
 */
static int MaxDollar(WORD index)
{
	int i;
	WORD *where, size, *r, *t;
	DOLLARS d;
	if ( PF.numtasks < 2 ) return -1;  /* Cannot be when AC.mparallelflag == PARALLELFLAG. */

	PFDollars[index].slavebuf[0] = PFDollars[index].slavebuf[1];
	for ( i = 2; i < PF.numtasks; i++ ) {
		if ( TwoExprCompare(PFDollars[index].slavebuf[i], PFDollars[index].slavebuf[0], GREATER) )
			PFDollars[index].slavebuf[0] = PFDollars[index].slavebuf[i];
	}

	where = NULL;
	size = 0;

	t = PFDollars[index].slavebuf[0];
	if (t != NULL && t != &AM.dollarzero ) {
		r = t;
		while ( *r ) r += *r;
		size = r - t + 1;  /* Must be include the last zero. */
		where = (WORD *)Malloc1(size * sizeof(WORD), "dollar content");
		r = where;
		i = size;
		NCOPY(r, t, i);
	}

	d = Dollars + index;
	if ( d->where != NULL && d->where != &AM.dollarzero )
		M_free(d->where, "old content of dollar");
	d->where = where;
	if ( where == NULL || *where == 0 ) {
		d->type = DOLZERO;
		if ( where != NULL ) M_free(where, "buffer of dollar");
		d->where = &AM.dollarzero;
		d->size = 0;
	}
	else {
		d->type = DOLTERMS;
		d->size = size;
	}
	cbuf[AM.dbufnum].rhs[index] = d->where;

	return 0;
}

/*
 		#] MaxDollar : 
 		#[ SumDollars :
*/

/**
 * Sums the dollar variable content in PFDollars[number].slavebuf
 * and assigns the result to the dollar variable with \a index.
 *
 * @param  index  the index of the dollar variable.
 * @return        0 if OK, nonzero on error.
 */
static int SumDollars(WORD index)
{
	GETIDENTITY
	int i, j, error = 0;
	WORD *dbuffer, *r, *m;
	DOLLARS d;

	CBUF *C = cbuf + AM.rbufnum;
	WORD *oldwork = AT.WorkPointer, *oldcterm = AN.cTerm;
	WORD olddefer = AR.DeferFlag, oldnumlhs = AR.Cnumlhs, oldnumrhs = C->numrhs;

	AN.cTerm = 0;
	AR.DeferFlag = 0;

	if ( NewSort(BHEAD0) || NewSort(BHEAD0) || NewSort(BHEAD0) ) {
		error = -1;
		goto cleanup;
	}

	for ( i = 1; i < PF.numtasks; i++ ) {
		r = PFDollars[index].slavebuf[i];
		if ( r == &AM.dollarzero ) continue;

		while ( *r ) {
			m = AT.WorkPointer;
			j = *r;

			while ( --j >= 0 ) *m++ = *r++;
			AT.WorkPointer = m;

			AR.Cnumlhs = 0;
			if ( Generator(BHEAD oldwork, 0) ) {
				LowerSortLevel(); LowerSortLevel(); LowerSortLevel();
				error = -1;
				goto cleanup;
			}

			AT.WorkPointer = oldwork;
		}
	}

	if ( EndSort(BHEAD (WORD *)&dbuffer, 2, 0) < 0 ) {
		LowerSortLevel(); LowerSortLevel();
		error = 1;
	}

	LowerSortLevel(); LowerSortLevel();

	d = Dollars + index;
	if ( d->where != NULL && d->where != &AM.dollarzero )
		M_free(d->where, "old content of dollar");
	d->where = dbuffer;
	if ( dbuffer == NULL || *dbuffer == 0 ) {
		d->type = DOLZERO;
		if ( dbuffer != NULL ) M_free(dbuffer, "buffer of dollar");
		d->where = &AM.dollarzero;
		d->size = 0;
	}
	else {
		d->type = DOLTERMS;
		r = d->where; while ( *r ) r += *r;
		d->size = r - d->where;  /* +1? (TU 22 Jul 2011) */
	}
	cbuf[AM.dbufnum].rhs[index] = d->where;

cleanup:
	AR.Cnumlhs = oldnumlhs;
	C->numrhs = oldnumrhs;
	AR.DeferFlag = olddefer;
	AN.cTerm = oldcterm;
	AT.WorkPointer = oldwork;

	return error;
}

/*
 		#] SumDollars : 
*/

/**
 * Combines dollars from the various slaves
 * and broadcasts the result to all slaves.
 *
 * There are NumPotModdollars of dollars which could be changed.
 * They are in the array PotModdollars.
 *
 * The current module could be executed in parallel only if all
 * "changeable" dollars are listed in the array ModOptdollars which
 * is an array of objects of type MODOPTDOLLAR (there are
 * NumModOptdollars of them), otherwise the module was switched
 * to sequential mode.
 *
 * If the current module was executed in sequential mode, the master
 * just broadcasts all "changeable" dollars to all slaves.
 *
 * If the current module was executed in parallel mode, the master receives
 * dollars from slaves, combines them and broadcasts the result to all slaves.
 *
 * The pseudo-code is as follows:
@verbatim
if parallel then
  if Master then
    INITIALIZATION
    MASTER RECEIVING: receive potentially modified dollars from slaves
    COMBINING: combine received dollars
    CLEANUP
  else
    SLAVE SENDING: pack potentially modified dollars send dollars to the Master
  endif
endif
if Master then
  MASTER PACK: pack potentially modified dollars
endif
Broadcast
if Slave then
  For each dollar:
    SLAVE UNPACK: unpack broadcasted data
    SLAVE STORE: replace corresponding dollar by unpacked data
endif
@endverbatim
 *
 * @remark  This function can be invoked only if NumPotModdollars > 0 !!!
 *          since NumPotModdollars > 0, then NumDollars > 0.
 *
 * @return  0 if OK, -1 on error.
 */
WORD PF_mkDollarsParallel(void)
{
	int i, j, nSlave, src, index, namesize;
	UBYTE *name, *p;
	WORD type, *where, *r;
	LONG size;
	DOLLARS d;

	if ( AC.mparallelflag == PARALLELFLAG ) {
		if ( PF.me == 0 ) { /*Master*/
/*
			#[ INITIALIZATION :
				Data from slaves will be placed into an array PFDollars.
				It must be re-allocated, if it's length is not enough.
				Realloc PFDollars, if needed:
*/
			if ( MaxPFDollars < NumDollars ) {
/*
						First, free previous allocation:
*/
				for ( i = 1; i < MaxPFDollars; i++ )
					M_free(PFDollars[i].slavebuf, "pointer to slave buffers");
				if ( PFDollars != NULL )
					M_free(PFDollars, "pointer to PFDOLLARS");
/*
						Allocate new one:
*/
				MaxPFDollars = NumDollars;
				PFDollars = (PFDOLLARS *)Malloc1(NumDollars*sizeof(PFDOLLARS),
															"pointer to PFDOLLARS");
/*
					and initialize it:
*/
				for ( i = 1; i < NumDollars; i++ ) {
					PFDollars[i].slavebuf = (WORD**)Malloc1(PF.numtasks*sizeof(WORD*),
									"pointer to array of slave buffers");
					for ( j = 0; j < PF.numtasks; j++ )
						PFDollars[i].slavebuf[j] = &(AM.dollarzero);
				}
			}
/*
			#] INITIALIZATION : 
			#[ MASTER RECEIVING :

				Get dollars from each of the slaves, unpack them and put
				data into PFDollars:
*/
			for ( nSlave = 1; nSlave < PF.numtasks; nSlave++ ) {
/*
					Master and slaves must initialize the "long" send buffer:
*/
				if ( PF_longSingleReset() ) return(-1);
/*
					PF_Receive(PF_ANY_SOURCE, PF_DOLLAR_MSGTAG, &src, &i);
*/
				if ( PF_longSingleReceive(PF_ANY_SOURCE, PF_DOLLAR_MSGTAG, &src, &i) )
					return(-1);
/*
					the last parameter (i) is always PF_DOLLAR_MSGTAG, ignored

					Now all the info is in PF_buffer.
					Here NumPotModdollars dollars totally available; we trust
					this number is the same on each slave:
*/
				for (i = 0; i < NumPotModdollars; i++) {
					PF_longSingleUnPack((UBYTE*)&namesize, 1, PF_INT);
					name = (UBYTE*)Malloc1(namesize, "dollar name");
					PF_longSingleUnPack(name, namesize, PF_BYTE);
					PF_longSingleUnPack((UBYTE*)&type, 1, PF_WORD);
					if (type != DOLZERO) {
						PF_longSingleUnPack((UBYTE*)&size, 1, PF_LONG);
						where = (WORD*)Malloc1(sizeof(WORD)*(size+1), "dollar content");
						PF_longSingleUnPack((UBYTE*)where, size+1, PF_WORD);
					}
					else {
						where = &(AM.dollarzero);
					}
/*
						Now we have the dollar name in "name", the dollar type in "type",
						the contents in "where" (of size "size").

						Find the dollar "index" (its order number):
*/
					index = GetDollar(name);
/*
						and find the corresponding index (j) of this dollar in the
						ModOptdollars array:
*/
					for ( j = 0; j < NumModOptdollars; j++ ) {
						if (ModOptdollars[j].number == index) break;
					}
/*
						In principle, if the dollar was not found in ModOptdollars,
						this means that it was not mentioned in the module option.
						At present, this is impossible since in such situation
						the module must be executed in the sequential mode.
*/
					if (j >= NumModOptdollars ) return(-1);

/*
						Now put data into PFDollars:

						The following type is NOT a dollar type, this is the
						module option type:
*/
					PFDollars[index].type = ModOptdollars[j].type;
/*
						Note the dollar type (from "type") is not used :O
*/
					PFDollars[index].slavebuf[src] = where;
/*
						Static buffer instead of name!!:
*/
					if ( name ) M_free(name, "dollar name");
				}
			}
/*
				Now all (raw) info from slaves is in PFDollars

			#] MASTER RECEIVING : 
			#[ COMBINING :
*/
			for ( i = 0; i < NumPotModdollars; i++ ) {
/*
					New dollar for the Master is created in the
					corresponding function similar to case MODLOCAL
*/
				switch (PFDollars[index=PotModdollars[i]].type) {
					case MODSUM:  /*  result must be summed up  */
						if(SumDollars(index)) MesPrint("error in SumDollars");
						break;
					case MODMAX:  /*  result must be a maximum  */
						if(MaxDollar(index)) MesPrint("error in MaxDollar");
						break;
					case MODMIN:  /*  result must be a minimum  */
						if(MinDollar(index)) MesPrint("error in MinDollar");
						break;
					case MODLOCAL:/*  no change  */
						continue;
					default:
						MesPrint("Serious internal error with module option");
						Terminate(-1);
				}
				/* According to the FORM manual, the results for MODMAX, MODMIN and MODSUM are numbers,
				   above implementation can give somewhat different results though. We clear all factors. */
				CleanDollarFactors(Dollars + index);
			}
/*
			#] COMBINING : 
			#[ CLEANUP :
*/
			for ( i = 1; i < NumDollars; i++ ) {
/*
					Note, slavebuf[0] was not allocated! It is just a copy!
*/
				for ( j = 1; j < PF.numtasks; j++ ) {
					if ( PFDollars[i].slavebuf[j] != &(AM.dollarzero) ) {
						M_free(PFDollars[i].slavebuf[j], "slave buffer");
						PFDollars[i].slavebuf[j] = &(AM.dollarzero);
					}
				}
			}
/*
			#] CLEANUP : 
*/
		}
		else { /*Slave*/
/*
			#[ SLAVE SENDING :

				Master and slaves must initialize the "long" send buffer:
*/
			if ( PF_longSingleReset() ) return(-1);
			for ( i = 0; i < NumPotModdollars; i++ ) {
				index = PotModdollars[i];
				p = name  = AC.dollarnames->namebuffer+Dollars[index].name;
				namesize = 1;
				while (*p++) namesize++;
				d = Dollars + index;
				PF_longSinglePack((UBYTE*)&namesize, 1, PF_INT);
				PF_longSinglePack(name, namesize, PF_BYTE);
				if ( d->type != DOLZERO ) {
					PF_longSinglePack((UBYTE *)&d->type, 1, PF_WORD);
					PF_longSinglePack((UBYTE *)&d->size, 1, PF_LONG);
					PF_longSinglePack((UBYTE *)d->where, d->size + 1, PF_WORD);
				}
				else {
					type = DOLZERO;
					PF_longSinglePack((UBYTE*)&type, 1, PF_WORD);
				}
			}
			PF_longSingleSend(MASTER, PF_DOLLAR_MSGTAG);
/*
			#] SLAVE SENDING : 
*/
		}
	}
/*
		The Master must pack and broadcast independently on mparallelflag!

		Initialization is performed independently for the Master and slaves:
*/
	if ( PF_longMultiReset() ) return(-1);
/*
 		#[ MASTER PACK :
*/
	if ( PF.me == 0 ) {
/*
			See a few lines above
			Prepare PF_buffer:
				PF_BroadCast(0);
*/
		for ( i = 0; i < NumPotModdollars; i++ ) {
			index = PotModdollars[i];
			p = name = AC.dollarnames->namebuffer+Dollars[index].name;
			namesize = 1;
			while ( *p++ ) namesize++;
			d = Dollars + index;
			PF_longMultiPack((UBYTE*)&namesize, 1, sizeof(int),PF_INT);
			PF_longMultiPack(name, namesize, 1,PF_BYTE);
			if ( d->type != DOLZERO ) {
				PF_longMultiPack((UBYTE *)&d->type, 1, sizeof(WORD), PF_WORD);
				PF_longMultiPack((UBYTE *)&d->size, 1, sizeof(LONG), PF_LONG);
				PF_longMultiPack((UBYTE *)d->where, d->size + 1, sizeof(WORD), PF_WORD);
				/* ...and the factored stuff. */
				PF_longMultiPack((UBYTE *)&d->nfactors, 1, sizeof(WORD), PF_WORD);
				if ( d->nfactors > 1 ) {
					for ( j = 0; j < d->nfactors; j++ ) {
						FACDOLLAR *f = &d->factors[j];
						PF_longMultiPack((UBYTE*)&(f->type), 1, sizeof(WORD), PF_WORD);
						PF_longMultiPack((UBYTE*)&(f->size), 1, sizeof(LONG), PF_LONG);
						if ( f->size > 0 ) {
							PF_longMultiPack((UBYTE*)f->where, f->size+1, sizeof(WORD), PF_WORD);
						}
						else {
							PF_longMultiPack((UBYTE*)&(f->value), 1, sizeof(WORD), PF_WORD);
						}
					}
				}
			}
			else {
				type = DOLZERO;
				PF_longMultiPack((UBYTE*)&type, 1, sizeof(WORD),PF_WORD);
			}
		}
	}
/*
 		#] MASTER PACK : 

		old PF_BroadCast(1); replaced by:
*/
	if ( PF_longBroadcast() ) return(-1);

	if ( PF.me != 0 ) {
/*
			For each dollar:
*/
		for ( i = 0; i < NumPotModdollars; i++ ) {
/*
			#[ SLAVE UNPACK :
*/
			WORD nfactors = 0;
			FACDOLLAR *factors = NULL;

			PF_longMultiUnPack((UBYTE*)&namesize, 1, sizeof(int),PF_INT);
			name = (UBYTE*)Malloc1(namesize, "dollar name");
			PF_longMultiUnPack(name, namesize, 1,PF_BYTE);
			PF_longMultiUnPack((UBYTE*)&type, 1, sizeof(WORD),PF_WORD);
			if ( type != DOLZERO ) {
				PF_longMultiUnPack((UBYTE*)&size, 1, sizeof(LONG),PF_LONG);
				where = (WORD*)Malloc1(sizeof(WORD)*(size+1), "dollar content");
				PF_longMultiUnPack((UBYTE*)where, size+1, sizeof(WORD),PF_WORD);
				/* ...and the factored stuff. */
				PF_longMultiUnPack((UBYTE*)&nfactors, 1, sizeof(WORD), PF_WORD);
				if ( nfactors > 1 ) {
					factors = (FACDOLLAR *)Malloc1(sizeof(FACDOLLAR)*nfactors, "dollar factored stuff");
					for ( j = 0; j < nfactors; j++ ) {
						FACDOLLAR *f = &factors[j];
						PF_longMultiUnPack((UBYTE*)&(f->type), 1, sizeof(WORD), PF_WORD);
						PF_longMultiUnPack((UBYTE*)&(f->size), 1, sizeof(LONG), PF_LONG);
						if ( f->size > 0 ) {
							f->where = (WORD*)Malloc1(sizeof(WORD)*(f->size+1), "dollar factor content");
							PF_longMultiUnPack((UBYTE*)(f->where), f->size+1, sizeof(WORD), PF_WORD);
							f->value = 0;
						}
						else {
							f->where = NULL;
							PF_longMultiUnPack((UBYTE*)&(f->value), 1, sizeof(WORD), PF_WORD);
						}
					}
				}
			}
			else {
				where = &(AM.dollarzero);
			}
/*
			#] SLAVE UNPACK : 
			#[ SLAVE STORE :
*/
			index = GetDollar(name);
			d = Dollars + index;
			if (d->where && d->where != &(AM.dollarzero))
						M_free(d->where, "old content of dollar");
			CleanDollarFactors(d);
			d->type  = type;
			d->where = where;
			d->nfactors = nfactors;
			d->factors = factors;
			if ( type != DOLZERO ) {
/*
					Strange stuff... To be investigated.
					How could it be, that
					where == 0 || *where == 0 and type != DOLZERO?:
*/
				if (where == 0 || *where == 0) {
					d->type  = DOLZERO;
					if (where) M_free(where, "received dollar content");
					d->where = &(AM.dollarzero); d->size  = 0;
				}
				else {
					r = d->where; while (*r) r += *r;
					d->size = r - d->where;
				}
			}
			cbuf[AM.dbufnum].rhs[index] = d->where;
			if (name) M_free(name, "dollar name");
/*
			#] SLAVE STORE : 
*/
		}
	}
	return (0);
}

/*
  	#] PF_mkDollarsParallel : 
  	#[ Potentially modified dollar variables :
 		#[ Explanations :

	Usage of a dollar just in a preprocessor "#$a=..." is indistingueshable
	from the "real" run-time "$a=...", "do $a=..." or "factor $a".
	Dollars, marked as potentially modified in CoAssign in comexpr.c may
	be not of a "really" potentially modified type.
	This becomes clear in CatchDollar() in dollar.c, but we cannot just
	remove the mark: this dollarvar could appear somewhere eles in this
	module in a "right" context. So, we count references to this dollarvar
	in the "right" context, and decrement the counter in CatchDollar(). If at
	the end the counter is > 0, the dollarvar is really "potentially modified".

 		#] Explanations : 
 		#[ Variables :
*/

static int * PF_potModDolls = NULL;
static int PF_potModDollsTop = 0;
static int PF_potModDollsN = -1;
#define PDLSTDELTA 16

/*
 		#] Variables : 
 		#[ PF_statPotModDollar :
*/

/**
 * Increases/decreases a reference counter for a dollar variable.
 *
 * @param  dollarnum  the dollar variable number.
 * @param  valToAdd   the value to be added to the reference counter.
 */
void PF_statPotModDollar(int dollarnum, int valToAdd)
{
	if ( dollarnum >= PF_potModDollsTop ) {
/*
			increase the array
*/
		int i = PF_potModDollsTop;
		PF_potModDolls =
			realloc(PF_potModDolls,(PF_potModDollsTop+=PDLSTDELTA)*sizeof(int));
		if ( PF_potModDolls == NULL ) Terminate(-1);
		for ( ; i < PF_potModDollsTop; i++ ) PF_potModDolls[i] = 0;
	}
	PF_potModDolls[dollarnum] += valToAdd;
	if ( dollarnum > PF_potModDollsN) PF_potModDollsN = dollarnum;
}
/*
 		#] PF_statPotModDollar : 
 		#[ PF_markPotModDollars :
*/

/**
 * Reflects reference counters of dollar variables into AC.PotModDolList.
 */
void PF_markPotModDollars(void)
{
	int i;
	for ( i = 0; i <= PF_potModDollsN; i++ ) {
		if ( PF_potModDolls[i] > 0 ) {
			WORD *pmd = (WORD *)FromList(&AC.PotModDolList);
			*pmd = i;
		}
		PF_potModDolls[i] = 0;
	}
	PF_potModDollsN = -1;
}

/*
 		#] PF_markPotModDollars : 
  	#] Potentially modified dollar variables : 
  	#[ PF_SetScratch :
*/

/**
 * Same as SetScratch() except it always fills the buffer from the given position.
 *
 * @param  f         the file handle.
 * @param  position  the position to be loaded into the buffer.
 */
static void PF_SetScratch(FILEHANDLE *f,POSITION *position)
{
	if(
			( f->handle >= 0) && ISGEPOS(*position,f->POposition) &&
			( ISGEPOSINC(*position,f->POposition,(f->POfull-f->PObuffer)*sizeof(WORD)) ==0 )
		)/*position is inside the buffer! SetScratch() will do nothing.*/
			f->POfull=f->PObuffer;/*force SetScratch() to re-read the position from the beginning:*/
	SetScratch(f,position);
}

/*
  	#] PF_SetScratch : 
  	#[ PF_pushScratch :
*/

/**
 * Flushes a scratch file.
 *
 * @param  f  the scratch file to be flushed.
 * @return    0 if OK, nonzero on error.
 */
static int PF_pushScratch(FILEHANDLE *f)
{
	LONG size,RetCode;
	if ( f->handle < 0){
		/*Create the file*/
		if ( ( RetCode = CreateFile(f->name) ) >= 0 ) {
			f->handle = (WORD)RetCode;
			PUTZERO(f->filesize);
			PUTZERO(f->POposition);
		}
		else{
			MesPrint("Cannot create scratch file %s",f->name);
			return(-1);
		}
	}/*if ( f->handle < 0)*/
	size = (f->POfill-f->PObuffer)*sizeof(WORD);
	if( size > 0 ){
		SeekFile(f->handle,&(f->POposition),SEEK_SET);
		if ( WriteFile(f->handle,(UBYTE *)(f->PObuffer),size) != size ){
			MesPrint("Error while writing to disk. Disk full?");
			return(-1);
		}
		ADDPOS(f->filesize,size);
		ADDPOS(f->POposition,size);
		f->POfill = f->POfull=f->PObuffer;
	}/*if( size > 0 )*/
	return(0);
}

/*
  	#] PF_pushScratch : 
  	#[ PF_WalkThroughExprSlave :
	Returns <=0 if the expression is ready, or dl+1;
*/

static int PF_WalkThroughExprSlave(FILEHANDLE *curfile, EXPRESSIONS e, int dl)
{
	LONG l=0;
	for(;;){
		if(curfile->POstop-curfile->POfill < dl){
			if(PF_pushScratch(curfile))
				return(-PF.exprbufsize-1);
		}
		curfile->POfill+=dl;
		curfile->POfull=curfile->POfill;
		l+=dl;
		if( l >= PF.exprbufsize){
			if( l == PF.exprbufsize){
				if( *(curfile->POfill) == 0)/*expression is ready*/
					return(0);
				}
			l-=PF.exprbufsize;
			curfile->POfill-=l;
			curfile->POfull=curfile->POfill;
			return l+1;
		}

		dl=*(curfile->POfill);
		if(dl == 0)
			return l-PF.exprbufsize;
		(e->counter)++;
		if(dl<0){/*compressed term*/
			if(curfile->POstop-curfile->POfill < 1){
				if(PF_pushScratch(curfile))
					return(-PF.exprbufsize-1);
			}
			dl=*(curfile->POfill+1)+2;
		}/*if(*(curfile->POfill)<0)*/
	}/*for(;;)*/
}

/*
  	#] PF_WalkThroughExprSlave : 
  	#[ PF_WalkThroughExprMaster :
	Returns <=0 if the expression is ready, or dl+1;
*/

static int PF_WalkThroughExprMaster(FILEHANDLE *curfile, int dl)
{
	LONG l=0;
	for(;;){
		if(curfile->POfull-curfile->POfill < dl){
			POSITION pos;
			SeekScratch(curfile,&pos);
			PF_SetScratch(curfile,&pos);
		}/*if(curfile->POfull-curfile->POfill < dl)*/
		curfile->POfill+=dl;
		l+=dl;
		if( l >= PF.exprbufsize){
			if( l == PF.exprbufsize){
				if( *(curfile->POfill) == 0)/*expression is ready*/
					return(0);
				}
			l-=PF.exprbufsize;
			curfile->POfill-=l;
			return l+1;
		}

		dl=*(curfile->POfill);
		if(dl == 0)
			return l-PF.exprbufsize;

		if(dl<0){/*compressed term*/
			if(curfile->POfull-curfile->POfill < 1){
				POSITION pos;
				SeekScratch(curfile,&pos);
				PF_SetScratch(curfile,&pos);
			}/*if(curfile->POfull-curfile->POfill < 1)*/
			dl=*(curfile->POfill+1)+2;
		}/*if(*(curfile->POfill)<0)*/
	}/*for(;;)*/
}

/*
  	#] PF_WalkThroughExprMaster : 
  	#[ PF_rhsBCastMaster :
*/

/**
 * On the master, broadcasts an expression to the all slaves.
 *
 * @param  curfile  the scratch file in which the expression is stored.
 * @param  e        the expression to be broadcasted.
 * @return          0 if OK, nonzero on error.
 */
static int PF_rhsBCastMaster(FILEHANDLE *curfile, EXPRESSIONS e)
{
	LONG l=1;/*PF_WalkThroughExpr returns length + 1*/
	SetScratch(curfile,&(e->onfile));
	do{
		if( curfile->POfull-curfile->POfill < PF.exprbufsize ){
			POSITION pos;
			SeekScratch(curfile,&pos);
			PF_SetScratch(curfile,&pos);
		}/*if( curfile->POfull-curfile->POfill < PF.exprbufsize )*/
		if ( PF_Bcast(curfile->POfill,PF.exprbufsize*sizeof(WORD)))
			return -1;
		l=PF_WalkThroughExprMaster(curfile,l-1);
	}while(l>0);
	if(l<0)/*The tail is extra, decrease POfill*/
		curfile->POfill-=l;
	return(0);
}

/*
  	#] PF_rhsBCastMaster : 
  	#[ PF_rhsBCastSlave :
*/

/**
 * On the slave, recieves an expression broadcasted from the master.
 *
 * @param  curfile  the scratch file to store the broadcasted expression
 *                  (AR.infile or AR.hidefile).
 * @param  e        the expression to be broadcasted.
 * @return          0 if OK, nonzero on error.
 */
static int PF_rhsBCastSlave(FILEHANDLE *curfile, EXPRESSIONS e)
{
	LONG l=1;/*PF_WalkThroughExpr returns length + 1*/
	e->counter=0;
	do{
		if( curfile->POstop-curfile->POfill < PF.exprbufsize ){
			if(PF_pushScratch(curfile))
				return(-1);
		}/*if( curfile->POstop-curfile->POfill < PF.exprbufsize )*/
		if ( PF_Bcast(curfile->POfill,PF.exprbufsize*sizeof(WORD)))
			return(-1);
		l=PF_WalkThroughExprSlave(curfile,e,l-1);
	}while(l>0);
	if(l<0){/*The tail is extra, decrease POfill*/
		if(l<-PF.exprbufsize)/*error due to a PF_pushScratch() failure */
			return(-1);
		curfile->POfill-=l;
	}
	curfile->POfull=curfile->POfill;
	if ( curfile != AR.hidefile ) AR.InInBuf = curfile->POfull-curfile->PObuffer;
	else                          AR.InHiBuf = curfile->POfull-curfile->PObuffer;
	return(0);
}

/*
  	#] PF_rhsBCastSlave : 
  	#[ PF_broadcastRHS :
*/

/**
 * Broadcasts expressions in the right-hand side from the master to the all slaves.
 *
 * @return  0 if OK, nonzero on error.
 */
int PF_broadcastRHS(void)
{
	int i;
	FILEHANDLE *curfile = 0;

	for ( i = 0; i < NumExpressions; i++ ) {
		EXPRESSIONS e = Expressions+i;
		if ( ( e->vflags & ISINRHS ) == 0 ) continue;
		switch ( e->status ) {
			case LOCALEXPRESSION:
			case SKIPLEXPRESSION:
			case DROPLEXPRESSION:
			case GLOBALEXPRESSION:
			case SKIPGEXPRESSION:
			case DROPGEXPRESSION:
			case HIDELEXPRESSION:
			case HIDEGEXPRESSION:
			case INTOHIDELEXPRESSION:
			case INTOHIDEGEXPRESSION:
				AR.GetFile = 0;
				curfile = AR.infile;
				break;
			case HIDDENLEXPRESSION:
			case HIDDENGEXPRESSION:
			case DROPHLEXPRESSION:
			case DROPHGEXPRESSION:
			case UNHIDELEXPRESSION:
			case UNHIDEGEXPRESSION:
				AR.GetFile = 2;
				curfile = AR.hidefile;
				break;
		}/*switch ( e->status )*/

		if ( PF.me != MASTER ){
			POSITION pos;
			SetEndHScratch(curfile,&pos);
			e->onfile = pos;
			if ( PF_rhsBCastSlave(curfile,e) ) return(-1);
		}
		else {
			if ( PF_rhsBCastMaster(curfile,e) ) return(-1);
		}
	}/*for ( i = 0; i < NumExpressions; i++ )*/
	if ( PF.me != MASTER )
		UpdatePositions();
	return(0);
}

/*
  	#] PF_broadcastRHS : 
  	#[ PF_InParallelProcessor :
*/

/**
 * Processes expressions in the InParallel mode, i.e.,
 * dividing expressions marked by partodo over the slaves.
 *
 * @return  0 if OK, nonzero on error.
 */
int PF_InParallelProcessor(void)
{
	GETIDENTITY
	int i, next,tag;
	EXPRESSIONS e;
	if(PF.me == MASTER){
		if ( PF.numtasks >= 3 ) {
			partodoexr = (WORD*)Malloc1(sizeof(WORD)*(PF.numtasks+1),"PF_InParallelProcessor");
			for ( i = 0; i < NumExpressions; i++ ) {
				e = Expressions+i;
				if ( e->partodo <= 0 ) continue;
				if ( e->counter == 0 ) { /* Expression with zero terms */
					e->partodo = 0;
					continue;
				}
				switch(e->status){
					case LOCALEXPRESSION:
					case GLOBALEXPRESSION:
					case UNHIDELEXPRESSION:
					case UNHIDEGEXPRESSION:
					case INTOHIDELEXPRESSION:
					case INTOHIDEGEXPRESSION:
						tag=PF_ANY_SOURCE;
						next=PF_Wait4SlaveIP(&tag);
						if(next<0)
							return(-1);
						if(tag == PF_DATA_MSGTAG){
							PF_Statistics(PF_stats,0);
							if(PF_Slave2MasterIP(next))
								return(-1);
						}
						if(PF_Master2SlaveIP(next,e))
							return(-1);
						partodoexr[next]=i;
						break;
					default:
						e->partodo = 0;
						continue;
				}/*switch(e->status)*/
			}/*for ( i = 0; i < NumExpressions; i++ )*/
			/*Here some slaves are working, other are waiting on PF_Send.
				Wait all of them.*/
			/*At this point no new slaves may be launched so PF_WaitAllSlaves()
				does not modify partodoexr[].*/
			if(PF_WaitAllSlaves())
				return(-1);
			/**/
			if ( AC.CollectFun ) AR.DeferFlag = 0;
			if(partodoexr){
				M_free(partodoexr,"PF_InParallelProcessor");
				partodoexr=NULL;
			}/*if(partodoexr)*/
		}/*if ( PF.numtasks >= 3 ) */
		else {
			for ( i = 0; i < NumExpressions; i++ ) {
				Expressions[i].partodo = 0;
			}
		}
		return(0);
	}/*if(PF.me == MASTER)*/
	/*Slave:*/
	if(PF_Wait4MasterIP(PF_EMPTY_MSGTAG))
		return(-1);
	/*master is ready to listen to me*/
	do{
		WORD *oldwork= AT.WorkPointer;
		tag=PF_ReadMaster();/*reads directly to its scratch!*/
		if(tag<0)
			return(-1);
		if(tag == PF_DATA_MSGTAG){
			oldwork = AT.WorkPointer;
			if(PF_DoOneExpr())/*the processor*/
				return(-1);
			if(PF_Wait4MasterIP(PF_DATA_MSGTAG))
				return(-1);
			if(PF_Slave2MasterIP(PF.me))/*both master and slave*/
				return(-1);
			AT.WorkPointer=oldwork;
		}/*if(tag == PF_DATA_MSGTAG)*/
	}while(tag!=PF_EMPTY_MSGTAG);
	PF.exprtodo=-1;
	return(0);
}/*PF_InParallelProcessor*/

/*
  	#] PF_InParallelProcessor : 
  	#[ PF_Wait4MasterIP :
*/

static int PF_Wait4MasterIP(int tag)
{
	int follow = 0;
	LONG cpu,space = 0;

	if(PF.log){
		fprintf(stderr,"[%d] Starting to send to Master\n",PF.me);
		fflush(stderr);
	}

	PF_Send(MASTER,tag,0);
	cpu = TimeCPU(1);
	PF_Pack(&cpu               ,1,PF_LONG);
	PF_Pack(&space             ,1,PF_LONG);
	PF_Pack(&PF_linterms       ,1,PF_LONG);
	PF_Pack(&(AM.S0->GenTerms) ,1,PF_LONG);
	PF_Pack(&(AM.S0->TermsLeft),1,PF_LONG);
	PF_Pack(&follow            ,1,PF_INT );

	if(PF.log){
		fprintf(stderr,"[%d] Now sending with tag = %d\n",PF.me,tag);
		fflush(stderr);
	}

	PF_Send(MASTER,tag,1);

	if(PF.log){
		fprintf(stderr,"[%d] returning from send\n",PF.me);
		fflush(stderr);
	}
	return(0);
}
/*
  	#] PF_Wait4MasterIP : 
  	#[ PF_DoOneExpr :
*/

/**
 * Processes an expression specified by PF.exprtodo.
 *
 * See also "case DOONEEXPRESSION" in RunThread().
 *
 * @return        0 if OK, nonzero on error.
 */
static int PF_DoOneExpr(void)/*the processor*/
{
				GETIDENTITY
				EXPRESSIONS e = Expressions + PF.exprtodo;
				POSITION position, outposition;
				FILEHANDLE *fi, *fout;
				LONG dd = 0;
				int i;
				WORD *term;

				i = PF.exprtodo;
				AR.CurExpr = i;
				AR.SortType = AC.SortType;

				position = AS.OldOnFile[i];
				if ( e->status == HIDDENLEXPRESSION || e->status == HIDDENGEXPRESSION ) {
					AR.GetFile = 2; fi = AR.hidefile;
				}
				else {
					AR.GetFile = 0; fi = AR.infile;
				}
				SetScratch(fi,&position);
				term = AT.WorkPointer;
				if ( GetTerm(BHEAD term) <= 0 ) {
					MesPrint("Expression %d has problems in scratchfile",i);
					Terminate(-1);
				}
				if ( AC.bracketindexflag > 0 ) OpenBracketIndex(i);
				term[3] = i;
				PUTZERO(outposition);
				fout = AR.outfile;
				fout->POfill = fout->POfull = fout->PObuffer;
				fout->POposition = outposition;
				if ( fout->handle >= 0 ) {
					fout->POposition = outposition;
				}
				if ( PutOut(BHEAD term,&outposition,fout,0) < 0 )
					return(-1);
				AR.DeferFlag = AC.ComDefer;

/*				AR.sLevel = AB[0]->R.sLevel;*/
				term = AT.WorkPointer;
				NewSort(BHEAD0);
				AR.MaxDum = AM.IndDum;
				AN.ninterms = 0;
				while ( GetTerm(BHEAD term) ) {
				  SeekScratch(fi,&position);
				  AN.ninterms++; dd = AN.deferskipped;
				  if ( AC.CollectFun && *term <= (LONG)(AM.MaxTer/(2*sizeof(WORD))) ) {
					if ( GetMoreTerms(term) < 0 ) {
					  LowerSortLevel(); return(-1);
					}
				    SeekScratch(fi,&position);
				  }
				  AT.WorkPointer = term + *term;
				  AN.RepPoint = AT.RepCount + 1;
				  AR.CurDum = ReNumber(BHEAD term);
				  if ( AC.SymChangeFlag ) MarkDirty(term,DIRTYSYMFLAG);
				  if ( AN.ncmod ) {
					if ( ( AC.modmode & ALSOFUNARGS ) != 0 ) MarkDirty(term,DIRTYFLAG);
					else if ( AR.PolyFun ) PolyFunDirty(BHEAD term);
				  }
				  if ( ( AR.PolyFunType == 2 ) && ( AC.PolyRatFunChanged == 0 )
					  && ( e->status == LOCALEXPRESSION || e->status == GLOBALEXPRESSION ) ) {
					  PolyFunClean(BHEAD term);
				  }
				  if ( Generator(BHEAD term,0) ) {
					LowerSortLevel(); return(-1);
				  }
				  AN.ninterms += dd;
				  SetScratch(fi,&position);
				  if ( fi == AR.hidefile ) {
					AR.InHiBuf = (fi->POfull-fi->PObuffer)
						-DIFBASE(position,fi->POposition)/sizeof(WORD);
				  }
				  else {
					AR.InInBuf = (fi->POfull-fi->PObuffer)
						-DIFBASE(position,fi->POposition)/sizeof(WORD);
				  }
				}
				AN.ninterms += dd;
				if ( EndSort(BHEAD AM.S0->sBuffer,0,0) < 0 ) return(-1);
				e->numdummies = AR.MaxDum - AM.IndDum;
				if ( AM.S0->TermsLeft )   e->vflags &= ~ISZERO;
				else                      e->vflags |= ISZERO;
				if ( AR.expchanged == 0 ) e->vflags |= ISUNMODIFIED;
				if ( AM.S0->TermsLeft ) AR.expflags |= ISZERO;
				if ( AR.expchanged )    AR.expflags |= ISUNMODIFIED;
				AR.GetFile = 0;
				fout->POfull = fout->POfill;
	return(0);
}

/*
  	#] PF_DoOneExpr : 
  	#[ PF_Slave2MasterIP :
*/

typedef struct bufIPstruct {
	LONG i;
	struct ExPrEsSiOn e;
} bufIPstruct_t;

static int PF_Slave2MasterIP(int src)/*both master and slave*/
{
	EXPRESSIONS e;
	bufIPstruct_t exprData;
	int i,l;
	FILEHANDLE *fout=AR.outfile;
	POSITION pos;
	/*Here we know the length of data to send in advance:
		slave has the only one expression in its scratch file, and it sends
		this information to the master.*/
	if(PF.me != MASTER){/*slave*/
		e = Expressions + PF.exprtodo;
		/*Fill in the expression data:*/
		memcpy(&(exprData.e), e, sizeof(struct ExPrEsSiOn));
		SeekScratch(fout,&pos);
		exprData.i=BASEPOSITION(pos);
		/*Send the metadata:*/
		if(PF_RawSend(MASTER,&exprData,sizeof(bufIPstruct_t),0))
			return(-1);
		i=exprData.i;
		SETBASEPOSITION(pos,0);
		do{
			int blen=PF.exprbufsize*sizeof(WORD);
			if(i<blen)
				blen=i;
			l=PF_SendChunkIP(fout,&pos, MASTER, blen);
			/*Here always l == blen!*/
			if(l<0)
				return(-1);
			ADDPOS(pos,l);
			i-=l;
		}while(i>0);
		if ( fout->handle >= 0 ) { /* Now get rid of the file */
			CloseFile(fout->handle);
			fout->handle = -1;
			remove(fout->name);
			PUTZERO(fout->POposition);
			PUTZERO(fout->filesize);
			fout->POfill = fout->POfull = fout->PObuffer;
		}
		return(0);
	}/*if(PF.me != MASTER)*/
	/*Master*/
	/*partodoexr[src] is the number of expression.*/
	e = Expressions +partodoexr[src];
	/*Get metadata:*/
	if (PF_RawRecv(&src, &exprData,sizeof(bufIPstruct_t),&i)!= sizeof(bufIPstruct_t))
		return(-1);
	/*Fill in the expression data:*/
	memcpy(e, &(exprData.e), sizeof(struct ExPrEsSiOn));
	SeekScratch(fout,&pos);
	e->onfile = pos;
	i=exprData.i;
	while(i>0){
		int blen=PF.exprbufsize*sizeof(WORD);
		if(i<blen)
			blen=i;
		l=PF_RecvChunkIP(fout,src,blen);
		/*Here always l == blen!*/
		if(l<0)
			return(-1);
		i-=l;
	}
	return(0);
}

/*
  	#] PF_Slave2MasterIP : 
  	#[ PF_Master2SlaveIP :
*/

static int PF_Master2SlaveIP(int dest, EXPRESSIONS e)
{
	bufIPstruct_t exprData;
	FILEHANDLE *fi;
	POSITION pos;
	int l;
	LONG ll=0,count=0;
	WORD *t;
	if(e==NULL){/*Say to the slave that no more job:*/
		if(PF_RawSend(dest,&exprData,sizeof(bufIPstruct_t),PF_EMPTY_MSGTAG))
			return(-1);
		return(0);
	}
	memcpy(&(exprData.e), e, sizeof(struct ExPrEsSiOn));
	exprData.i=e-Expressions;
	if ( AC.StatsFlag && AC.OldParallelStats ) {
		MesPrint("");
		MesPrint(" Sending expression %s to slave %d",EXPRNAME(exprData.i),dest);
	}
	if(PF_RawSend(dest,&exprData,sizeof(bufIPstruct_t),PF_DATA_MSGTAG))
		return(-1);
	if ( e->status == HIDDENLEXPRESSION || e->status == HIDDENGEXPRESSION )
		fi = AR.hidefile;
	else
		fi = AR.infile;
	pos=e->onfile;
	SetScratch(fi,&pos);
	do{
		l=PF_SendChunkIP(fi, &pos, dest, PF.exprbufsize*sizeof(WORD));
		if(l<0)
			return(-1);
		t=fi->PObuffer+ (DIFBASE(pos,fi->POposition))/sizeof(WORD);
		ll=PF_WalkThrough(t,ll,l/sizeof(WORD),&count);
		ADDPOS(pos,l);
	}while(ll>-2);
	return(0);
}

/*
  	#] PF_Master2SlaveIP : 
  	#[ PF_ReadMaster :
*/

static int PF_ReadMaster(void)/*reads directly to its scratch!*/
{
	bufIPstruct_t exprData;
	int tag,m=MASTER;
	EXPRESSIONS e;
	FILEHANDLE *fi;
	POSITION pos;
	LONG count=0;
	WORD *t;
	LONG ll=0;
	int l;
	/*Get metadata:*/
	if (PF_RawRecv(&m, &exprData,sizeof(bufIPstruct_t),&tag)!= sizeof(bufIPstruct_t))
		return(-1);

	if(tag == PF_EMPTY_MSGTAG)/*No data, no job*/
		return(tag);

	/*data expected, tag must be == PF_DATA_MSTAG!*/
	PF.exprtodo=exprData.i;
	e=Expressions + PF.exprtodo;
	/*Fill in the expression data:*/
	memcpy(e, &(exprData.e), sizeof(struct ExPrEsSiOn));
	if ( e->status == HIDDENLEXPRESSION || e->status == HIDDENGEXPRESSION )
		fi = AR.hidefile;
	else
		fi = AR.infile;
	SetEndHScratch(fi,&pos);
	e->onfile=AS.OldOnFile[PF.exprtodo]=pos;

	do{
		l=PF_RecvChunkIP(fi,MASTER,PF.exprbufsize*sizeof(WORD));
		if(l<0)
			return(-1);
		t=fi->POfull-l/sizeof(WORD);
		ll=PF_WalkThrough(t,ll,l/sizeof(WORD),&count);
	}while(ll>-2);
	/*Now -ll-2 is the number of "extra" elements transferred from the master.*/
	fi->POfull-=-ll-2;
	fi->POfill=fi->POfull;
	return(PF_DATA_MSGTAG);
}

/*
  	#] PF_ReadMaster : 
  	#[ PF_SendChunkIP :
	thesize is in bytes. Returns the number of sent bytes or <0 on error:
*/

static int PF_SendChunkIP(FILEHANDLE *curfile, POSITION *position, int to, LONG thesize)
{
	LONG l=thesize;
	if(
		ISLESSPOS(*position,curfile->POposition) ||
		ISGEPOSINC(*position,curfile->POposition,
		((curfile->POfull-curfile->PObuffer)*sizeof(WORD)-thesize) )
	){
		if(curfile->handle< 0)
			l=(curfile->POfull-curfile->PObuffer)*sizeof(WORD) - (LONG)(position->p1);
		else{
			PF_SetScratch(curfile,position);
			if(
				ISGEPOSINC(*position,curfile->POposition,
				((curfile->POfull-curfile->PObuffer)*sizeof(WORD)-thesize) )
				)
			l=(curfile->POfull-curfile->PObuffer)*sizeof(WORD) - (LONG)position->p1;
		}
	}
	/*Now we are able to sent l bytes from the
		curfile->PObuffer[position-curfile->POposition]*/
	if(PF_RawSend(to,curfile->PObuffer+ (DIFBASE(*position,curfile->POposition))/sizeof(WORD),l,0))
		return(-1);
	return(l);
}

/*
  	#] PF_SendChunkIP : 
  	#[ PF_RecvChunkIP :
	thesize is in bytes. Returns the number of sent bytes or <0 on error:
*/

static int PF_RecvChunkIP(FILEHANDLE *curfile, int from, LONG thesize)
{
	LONG receivedBytes;

	if( (LONG)((curfile->POstop - curfile->POfull)*sizeof(WORD)) < thesize )
		if(PF_pushScratch(curfile))
			return(-1);
	/*Now there is enough space from curfile->POfill to curfile->POstop*/
	{/*Block:*/
		int tag=0;
		receivedBytes=PF_RawRecv(&from,curfile->POfull,thesize,&tag);
	}/*:Block*/
	if(receivedBytes >= 0 ){
		curfile->POfull+=receivedBytes/sizeof(WORD);
		curfile->POfill=curfile->POfull;
	}/*if(receivedBytes >= 0 )*/
	return(receivedBytes);
}

/*
  	#] PF_RecvChunkIP : 
  	#[ PF_WalkThrough :
	Returns:
	>=  0 -- initial offset,
		-1 -- the first element of t contains the length of the tail of compressed term,
	<= -2 -- -(d+2), where d is the number of extra transferred elements.
	Expects:
	l -- initial offset or -1,
	chunk -- number of transferred elements (not bytes!)
	*count -- incremented each time a new term is found
*/

static int PF_WalkThrough(WORD *t, LONG l, LONG chunk, LONG *count)
{
	if(l<0) /*==-1!*/
		l=(*t)+1;/*the first element of t contains the length of
						the tail of compressed term*/
	else{
		if(l>=chunk)/*next term is out of the chunk*/
			return(l-chunk);
		t+=l;
		chunk-=l;/*note, l was less than chunk so chunk >0!*/
		l=*t;
	}
	/*Main loop:*/
	while(l!=0){
		if(l>0){/*an offset to the next term*/
			if(l<chunk){
				t+=l;
				chunk-=l;/*note, l was less than chunk so chunk >0!*/
				l=*t;
				(*count)++;
			}/*if(l<chunk)*/
			else
				return(l-chunk);
		}/*if(l>0)*/
		else{ /* l<0 */
			if(chunk < 2)/*i.e., chunk == 1*/
				return(-1);/*the first WORD in the next chunk is length of the tail of the compressed term*/
			l=*(t+1)+2;/*+2 since
					1. t points to the length field -1,
					2. the size of a tail of compressed term is equal to the number of WORDs in this tail*/
		}
	}/*while(l!=0)*/
	return(-1-chunk);/* -(2+(chunk-1)), chunk>0 ! */
}

/*
  	#] PF_WalkThrough : 
  	#[ PF_SendFile :
*/

#define PF_SNDFILEBUFSIZE 4096

/**
 * Sends a file to the process specified by \a to.
 *
 * @param  to  the destination process number.
 * @param  fd  the file to be sent.
 * @return     the size of sent data in bytes, or -1 on error.
 */
int PF_SendFile(int to, FILE *fd)
{
	size_t len=0;
	if(fd == NULL){
		if(PF_RawSend(to,&to,sizeof(int),PF_EMPTY_MSGTAG))
			return(-1);
		return(0);
	}
	for(;;){
		char buf[PF_SNDFILEBUFSIZE];
		size_t l;
		l=fread(buf, 1, PF_SNDFILEBUFSIZE, fd);
		len+=l;
		if(l==PF_SNDFILEBUFSIZE){
			if(PF_RawSend(to,buf,PF_SNDFILEBUFSIZE,PF_BUFFER_MSGTAG))
				return(-1);
		}
		else{
			if(PF_RawSend(to,buf,l,PF_ENDBUFFER_MSGTAG))
				return(-1);
			break;
		}
	}/*for(;;)*/
	return(len);
}

/*
  	#] PF_SendFile : 
  	#[ PF_RecvFile :
*/

/**
 * Receives a file from the process specified by \a from.
 *
 * @param  from  the source process number.
 * @param  fd    the file to save the recieved data.
 * @return       the size of received data in bytes, or -1 on error.
 */
int PF_RecvFile(int from, FILE *fd)
{
	size_t len=0;
	int tag;
	do{
		char buf[PF_SNDFILEBUFSIZE];
		int l;
			l=PF_RawRecv(&from,buf,PF_SNDFILEBUFSIZE,&tag);
			if(l<0)
				return(-1);
			if(tag == PF_EMPTY_MSGTAG)
				return(0);

			if( fwrite(buf,l,1,fd)!=1 )
				return(-1);
			len+=l;
	}while(tag!=PF_ENDBUFFER_MSGTAG);
	return(len);
}

/*
  	#] PF_RecvFile : 
  	#[ Synchronised output :
 		#[ Explanations :
*/

/*
 * If the master and slaves output statistics or error messages to the same stream
 * or file (e.g., the standard output or the log file) simultaneously, then
 * a mixing of their outputs can occur. To avoid this, TFORM uses a lock of
 * ErrorMessageLock, but there is no locking functionality in the original MPI
 * specification. We need to synchronise the output from the master and slaves.
 *
 * The idea of the synchronised output (by, e.g., MesPrint()) implemented here is
 *   Slaves:
 *     1. Save the output by WriteFile() (set to PF_WriteFileToFile())
 *        into some buffers between MLOCK(ErrorMessageLock) and
 *        MUNLOCK(ErrorMessageLock), which call PF_MLock() and PF_MUnlock(),
 *        respectively. The output for AM.StdOut and AC.LogHandle are saved to
 *        the buffers.
 *     2. At MUNLOCK(ErrorMessageLock), send the output in the buffer to the master,
 *        with PF_STDOUT_MSGTAG or PF_LOG_MSGTAG.
 *   Master:
 *     1. Recieve the buffered output from slaves, and write them by
 *        WriteFileToFile().
 *   The main problem is how and where the master receives messages from
 *   the slaves (PF_ReceiveErrorMessage()). For this purpose there are three
 *   helper functions: PF_CatchErrorMessages() and PF_CatchErrorMessagesForAll()
 *   which remove messages with PF_STDOUT_MSGTAG or PF_LOG_MSGTAG from the top
 *   of the message queue, and PF_ProbeWithCatchingErrorMessages() which is same as
 *   PF_Probe() except removing these messages.
 */

/*
 		#] Explanations : 
 		#[ Variables :
*/

static int errorMessageLock = 0;     /* (slaves) The lock count. See PF_MLock() and PF_MUnlock(). */
static Vector(UBYTE, stdoutBuffer);  /* (slaves) The buffer for AM.StdOut. */
static Vector(UBYTE, logBuffer);     /* (slaves) The buffer for AC.LogHandle. */
#define recvBuffer logBuffer         /* (master) The buffer for receiving messages. */

/*
 * If PF_ENABLE_STDOUT_BUFFERING is defined, the master performs the line buffering
 * (using stdoutBuffer) at PF_WriteFileToFile().
 */
#ifndef PF_ENABLE_STDOUT_BUFFERING
#ifdef UNIX
#define PF_ENABLE_STDOUT_BUFFERING
#endif
#endif

/*
 		#] Variables : 
 		#[ PF_MLock :
*/

/**
 * A function called by MLOCK(ErrorMessageLock) for slaves.
 */
void PF_MLock(void)
{
	/* Only on slaves. */
	if ( errorMessageLock++ > 0 ) return;
	VectorClear(stdoutBuffer);
	VectorClear(logBuffer);
}

/*
 		#] PF_MLock : 
 		#[ PF_MUnlock :
*/

/**
 * A function called by MUNLOCK(ErrorMessageLock) for slaves.
 */
void PF_MUnlock(void)
{
	/* Only on slaves. */
	if ( --errorMessageLock > 0 ) return;
	if ( !VectorEmpty(stdoutBuffer) ) {
		PF_RawSend(MASTER, VectorPtr(stdoutBuffer), VectorSize(stdoutBuffer), PF_STDOUT_MSGTAG);
	}
	if ( !VectorEmpty(logBuffer) ) {
		PF_RawSend(MASTER, VectorPtr(logBuffer), VectorSize(logBuffer), PF_LOG_MSGTAG);
	}
}

/*
 		#] PF_MUnlock : 
 		#[ PF_WriteFileToFile :
*/

/**
 * Replaces WriteFileToFile() on the master and slaves.
 *
 * It copies the given buffer into internal buffers if called between
 * MLOCK(ErrorMessageLock) and MUNLOCK(ErrorMessageLock) for slaves and
 * handle is StdOut or LogHandle, otherwise calls WriteFileToFile().
 *
 * @param  handle  a file handle that specifies the output.
 * @param  buffer  a pointer to the source buffer containing the data to be written.
 * @param  size    the size of data to be written in bytes.
 * @return         the actual size of data written to the output in bytes.
 */
LONG PF_WriteFileToFile(int handle, UBYTE *buffer, LONG size)
{
	if ( PF.me != MASTER && errorMessageLock > 0 ) {
		if ( handle == AM.StdOut ) {
			VectorPushBacks(stdoutBuffer, buffer, size);
			return size;
		}
		else if ( handle == AC.LogHandle ) {
			VectorPushBacks(logBuffer, buffer, size);
			return size;
		}
	}
#ifdef PF_ENABLE_STDOUT_BUFFERING
	/*
	 * On my computer, sometimes a single linefeed "\n" sent to the standard
	 * output is ignored on the execution of mpiexec. A typical example is:
	 *   $ cat foo.c
	 *     #include <unistd.h>
	 *     int main() {
	 *       write(1, "    ", 4);
	 *       write(1, "\n", 1);
	 *       write(1, "    ", 4);
	 *       write(1, "123\n", 4);
	 *       return 0;
	 *     }
	 * or even as a shell script:
	 *   $ cat foo.sh
	 *     #! bin/sh
	 *     printf "    "
	 *     printf "\n"
	 *     printf "    "
	 *     printf "123\n"
	 * When I ran it on mpiexec
	 *   $ while :; do mpiexec -np 1 ./foo.sh; done
	 * I observed the single linefeed (printf "\n") was sometimes ignored. Even
	 * though this phenomenon might be specific to my environment, I added this
	 * code because someone may encounter a similar phenomenon and feel it
	 * frustrating. (TU 16 Jun 2011)
	 *
	 * Phenomenon:
	 *   A single linefeed sent to the standard output occasionally ignored
	 *   on mpiexec.
	 *
	 * Environment:
	 *   openSUSE 11.4 (x86_64)
	 *   kernel: 2.6.37.6-0.5-desktop
	 *   gcc: 4.5.1 20101208
	 *   mpich2-1.3.2p1 configured with '--enable-shared --with-pm=smpd'
	 *
	 * Solution:
	 *   In Unix (in which Uwrite() calls write() system call without any buffering),
	 *   we perform the line buffering here. A single linefeed is also buffered.
	 *
	 * XXX:
	 *   At the end of the program the buffered output (text without LF) will not be flushed,
	 *   i.e., will not be written to the standard output. This is not problematic at a normal run.
	 */
	if ( PF.me == MASTER && handle == AM.StdOut ) {
		size_t oldsize;
		/* Assume the newline character is LF (when UNIX is defined). */
		if ( (size > 0 && buffer[size - 1] != LINEFEED) || (size == 1 && buffer[0] == LINEFEED) ) {
			VectorPushBacks(stdoutBuffer, buffer, size);
			return size;
		}
		if ( (oldsize = VectorSize(stdoutBuffer)) > 0 ) {
			LONG ret;
			VectorPushBacks(stdoutBuffer, buffer, size);
			ret = WriteFileToFile(handle, VectorPtr(stdoutBuffer), VectorSize(stdoutBuffer));
			VectorClear(stdoutBuffer);
			if ( ret < 0 ) {
				return ret;
			}
			else if ( ret < (LONG)oldsize ) {
				return 0;  /* This means the buffered output in previous calls is lost. */
			}
			else {
				return ret - (LONG)oldsize;
			}
		}
	}
#endif
	return WriteFileToFile(handle, buffer, size);
}

/*
 		#] PF_WriteFileToFile : 
 		#[ PF_ReceiveErrorMessage :
*/

/**
 * Receives an error message from a slave's PF_MUnlock() call, and writes
 * the message to the corresponding output.
 * instead of LOCK(ErrorMessageLock) and UNLOCK(ErrorMessageLock).
 *
 * @param  src  the source process.
 * @param  tag  the tag value (must be PF_STDOUT_MSGTAG or PF_LOG_MSGTAG or PF_ANY_MSGTAG).
 */
static void PF_ReceiveErrorMessage(int src, int tag)
{
	/* Only on the master. */
	int size;
	int ret = PF_RawProbe(&src, &tag, &size);
	if ( ret == 0 ) {
		switch ( tag ) {
			case PF_STDOUT_MSGTAG:
			case PF_LOG_MSGTAG:
				VectorReserve(recvBuffer, size);
				ret = PF_RawRecv(&src, VectorPtr(recvBuffer), size, &tag);
				if ( ret > 0 ) {
					int handle = (tag == PF_STDOUT_MSGTAG) ? AM.StdOut : AC.LogHandle;
#ifdef PF_ENABLE_STDOUT_BUFFERING
					if ( handle == AM.StdOut ) PF_WriteFileToFile(handle, VectorPtr(recvBuffer), size);
					else
#endif
					WriteFileToFile(handle, VectorPtr(recvBuffer), size);
				}
				break;
		}
	}
}

/*
 		#] PF_ReceiveErrorMessage : 
 		#[ PF_CatchErrorMessages :
*/

/**
 * Processes all incoming messages whose tag is PF_STDOUT_MSGTAG
 * or PF_LOG_MSGTAG. It ensures that the next PF_Recieve(src, PF_ANY_MSGTAG, ...)
 * will not receive the message with PF_STDOUT_MSGTAG or PF_LOG_MSGTAG.
 *
 * @param  src  the source process.
 */
static void PF_CatchErrorMessages(int src)
{
	/* Only on the master. */
	for (;;) {
		int next = src;
		int tag = PF_ANY_MSGTAG;
		int ret = PF_RawProbe(&next, &tag, NULL);
		if ( ret == 0 ) {
			if ( tag == PF_STDOUT_MSGTAG || tag == PF_LOG_MSGTAG ) {
				PF_ReceiveErrorMessage(next, tag);
				continue;
			}
		}
		break;
	}
}

/*
 		#] PF_CatchErrorMessages : 
 		#[ PF_CatchErrorMessagesForAll :
*/

/**
 * Calls PF_CatchErrorMessages() for all slaves.
 * Note that it is NOT equivalent to PF_CatchErrorMessages(PF_ANY_SOURCE).
 */
static void PF_CatchErrorMessagesForAll(void)
{
	/* Only on the master. */
	int i;
	for ( i = 1; i < PF.numtasks; i++ ) PF_CatchErrorMessages(i);
}

/*
 		#] PF_CatchErrorMessagesForAll : 
 		#[ PF_ProbeWithCatchingErrorMessages :
*/

/**
 * Same as PF_Probe() except processing incoming messages with PF_STDOUT_MSGTAG
 * and PF_LOG_MSGTAG.
 *
 * @param[in,out]  src  the source process. The output value is that of the actual found message.
 * @return              the tag value of the next incoming message if found,
 *                      0 if a nonbloking probe (input src != PF_ANY_SOURCE) did not
 *                      find any messages. The negative returned value indicates an error.
 */
static int PF_ProbeWithCatchingErrorMessages(int *src)
{
	for (;;) {
		int newsrc = *src;
		int tag = PF_Probe(&newsrc);
		if ( tag == PF_STDOUT_MSGTAG || tag == PF_LOG_MSGTAG ) {
			PF_ReceiveErrorMessage(newsrc, tag);
			continue;
		}
		if ( tag > 0 ) *src = newsrc;
		return tag;
	}
}

/*
 		#] PF_ProbeWithCatchingErrorMessages : 
 		#[ PF_FreeErrorMessageBuffers :
*/

/**
 * Frees the buffers allocated for the synchronized output.
 *
 * Currently, not used anywhere, but could be used in PF_Terminate().
 */
void PF_FreeErrorMessageBuffers(void)
{
	VectorFree(stdoutBuffer);
	VectorFree(logBuffer);
}

/*
 		#] PF_FreeErrorMessageBuffers : 
  	#] Synchronised output : 
*/
