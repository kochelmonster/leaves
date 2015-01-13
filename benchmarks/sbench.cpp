//@+leo-ver=5-thin
//@+node:michael.20150111145018.91: * @file sbench.cpp
#include <stdlib.h>
#include <memory.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include "larch/leaves.h"
#include <memory>
#include <iostream>

using namespace larch_leaves;

#if !defined(_WIN32)
unsigned long long rd_clock ()
{
unsigned int low, high;

  __asm__ volatile("rdtsc" : "=a"(low), "=d" (high)); 
  return (unsigned long long)low | (unsigned long long)high << 32;
}
#endif


#if !defined(_WIN32)
// naskitis.com:
// This function will report the actual process size.
// note: this many not work on an Apple OS linux console.

typedef struct timeval timer;

unsigned long long report_process_size(void)
{
  FILE * statf;
  char fname[1024];
  char commbuf[1024];
  char state;
  pid_t mypid;
  unsigned long long vsize=0;
  unsigned int ppid, pgrp, session, ttyd, tpgid, flags, minflt, cminflt, majflt, cmajflt;
  unsigned int utime, stime, cutime, cstime, counter, priority, timeout, itrealvalue;
  unsigned int starttime, rss, rlim, startcode, endcode, startstack, kstkesp, ksteip;
  unsigned int signal, blocked, sigignore, sigcatch, wchan, ret, pid;
 
  mypid = getpid();
  snprintf(fname, 1024, "/proc/%u/stat", mypid);
  statf = fopen(fname, "r");
  ret = fscanf(statf, "%lu %s %c %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu "
                      "%lu %llu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
               &pid, commbuf, &state, &ppid, &pgrp, &session, &ttyd, &tpgid, &flags, &minflt, &cminflt, &majflt,
               &cmajflt, &utime, &stime, &cutime, &cstime, &counter, &priority, &timeout, &itrealvalue,
               &starttime, &vsize, &rss, &rlim, &startcode, &endcode, &startstack, &kstkesp, &ksteip, &signal,
               &blocked, &sigignore, &sigcatch, &wchan);
 
  if (ret != 35)
     fprintf(stderr, "Failed to read all 35 fields, only %lu decoded\n", ret);
 
  fclose(statf);
  return vsize; 
}
#endif

unsigned long long MaxMem = 0;
unsigned long long Searches = 0;
unsigned long long Probes = 0;
unsigned long long Bucket = 0;
unsigned long long Pail = 0;
unsigned long long Radix = 0;
unsigned long long Small = 0;

int Words = 0;
int Inserts = 0;
int Missing = 0;
int Found = 0;


int main (int argc, char **argv)
{
FILE *in, *in2;
char *askitis;

double insert_real_time=0.0;
double search_real_time=0.0;
unsigned long long size, off, prev;
#if !defined(_WIN32)
timer start, stop;
#else
clock_t start[1], stop[1];
#endif
unsigned long long startcycles, stopcycles;

  if( argc > 1 )
    in = fopen (argv[1], "rb");
  else
    in = NULL;

  if( argc > 2 && strlen(argv[2]) > 0 )
    in2 = fopen (argv[2], "rb");
  else
    in2 = NULL;

  if( !in )
    fprintf (stderr, "unable to open input file #1\n");


  std::unique_ptr<MemoryDatabase> db(MemoryDatabase::create());
  
#if !defined(_WIN32)
  size = lseek (fileno(in), 0L, 2);
  askitis = (char*)malloc(size);
  lseek (fileno(in), 0L, 0);
#else
  size = _lseeki64 (fileno(in), 0L, 2);
  askitis = malloc(size);
  _lseeki64 (fileno(in), 0L, 0);
#endif
  off = 0;

  do {
    prev = read (fileno(in), askitis+off,size-off > 65536 ? 65536 : size-off);
    off += prev;
  } while( off < size );

//  naskitis.com:
//  Start the timer. 
  
#if !defined(_WIN32)
  gettimeofday(&start, NULL);
  startcycles = rd_clock();
#else
  QueryProcessCycleTime(GetCurrentProcess(), &startcycles);
  *start = clock();
#endif

  for( prev = off = 0; off < size; off++ )
    if( askitis[off] == '\n' ) {
      Words++;
      db->find(Slice(askitis+prev, off-prev));
      if (db->is_valid()) {
        Found++;
      } else {
        db->set_value(Slice());
        Inserts++;
      }
    prev = off + 1;
    }

//  naskitis.com:
//  Stop the timer and do some math to compute the time required to insert the strings into the hat array.

#if !defined(_WIN32)
  stopcycles = rd_clock();
  gettimeofday(&stop, NULL);
  
  insert_real_time = 1000.0 * ( stop.tv_sec - start.tv_sec ) + 0.001 * (stop.tv_usec - start.tv_usec );
  insert_real_time = insert_real_time/1000.0;
#else
  *stop = clock();
  QueryProcessCycleTime(GetCurrentProcess(), &stopcycles);
  insert_real_time = (*stop - *start) / (float)CLOCKS_PER_SEC;
#endif

//  naskitis.com:
//  Free the input buffer used to store the first file.  We must do this before we get the process size below. 
  free (askitis);
  
  MaxMem = db->pages()*4096;
  
  fprintf(stderr, "HatArray@Karl_Malbrain\nDASKITIS option enabled\n-------------------------------\n%-20s %.2f MB\n%-20s %.2f sec\n",
    "Hat Array size:", MaxMem/1000000., "Time to insert:", insert_real_time);
#if !defined(_WIN32)
  fprintf(stderr, "%-20s %.2f MB\n", "Process Size:", report_process_size()/1000000.);
#endif
  fprintf(stderr, "%-20s %d\n", "Words:", Words);
  fprintf(stderr, "%-20s %d\n", "Inserts:", Inserts);
  fprintf(stderr, "%-20s %d\n", "Found:", Found);
  fprintf(stderr, "%-20s %d\n", "Cycles/Insert", (stopcycles - startcycles)/Words);
  //fprintf(stderr, "%-20s %d\n", "Short Bucket:", Small);
  //fprintf(stderr, "%-20s %d\n", "Radix Nodes:", hat->counts[0]);
  //fprintf(stderr, "%-20s %d\n", "Bucket Nodes:", hat->counts[1]);
  //fprintf(stderr, "%-20s %d\n", "Pail Nodes:", hat->counts[3]);

  //for( idx = 4; idx <= HatMax; idx++ )
  //  fprintf(stderr, "HAT_%.4d Nodes:      %d\n", HatSize[idx], hat->counts[idx]);

  Words = 0;
  Probes = 0;
  Searches = 0;
  Pail = 0;
  Bucket = 0;
  Inserts = 0;
  Missing = 0;
  Found = 0;

//  search hat array

#if !defined(_WIN32)
  size = lseek (fileno(in2), 0L, 2);
  askitis = (char*)malloc(size);
  lseek (fileno(in2), 0L, 0);
#else
  size = _lseeki64 (fileno(in2), 0L, 2);
  askitis = malloc(size);
  _lseeki64 (fileno(in2), 0L, 0);
#endif
  off = 0;

  while( off < size ) {
    prev = read (fileno(in2), askitis+off,size-off > 65536 ? 65536 : size-off);
    off += prev;
  }

#if !defined(_WIN32)
  gettimeofday(&start, NULL);
  startcycles = rd_clock();
#else
  QueryProcessCycleTime(GetCurrentProcess(), &startcycles);
  *start = clock();
#endif
  for( prev = off = 0; off < size; off++ )
    if( askitis[off] == '\n' ) {
      Words++;
      db->find(Slice(askitis+prev, off-prev));
      if (db->is_valid())
        Found++;
      else
        Missing++;
        
    prev = off + 1;
    }

//  naskitis.com:
//  Stop the timer and do some math to compute the time required to search the hat array.

#if !defined(_WIN32)
  gettimeofday(&stop, NULL);
  search_real_time = 1000.0 * ( stop.tv_sec - start.tv_sec ) + 0.001  
  * (stop.tv_usec - start.tv_usec );
  search_real_time = search_real_time/1000.0;
  stopcycles = rd_clock();
#else  
  QueryProcessCycleTime(GetCurrentProcess(), &stopcycles);
  *stop = clock ();
  search_real_time = (*stop - *start) / (float)CLOCKS_PER_SEC;
#endif

  free (askitis);

  fprintf(stderr,"\n%-20s %.2f sec\n", "Time to search:", search_real_time);
  fprintf(stderr, "%-20s %d\n", "Words:", Words);
  fprintf(stderr, "%-20s %d\n", "Missing:", Missing);
  fprintf(stderr, "%-20s %d\n", "Found:", Found);
  fprintf(stderr, "%-20s %d\n", "Cycles/Search", (stopcycles - startcycles)/Words);
  fprintf(stderr, "%-20s %.2f\n", "nSec/Search:", 1000000000. * search_real_time);
  //fprintf(stderr, "%-20s %.2f\n", "Probes/Array:", (double)Probes / Searches);
  //fprintf(stderr, "%-20s %.2f\n", "Pail/Search:", (double)Pail / Searches);
  //fprintf(stderr, "%-20s %.2f\n", "Bucket/Search:", (double)Bucket / Words);
  //fprintf(stderr, "%-20s %.2f\n", "Radix/Search:", (double)Radix / Words);

  exit(0);
}

//@-leo
