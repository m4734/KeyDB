#pragma once

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>

//#ifndef MDC_DEF
//#define MDE_DEF

#define DEBUG 0
#define DEBUG2 0
#define DEBUG_TIME 0
#define DEBUG_TIME2 0
//#define DEBUG3 1

#define MDC_TYPE 2
/*
0 = original
1 = mdc
2 = group
3 = all
*/

#if (MDC_TYPE == 1 || MDC_TYPE == 3)
#define MDC_ON 1
#define ENABLE_MDCP 1
#endif

#if (MDC_TYPE == 2 || MDC_TYPE == 3)
#define GROUP_ON 1
#define ENABLE_MALLOC_GROUP 1
#define REMOVE_MINCORE 1

enum GROUP_NUM
{
	NONE_GROUP,
	VALUE_GROUP,
	META_GROUP,
	KEY_GROUP
};

void check_end(void* buf);
void check_end2(void *buf);

#endif

// i think need one of these
//malloc_group if these are commented
//ifdef THREAD2, it will be MDC+
//#define THREAD1
//#define THREAD2

//choose one
//#define ENABLE_MALLOC_GROUP 0
//#define ENABLE_MDCP 1


//#if ENABLE_MDCP
//	#define THREAD2
//#endif

#if (MDC_TYPE == 3)
#define THREAD2
#endif


#define PAGE_SHIFT 12UL
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define PAGE_MASK (~(PAGE_SIZE-1))
#define BATCH_SHIFT 6UL
#define BATCH_UNIT_IN_PAGES 64UL
#define BATCH_UNIT_IN_BYTES (BATCH_UNIT_IN_PAGES * PAGE_SIZE)

#define POINTER_SIZE_IN_BYTES 8UL

#define MIN(a,b)	(((a)<(b))?(a):(b))
#define MAX(a,b)	(((a)>(b))?(a):(b))

enum chk_type {
	CHKPOINT_VAL = 0,
	CHKPOINT_REF,
};

size_t mdc_fwrite(const void *buf, size_t size, size_t count, FILE *fp, int type);
size_t mdc_fread(void *buf, size_t size, size_t count, FILE *fp, int type);
int checkpoint_start(char *name);
int checkpoint_end(char *name, int free);
int restore_start(char *name);
void restore_end(void);

//cgmin
/*
unsigned long global_file_position;
int global_dbid; //uint64_t
int global_REF;
int global_sort;
int *global_num_in_page;
int global_max_page;

//reversed because it is fixed
#define MAX_IN_PAGE 4096/128 // need VAL threshold // 32
unsigned long *global_ref_file_position[MAX_IN_PAGE];
unsigned long *global_ref_dbid[MAX_IN_PAGE];

unsigned long get_file_position(FILE *fp);
void set_file_position(FILE *fp,unsigned long);
*/
//size_t global_size[10000000]; //temp size
//int gsn,gsi;


//#endif
