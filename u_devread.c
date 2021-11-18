/*
 * This is a file system benchmark which attempts to study bottlenecks -
 * it is named 'Bonnie' after Bonnie Raitt, who knows how to use one.
 * 
 * Modified, 02/04/04, Kurt Garloff <garloff@suse.de>
 * Compile fix (Direct_IO related) for non-Linux platforms
 * Use small chunks (16k) in seek test to reproduce pre-1.3 results.
 * 
 * Modified, 02/02/20, Kurt Garloff <garloff@suse.de>
 * Fix HTML formatting bug (thanks to Rupert Kolb)
 * Support for Direct-IO (thanks to Chris Mason & Andrea Arcangeli)
 * 
 * Modified, 00/09/09, Kurt Garloff <garloff@suse.de>
 * Put under CVS. Proper srand init for seeker processes.
 * Fixed SEGV on cmd line parsing.
 * 
 * Modified, 00/08/30, Kurt Garloff <garloff@suse.de>
 * -u enables the use of the putc/getc_unlocked versions
 * machine name defaults to hostname now.
 * 
 * Modified, 00/01/26, Kurt Garloff <garloff@suse.de>
 * -p sets number of seeker processes (SeekProcCount),
 * -S the number of seeks. Optional -y causes data to be fsync()ed.
 * 
 * Modified, 00/01/15, Kurt Garloff <garloff@suse.de>:
 * Report results immediately; warn if phys ram > test sz
 * Tolerate optargs w/o space
 * 
 * Modified, 99/07/20, Kurt Garloff <garloff@suse.de>:
 * Delete files when interrupted: delfiles(); breakhandler();
 *
 * Modified version of 25/4/99, by Jelle Foks:
 * This version supports multiple volumes benchmarking, allowing for
 * >2GB benchmarks on 32-bit operating systems. Use the '-v' option
 * for this feature.
 *
 * Commentary on Bonnie's operations may be found at 
 * http://www.textuality.com/bonnie/intro.html
 *
 * COPYRIGHT NOTICE: 
 * Copyright (c) Tim Bray, 1990-1996.
 * Copyright (c) Kurt Garloff, 1999-2002.
 *
 * Everybody is hereby granted rights to use, copy, and modify this program, 
 *  provided only that this copyright notice and the disclaimer below
 *  are preserved without change.
 * DISCLAIMER:
 * This program is provided AS IS with no warranty of any kind, and
 * The author makes no representation with respect to the adequacy of this
 *  program for any particular purpose or with respect to its adequacy to 
 *  produce any particular result, and
 * The author shall not be liable for loss or damage arising out of
 *  the use of this program regardless of how sustained, and
 * In no event shall the author be liable for special, direct, indirect
 *  or consequential damage, loss, costs or fees or expenses of any
 *  nature or kind.
 */
/* $Id: Bonnie.c,v 1.6 2002/04/04 12:59:46 garloff Exp $ */

#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <string.h>
#if defined(SysV)
#include <limits.h>
#include <sys/times.h>
#else
#include <sys/resource.h>
#endif

/* these includes can safely be left out in many cases. 
 * Included for less warnings with CFLAGS = -Wall */
#include <sys/types.h>
#include <sys/wait.h>
#include <stdlib.h>

#ifdef __linux__
/* for o_direct */
//#include <asm/page.h>
#define O_DIRECT 040000
#define PAGE_SHIFT		12
#define PAGE_SIZE		(1 << PAGE_SHIFT)
#define PAGE_MASK		(~(PAGE_SIZE-1))
#endif

#ifdef unix
#include <signal.h>
#endif

#define IntSize (sizeof(int))

/*
 * N.B. in seeker_reports, CPU appears and Start/End time, but not Elapsed,
 *  so position 1 is re-used; icky data coupling.
 */
#define CPU (0)
#define Elapsed (1)
#define StartTime (1)
#define EndTime (2)
//#define Seeks (4000)
unsigned Seeks = 4000;
#define UpdateSeek (10)
//#define SeekProcCount (3)
unsigned SeekProcCount = 3;
#define Chunk (1<<20)
#define SmallChunk (8192)
#define SeekChunk (16384)

#if defined(__linux__)
//# include <asm/fcntl.h>
# if !defined(O_DIRECT) && (defined(__alpha__) || defined(__i386__))
#  define O_DIRECT 040000 /* direct disk access */
# endif
# ifdef O_DIRECT
#  define SUPPORT_DIO
# endif
#endif


static double cpu_so_far();

static void   get_delta_t();

static double time_so_far();
static void   timestamp();
static void   usage();

/* 
 * Housekeeping variables to build up timestamps for the tests;
 *  global to make it easy to keep track of the progress of time.
 * all of this could have been done with non-global variables,
 *  but the code is easier to read this way and I don't anticipate
 *  much software engineering down the road 
 */
static int    basetime;                  /* when we started */
static double delta[2]; /* array of DeltaT values */
static double last_cpustamp = 0.0;       /* for computing delta-t */
static double last_timestamp = 0.0;      /* for computing delta-t */

static const char * version = "1.0";

char 	dosync  = 0;
char	useunlock = 0;
#ifdef SUPPORT_DIO
char	o_direct = 0;
#else 
#define o_direct 0
#endif

#define MAX_ITEM  10

int test_blocksize[]={
    1024*1024,  256*1024,   64*1024,    16*1024,
    4096,   2048,   1024,   512,    128,    64,     32,
    16,     8,  4,  2,  1
};

int main(
  int    argc,
  char * argv[])
{
  char   *__buf ;
  int    *buf ;
  int    fd=-1;
  char *filename = argv[1];
  off_t  size;
  int next;
  int    memsz;
  register off_t  words;
  int blocksize,item = 0;
  off_t dev_size,start_addr;
  int readsize;

  __buf = malloc(Chunk + ~PAGE_MASK);
  if (!__buf) 
  {
    fprintf(stderr, "unable to malloc %d bytes\n", Chunk + ~PAGE_MASK);
    exit(1) ;
  }
  buf = (int *)((unsigned long)(__buf + ~PAGE_MASK) & PAGE_MASK);

  basetime = (int) time((time_t *) NULL);
  size = 100;

  /* pick apart args */
  for (next = 2; next < argc; next++) {
    int notyetparsed = 1;
    if (next < argc - 1) {
	notyetparsed = 0;
	if (memcmp(argv[next], "-s", 2) == 0)
		size = atol(argv[next][2]? &argv[next][2]: argv[++next]);
	else notyetparsed = 1;
    }
    if (notyetparsed) {
	if (memcmp(argv[next], "-y", 2) == 0)
		dosync = 1;
	else
		usage();
    }
  }

  if (size < 1)
    usage();
  
#if defined(_SC_PHYS_PAGES)
  memsz = sysconf (_SC_PHYS_PAGES);
  memsz *= sysconf (_SC_PAGESIZE);
  if (1024*1024*size <= memsz)
  {
	fprintf (stderr, "u_devdevread: Warning: You have %iMB RAM, but you test with only %iMB datasize!\n",
		memsz/(1024*1024), size);
	fprintf (stderr, "u_devdevread:          This might yield unrealistically good results,\n");
	fprintf (stderr, "u_devdevread:          for reading and seeking%s.\n",
		 (dosync? "": " and writing"));
  }
#endif
  /* sanity check - 32-bit machines can't handle more than 2047 Mb */
  if (sizeof(off_t) <= 4 && size > 2047)
  {
    fprintf(stderr, "File too large for 32-bit machine, sorry\n");
    fprintf(stderr, "Use multiple volumes instead (option -v)\n");
    free(__buf) ;
    exit(1);
  }

  sync ();
  /* size is in meg, rounded down to multiple of Chunk */
  size *= (1024 * 1024);
  size = Chunk * (size / Chunk);
  sleep (1); sync ();

   fd = open(filename,O_RDONLY);
   if(fd < 0)
   		fprintf(stderr, "open file failed\n");
   dev_size = 0x7fffffff;//lseek(fd,0,SEEK_END);
   fprintf(stderr, "Device read speed test\n");
   fprintf(stderr, "Device name: %s     Device Size: %d\n",filename,dev_size);
   fprintf(stderr, "\n"); 
   fflush(stderr);

   start_addr = lseek(fd,0,SEEK_SET);

   while(item<MAX_ITEM)
   {
       blocksize = test_blocksize[item++];
       words = 0;
       timestamp();
       while(words<size)
       {
       			readsize = read(fd, (char *) buf, blocksize);
            if(readsize < blocksize)
            {
                fprintf(stderr,"read error readsize=%d,blocksize=%d\n",readsize,blocksize);    
                break;
            }
           words+=blocksize;
           start_addr+=blocksize;
           
           if(blocksize>dev_size-start_addr)
           {
                start_addr = lseek(fd,0,SEEK_SET);
           } 
        }

       get_delta_t();
       fprintf(stderr, "done: block=%8d  %6d kB/s %5.1f %%CPU %3.3f s\n",blocksize,
    	(int)((double) size  / (delta[Elapsed] * 1024.0)),
    	delta[CPU] / delta[Elapsed] * 100.0, delta[Elapsed]);
   }

    close(fd);
    
  free(__buf) ;
  return(0);
}

static void
usage()
{
  fprintf(stderr, "u_devdevread %s: USAGE:\n", version);
  fprintf(stderr,
    "u_devdevread devname [-s size-in-Mb] [-y (=fsync)]\n");
  exit(1);
}

static void
timestamp()
{
  last_timestamp = time_so_far();
  last_cpustamp = cpu_so_far();
}

static void 
get_delta_t()
{
  delta[Elapsed] = time_so_far() - last_timestamp;
  delta[CPU] = cpu_so_far() - last_cpustamp;
}

static double 
cpu_so_far()
{
#if defined(SysV)
  struct tms tms;

  if (times(&tms) == -1)
    fprintf(stderr,"times");
  return ((double) tms.tms_utime) / ((double) sysconf(_SC_CLK_TCK)) +
    ((double) tms.tms_stime) / ((double) sysconf(_SC_CLK_TCK));

#else
  struct rusage rusage;

  getrusage(RUSAGE_SELF, &rusage);
  return
    ((double) rusage.ru_utime.tv_sec) +
      (((double) rusage.ru_utime.tv_usec) / 1000000.0) +
        ((double) rusage.ru_stime.tv_sec) +
          (((double) rusage.ru_stime.tv_usec) / 1000000.0);
#endif
}

static double
time_so_far()
{
#if defined(SysV)
  int        val;
  struct tms tms;

  if ((val = times(&tms)) == -1)
    fprintf(stderr,"times");

  return ((double) val) / ((double) sysconf(_SC_CLK_TCK));

#else
  struct timeval tp;

  if (gettimeofday(&tp, (struct timezone *) NULL) == -1)
    fprintf(stderr,"gettimeofday");
  return ((double) (tp.tv_sec - basetime)) +
    (((double) tp.tv_usec) / 1000000.0);
#endif
}




