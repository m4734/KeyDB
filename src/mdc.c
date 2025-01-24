#define _XOPEN_SOURCE 600
#define _BSD_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/param.h>
#include <fcntl.h>
#include <sys/sysctl.h>

#include "mdc.h"

//#include "../deps/malloc_group/include/malloc.h"
#include "malloc_group.h"
#include <pthread.h>

#define MADV_PAGE_UNIT 0
#define MADV_DUMP_UNIT 1
#define MADV_VMA_UNIT 2

//#define MADVISE_UNIT_TYPE MADV_PAGE_UNIT
#define MADVISE_UNIT_TYPE 1

#define MADV_DONTNEED2 5

#define DEBUG 0
#define DEBUG2 0
#define DEBUG_TIME 0
#define DEBUG_TIME2 1

// i think need one of these
//malloc_group if these are commented
//ifdef THREAD2, it will be MDC+
//#define THREAD1
//#define THREAD2

//choose one
#define ENABLE_MALLOC_GROUP 0
#define ENABLE_MDCP 1

#define REMOVE_MINCORE 1

#if ENABLE_MDCP
	#define THREAD2
#endif


#if 0
#define __NR_mlock2 284

static int mlock2(const void *addr, size_t len, int flags)
{
	int ret = syscall(__NR_mlock2, addr, len, flags);
	if (ret == 0 || errno != ENOSYS)
		return ret;
	errno = (EINVAL);
	return -1;
}
#endif

struct vma_info {
	unsigned long start;
	unsigned long end;
	int stored;
	int dumped; //cgmin
	int* size_sum; //cgmin
	int* size_cnt;
	int* dumped2;
};

struct vma_entry {
	unsigned long vma_start;
	unsigned long vma_end;
	unsigned long bitmap_offset;
};

struct bitmap_entry {
	unsigned long bitmap;
	unsigned long page_offset;
};

struct address_space_table {
	unsigned long nr_entries;
	struct vma_info *table;
};

struct vma_table {
	unsigned long pos;
	unsigned long nr_entries;
	struct vma_entry *table;
};

struct bitmap_table {
	unsigned long pos;
	unsigned long nr_entries;
	struct bitmap_entry *table;
};

struct address_table {
	unsigned long nr_entries;
	unsigned long *table;
};

struct transactional_data {
	int vma_table_fd;
	int bitmap_table_fd;
	int dump_fd;
	pthread_t ra_thd;
	void *dump_mmap_addr;
	size_t dump_mmap_size;
	struct address_space_table as_table;
	struct vma_table vma_table;
	struct bitmap_table bm_table;
};

struct transaction {
	char name[256];
	struct transactional_data trx_data;
};

static pthread_mutex_t trx_mutex = PTHREAD_MUTEX_INITIALIZER;
struct transaction global_trx;


long total_time;
long _fread_original_time;
long _fread_time;
long _object_time;
long __file_offset_time;
long __pread_time;


//cgmin
long vs_time;
long dump_time;
long pmd_time;
long residency_time;
long bitmap_time;
long bitmap2_time;
/*
#ifdef THREAD1
int new_vma;
#endif
 */
#ifdef THREAD2
volatile int new_vma;
volatile int dump_exit;
pthread_t dump_thread;
void *dump_function();
#else
int new_vma;
#endif
//cgmin

unsigned long get_file_position(FILE *fp)
{
	//	return fseek(fp,0,SEEK_CUR);
	return ftell(fp);
}
void set_file_position(FILE *fp,unsigned long offset)
{
	fseek(fp,offset,SEEK_SET);
}


static int perform_memory_dump_for_vma_partial(struct transactional_data *trx_data,
		int partial, int free_after_write);

static inline long get_time_difference_ns(struct timespec *end, struct timespec *start) // jwpark
{
	return ((end->tv_sec-start->tv_sec)*1000000000 +
			end->tv_nsec-start->tv_nsec);
}

struct vma_info *vma_cache = NULL;

static struct bitmap_entry *allocate_bitmap_table(size_t nr_entries)
{
	return calloc(nr_entries, sizeof(struct bitmap_entry));
}

static size_t get_max_nr_bitmap_entries(struct address_space_table *as_table)
{
	size_t i = 0;
	size_t max_nr_entries = 0;
	for (; i < as_table->nr_entries; i++) {
		struct vma_info *vma = &as_table->table[i];
		max_nr_entries += MAX(((vma->end - vma->start) 
					/ BATCH_UNIT_IN_BYTES), 1);
	}
	return max_nr_entries;

}

static int create_bitmap_table(struct address_space_table *as_table,
		struct bitmap_table *bm_table)
{
	size_t max_nr_entries;
	struct bitmap_entry *table;
	max_nr_entries = get_max_nr_bitmap_entries(as_table);
	if (!max_nr_entries) {
		printf("%s: max_nr_entries = 0\n", __func__);
		return -1;
	}
	table = allocate_bitmap_table(max_nr_entries);
	if (!table) {
		printf("%s: allocate bitmap table failed\n", __func__);
		return -1;
	}

	bm_table->table = table;
	bm_table->nr_entries = max_nr_entries;
	return 0;
}

static struct vma_entry *allocate_vma_table(size_t nr_entries)
{
	return calloc(nr_entries, sizeof(struct vma_entry));
}

static inline size_t get_max_nr_vma_entries(
		struct address_space_table *as_table)
{
	return as_table->nr_entries;
}

static int create_vma_table(struct address_space_table *as_table, 
		struct vma_table *vma_table)
{
	size_t max_nr_entries;
	struct vma_entry *table;

	max_nr_entries = get_max_nr_vma_entries(as_table);
	if (!max_nr_entries) {
		printf("%s: max_nr_entries = 0\n", __func__);
		return -1;
	}
	table = allocate_vma_table(max_nr_entries);
	if (!table) {
		printf("%s: allocate vma table failed\n", __func__);
		return -1;
	}

	vma_table->table = table;
	vma_table->nr_entries = max_nr_entries;

	return 0;
}


static void close_file_for_vma_info(FILE *fp)
{
	fclose(fp);
}

static int fill_vma_info(char *line, struct vma_info *vma)
{
	char *p = strchr(line,'-');
	char *p2= strchr(line,' ');
	if (p && p2) {
		*p = '\0';
		*p2 = '\0';
		vma->start = strtoul(line,NULL,16);
		p++;
		vma->end = strtoul(p,NULL,16);
#if DEBUG | DEBUG2
		printf("%lx-%lx\n", vma->start, vma->end);
#endif

		//cgmin size sum
		//printf("size_sum\n");
		int i,pn;

		pn = (vma->end-vma->start)/4096;
		vma->size_sum = (int*)malloc(pn*sizeof(int));
		vma->size_cnt = (int*)malloc(pn*sizeof(int));
		vma->dumped2 = (int*)malloc(pn*sizeof(int));
		//printf("pages %d\n",p);
		for (i=0;i<pn;i++)
		{
			//	vma->size_sum[i] = zget_size_sum((void*)(vma->start+i*4096));
			vma->size_sum[i] = -1; // why we need -1??? // if it is not zmalloc it makes error..
			vma->size_cnt[i] = 0;
			vma->dumped2[i] = 0;
			if (vma->size_sum[i] > 4096)
				printf("size sum big %d\n",vma->size_sum[i]);
			//printf("%d ",vma->size_sum[i]);
		}
		//printf("size_sum end\n");

		return 0;
	}
	return -1;
}

static void load_vma_info_into_table_from_file(struct vma_info *table, 
		FILE *vma_fp)
{
	char line[1024];
	unsigned long idx = 0;
	fseek(vma_fp, 0, SEEK_SET);
#if DEBUG | DEBUG2
	printf("\n<Address Space Table>\n");
#endif
	while (fgets(line,sizeof(line),vma_fp) != NULL) {
		if (!fill_vma_info(line, &table[idx]))
			idx++;
	}
#if DEBUG | DEBUG2
	printf("\n");
#endif
}

static struct vma_info *allocate_address_space_table(unsigned long nr_entries)
{
	return calloc(nr_entries, sizeof(struct vma_info));
}

static unsigned long get_nr_vmas(FILE *vma_fp)
{
	char line[1024];
	unsigned long nr_vma_info = 0;
	while (fgets(line,sizeof(line),vma_fp) != NULL) {
		nr_vma_info++;
	}
	return nr_vma_info;
}

static FILE *open_file_for_vma_info(void)
{
	FILE *fp;
	fp = fopen("/proc/self/maps", "r");
	if (!fp)
		printf("%s: open failed vma_info\n", __func__);
	return fp;
}

static int load_vma_info_into_table(struct address_space_table *as_table)
{
	struct vma_info *table;
	unsigned long nr_vma_info;
	FILE *fp;

	fp = open_file_for_vma_info();
	if (!fp) 
		return -1;

	nr_vma_info = get_nr_vmas(fp);

	table = allocate_address_space_table(nr_vma_info);
	if (!table)
		goto error_close_file;

	load_vma_info_into_table_from_file(table, fp);

	if (as_table->table)
		free(as_table->table);

	as_table->table = table;
	as_table->nr_entries = nr_vma_info;

	close_file_for_vma_info(fp);
	return 0;
error_close_file:
	printf("%s: allocate address space table failed\n", __func__);
	close_file_for_vma_info(fp);
	return -1;
}

static void initialize_address_space_table(struct address_space_table *table)
{
	table->nr_entries = 0;
	table->table = NULL;
}

static void initialize_vma_table(struct vma_table *table)
{
	table->pos = 0;
	table->nr_entries = 0;
	table->table = NULL;
}

static void initialize_bitmap_table(struct bitmap_table *table)
{
	table->pos = 0;
	table->nr_entries = 0;
	table->table = NULL;
}

static void initialize_transactional_data(struct transactional_data *trx_data)
{
	trx_data->vma_table_fd = -1;
	trx_data->bitmap_table_fd = -1;
	trx_data->dump_fd = -1;
	initialize_address_space_table(&trx_data->as_table);
	initialize_vma_table(&trx_data->vma_table);
	initialize_bitmap_table(&trx_data->bm_table);
}


static void set_transaction_name(char *dst, char *src)
{
	if (!src) {
		char tmpfile[256];
		snprintf(tmpfile,256,"temp-%d",(int) getpid());
		strncpy(dst, tmpfile, 256);
	} else {
		strcpy(dst, src);
	}
}

static void initialize_transaction(struct transaction *trx, char *name)
{
	pthread_mutex_lock(&trx_mutex);
	initialize_transactional_data(&trx->trx_data);
	set_transaction_name(trx->name, name);
}

static void create_new_name_with_suffix(char *new_name, size_t size, 
		char *old_name, char *suffix)
{
	snprintf(new_name, size, "%s.%s", old_name, suffix);
}


static int ___open_file_for_read(char *filename)
{
	int fd = open(filename, O_RDONLY);
	if (fd < 0)
		printf("open failed %s\n", filename);
	return fd;
}

static int ___open_file_for_write(char *filename)
{
	int fd = open(filename, O_RDWR|O_CREAT|O_TRUNC, 0666);
	if (fd < 0)
		printf("open failed %s\n", filename);
	return fd;
}

static int __open_file_with_suffix(char *name, int write, char *suffix)
{
	char filename[128];
	create_new_name_with_suffix(filename, 
			sizeof(filename), name, suffix);
	if (write)
		return ___open_file_for_write(filename);
	return ___open_file_for_read(filename);
}

static int open_file_for_vma_table(char *name, int write)
{
	return __open_file_with_suffix(name, write, "vma");
}

static int open_file_for_bitmap_table(char *name, int write)
{
	return __open_file_with_suffix(name, write, "bitmap");
}

static int open_file_for_dump(char *name, int write)
{
	return __open_file_with_suffix(name, write, "dump");
}

static int __open_files(struct transactional_data *trx_data,
		char *name, int write)
{
	int fd;
	if (trx_data->vma_table_fd < 0) {
		fd = open_file_for_vma_table(name, write);
		if (fd < 0)
			goto error_return;
		trx_data->vma_table_fd = fd;
	}

	if (trx_data->bitmap_table_fd < 0) {
		fd = open_file_for_bitmap_table(name, write);
		if (fd < 0)
			goto error_close_vma_table;
		trx_data->bitmap_table_fd = fd;
	}
	if (trx_data->dump_fd < 0) {
		fd = open_file_for_dump(name, write);
		if (fd < 0)
			goto error_close_bitmap_table_and_vma_table;
		trx_data->dump_fd = fd;
	}
	return 0;

error_close_bitmap_table_and_vma_table:
	close(trx_data->bitmap_table_fd);
	trx_data->bitmap_table_fd = -1;
error_close_vma_table:
	close(trx_data->vma_table_fd);
error_return:
	trx_data->vma_table_fd = -1;
	return -1;

}

static int open_files_for_checkpointing(struct transactional_data *trx_data, 
		char *name)
{
	return __open_files(trx_data, name, 1);
}

static int prepare_for_checkpointing(struct transaction *trx)
{
	char *name = trx->name;
	struct transactional_data *trx_data = &trx->trx_data;
	if (open_files_for_checkpointing(trx_data, name))
		return -1;
	if (load_vma_info_into_table(&trx_data->as_table))
		return -1;
	if (create_vma_table(&trx_data->as_table, &trx_data->vma_table))
		return -1;
	return create_bitmap_table(&trx_data->as_table, &trx_data->bm_table);
}

int checkpoint_start(char *name)
{
	struct transaction *trx = &global_trx;

	//#ifdef USE_MALLOC_GROUP
	//zmalloc_group(0,10); //cgmin malloc group test
	//malloc_group(10,0);
	//#endif



#ifdef THREAD2
	dump_exit = 0;
	pthread_create(&dump_thread,NULL,dump_function,NULL);
#endif

	//moved to after size scan



	dump_time=0;
	residency_time=0;
	bitmap_time=0;
	bitmap2_time=0;

#if DEBUG
	printf("\n>> checkpoint_start(%s)\n", name);
#endif
	initialize_transaction(trx, NULL);
	return prepare_for_checkpointing(trx);
}

static int __mdc_fwrite_for_chkpointing_reference(const void *buf,
		size_t size, size_t count, FILE *fp)
{
	unsigned long i = 0;
	unsigned long address = (unsigned long)buf;

	for (; i < count; i++) {
		size_t result = fwrite(&address, POINTER_SIZE_IN_BYTES, 1, fp);
		if (result != 1) {
			printf("%s: fwrtie failed %lu\n", __func__, result);
			return -1;
		}
		address += size;
	}
	return 0;
}

#if DEBUG
static void print_bitmap_table(struct bitmap_table *table)
{
	size_t idx;
	printf("<Bitmap Table>\nbitmap,page_offset\n");
	for (idx = 0; idx < table->pos; idx++) {
		printf("%lx,%lx\n", 
				table->table[idx].bitmap,
				table->table[idx].page_offset);
	}
	printf("\n");
}
#endif

#if DEBUG | DEBUG2
static void print_vma_table(struct vma_table *table)
{
	size_t idx;
	printf("\n<VMA Table>\nvma_start,bitmap_offset\n");
	for (idx = 0; idx < table->pos; idx++) {
		printf("%lx,%lx\n", table->table[idx].vma_start,
				table->table[idx].bitmap_offset);
	}
	printf("\n");
}
#endif


static int save_bitmap_table(struct transactional_data *trx_data,
		struct bitmap_entry *be)
{
	size_t bytes = write(trx_data->bitmap_table_fd, be, 
			sizeof(struct bitmap_entry));
	if (bytes != sizeof(struct bitmap_entry))
		return -1;
	return 0;
}

static int __perform_memory_dump_in_batch(struct transactional_data *trx_data,
		unsigned long start_addr, unsigned long end_addr, 
		unsigned char *residency_vec)
{
	unsigned long nr_pages = (end_addr - start_addr) >> PAGE_SHIFT;

	unsigned long addr = start_addr;

	size_t i;

#if 0
	unsigned long batch_size = 0;
	for (i = 0; i < nr_pages; i++) {
		if (residency_vec[i]) {
			batch_size += PAGE_SIZE;
		} else {
			size_t bytes = write(trx_data->dump_fd, (void *)addr, batch_size);
			if (bytes != batch_size)
				return -1;
			addr += batch_size;
			batch_size = 0;
		}
	}
	if (batch_size) {
		size_t bytes = write(trx_data->dump_fd, (void *)addr, batch_size);
		if (bytes != batch_size)
			return -1;
	}
	return 0;
#else
	int nr_non_present_pages = 0;
	for (i = 0; i < nr_pages; i++) {
		if (residency_vec[i]) { //cgmin what about is not resident free??
			size_t bytes = write(trx_data->dump_fd, (void *)addr, PAGE_SIZE);
			if (bytes != PAGE_SIZE)
				return -1;
#if MADVISE_UNIT_TYPE == MADV_PAGE_UNIT
			if (false && madvise((void *)addr, PAGE_SIZE, MADV_DONTNEED2)) { //cgmin size_sum have to full
				printf("madvise with page unit failed %d\n", errno);
				return -1;
			}
#endif
			//printf("memory dump addr %lx\n", addr);
		} else {
			nr_non_present_pages++;
		}
		addr += PAGE_SIZE;
	}
#if DEBUG
	if (nr_non_present_pages)
		printf("nr_non_present_pages = %d\n", nr_non_present_pages);
#endif
	return 0;
#endif
}

static unsigned long set_bitmap(unsigned long bitmap, unsigned long idx)
{
	if (idx > 63)
		printf("%s: idx > 63\n", __func__);
	return (bitmap) | (1UL << idx);
}

static void set_bitmap_entry_for_batch(
		struct transactional_data *trx_data, 
		struct bitmap_entry *be,
		unsigned char *residency_vec,
		size_t nr_pages)
{
	unsigned long bitmap = 0;
	unsigned long cur;
	for (cur = 0; cur < nr_pages; cur++) {
		if (residency_vec[cur])
			bitmap = set_bitmap(bitmap, cur);
	}
	be->bitmap = bitmap;
	be->page_offset = lseek(trx_data->dump_fd, 0, SEEK_CUR);
#if DEBUG
	printf("%s: bm_table[%lu] bitmap:%lx, page_offset:%lx\n",
			__func__, trx_data->bm_table.pos - 1, 
			be->bitmap, be->page_offset);
#endif
	return;
}

static int get_page_residency(unsigned char *residency_vec,
		unsigned long start_addr, size_t nr_pages)
{
#if REMOVE_MINCORE
	// remove mincore

	size_t i;
	for (i = 0; i < nr_pages; i++)
		residency_vec[i] = 255;
	return 0;
#else
	if (mincore((void *)start_addr, nr_pages << PAGE_SHIFT, 
				residency_vec)) {
		printf("%s: mincore error %d\n", __func__, errno);
		return -1;
	} else {
#if 1
		size_t i;
		for (i = 0; i < nr_pages; i++) {
			printf("residency_vec[%lu]=%x\n", i, residency_vec[i]);
		}
#endif
		return 0;
	}
#endif
}

static unsigned long get_end_address_in_current_batch(unsigned long address, 
		unsigned long vma_end_address)
{
	return MIN(vma_end_address, address + BATCH_UNIT_IN_BYTES);
}

static inline struct bitmap_entry *get_free_bitmap_entry(
		struct bitmap_table *bm_table)
{
	return &bm_table->table[bm_table->pos++];
}

static inline struct vma_entry *get_free_vma_entry(struct vma_table *vma_table)
{
	return &vma_table->table[vma_table->pos++];
}

static int save_vma_info(struct transactional_data *trx_data, 
		struct vma_info *target_vma)
{
	size_t bytes;
	struct vma_entry *ve = get_free_vma_entry(&trx_data->vma_table);
	ve->vma_start = target_vma->start;
	ve->vma_end = target_vma->end;
	ve->bitmap_offset = lseek(trx_data->bitmap_table_fd, 0, SEEK_CUR);
	bytes = write(trx_data->vma_table_fd, ve, 
			sizeof(struct vma_entry));
	if (bytes != sizeof(struct vma_entry))
		return -1;
#if DEBUG
	printf("%s: vma_table[%lu] vma_start:%lx, vma_end:%lx, bitmap_offset:%lx\n",
			__func__, trx_data->vma_table.pos - 1, 
			ve->vma_start, ve->vma_end, ve->bitmap_offset);
#endif
	return 0;
}
int mc=0;
int nmc=0;
void check_and_free(struct vma_info *target_vma, int index) //cgmin size_sum
{
	//printf(" max %d  index  %d  ",(target_vma->end-target_vma->start)/4096,index);
	if ((target_vma->end-target_vma->start)/4096 <= index)
	{
		printf("  index error!!  ");
		return;
	}

	if (target_vma->size_sum[index] == -1)
	{
		/*
		   int i,pn;
		   pn = (target_vma->end-target_vma->start)/4096;
		   for (i=0;i<pn;i++)
		   target_vma->size_sum[i] = zget_size_sum(((void *)(target_vma->start+i*4096)));
		 */
		target_vma->size_sum[index] = zget_size_sum(((void *)(target_vma->start+index*4096)));
		//	target_vma->size_sum[index] = 4096; // cgmin test
	}

	//printf("sum %d cnt %d\n",target_vma->size_sum[index],target_vma->size_cnt[index]);
	/*
	   if (target_vma->dumped2[index])
	   {
	   printf("dumped error\n");
	   return;
	   }
	 */
#if ENABLE_MALLOC_GROUP
	if (target_vma->size_sum[index] == target_vma->size_cnt[index]/* && target_vma->dumped2[index]*/) //cgmin VAL
#else
	if (target_vma->size_sum[index] == target_vma->size_cnt[index] && target_vma->dumped2[index]) //cgmin VAL
#endif
	{
		//target_vma->dumped2[index] = 1;
		//return;
		//printf("madvise %p ",((void *)(target_vma->start+index*4096)));

		if(/*index > 0 && */madvise((void *)(target_vma->start+index*4096),4096,MADV_DONTNEED2)) // index 0 has heap metadata // not only 0 more pages have metadata
			printf("madvise error\n");

		mc++;

		//printf("mc %d madvise end\n",mc);
	}
	else if (target_vma->size_cnt[index] > target_vma->size_sum[index])
	{
		printf("size cnt big %d sum %d index %d\n",target_vma->size_cnt[index],target_vma->size_sum[index],index);
		if (index > 0)
			printf("-1 size cnt big %d sum %d\n",target_vma->size_cnt[index-1],target_vma->size_sum[index-1]);
		printf("+1 size cnt big %d sum %d\n",target_vma->size_cnt[index+1],target_vma->size_sum[index+1]);
	}
}

static int perform_memory_dump_for_vma(struct transactional_data *trx_data,
		struct vma_info *target_vma, int free_after_write) 
{
	unsigned long page_addr;
	struct timespec ts1,ts2;

#if DEBUG | DEBUG2
	printf("\nVMA %lx-%lx has been just stored.\n", target_vma->start,
			target_vma->end);
#endif
	if (save_vma_info(trx_data, target_vma)) {
		printf("%s: save_vma_info failed\n", __func__);
		return -1;
	}

	for (page_addr = target_vma->start; page_addr < target_vma->end;
			page_addr += BATCH_UNIT_IN_BYTES) {
		unsigned char residency_vec[BATCH_UNIT_IN_PAGES];
		unsigned long end_addr;
		size_t nr_pages;
		size_t size;
		struct bitmap_entry *be = get_free_bitmap_entry(&trx_data->bm_table);
		end_addr = get_end_address_in_current_batch(page_addr, target_vma->end);
		size = end_addr - page_addr;
		nr_pages = size >> PAGE_SHIFT;
		clock_gettime(CLOCK_MONOTONIC,&ts1);
		if (get_page_residency(residency_vec, page_addr, nr_pages)) {
			printf("%s: get_page_residency failed\n", __func__);
			return -1;
		}
		clock_gettime(CLOCK_MONOTONIC,&ts2);
		residency_time+=(ts2.tv_sec-ts1.tv_sec)*1000000000+ts2.tv_nsec-ts1.tv_nsec;
		clock_gettime(CLOCK_MONOTONIC,&ts1);
		set_bitmap_entry_for_batch(trx_data, be, residency_vec, nr_pages);
		clock_gettime(CLOCK_MONOTONIC,&ts2);
		bitmap_time+=(ts2.tv_sec-ts1.tv_sec)*1000000000+ts2.tv_nsec-ts1.tv_nsec;
#if MADVISE_UNIT_TYPE == MADV_DUMP_UNIT
#if 0
		if (mlock((void *)page_addr, size)) {
			printf("mlock failed %d addr:%lx size:%lx\n", 
					errno, page_addr, size);
			return -1;
		}
#endif
#endif
		clock_gettime(CLOCK_MONOTONIC,&ts1);
		if (__perform_memory_dump_in_batch(trx_data, page_addr, end_addr, residency_vec)) {
			printf("%s: __perform_memory_dump_in_batch failed\n", __func__);
			return -1;
		}
		clock_gettime(CLOCK_MONOTONIC,&ts2);
		dump_time+=(ts2.tv_sec-ts1.tv_sec)*1000000000+ts2.tv_nsec-ts1.tv_nsec;


		//cgmin madvise --------------------------------------------------------------
		//batch doesn't have start end addr
		if (free_after_write)
		{
			int index,eee=(end_addr-page_addr)/4096;
			int base = (page_addr-target_vma->start)/4096;
			//for (addr=page_addr;addr<page_addr + BATCH_UNIT_IN_BYTES;addr+=4096)
			for (index=0;index<eee;index++)
			{
				target_vma->dumped2[base+index] = 1;
				check_and_free(target_vma,base+index);
			}
		}
#if MADVISE_UNIT_TYPE == MADV_DUMP_UNIT
#if 0
		if (munlock((void *)page_addr, size)) {
			printf("mulock failed %d addr:%lx size:%lx\n", 
					errno, page_addr, size);
		}
#endif
		if (0 && free_after_write) { //cgmin size_sum
			if (madvise((void *)page_addr, size, MADV_DONTNEED2)) {
				printf("madvise with dump unit failed %d\n", errno);
				return -1;
			}
		}
#endif
		clock_gettime(CLOCK_MONOTONIC,&ts1);

		if (save_bitmap_table(trx_data, be)) {
			printf("%s: save_bitmap_table failed\n", __func__);
			return -1;
		}
		clock_gettime(CLOCK_MONOTONIC,&ts2);
		bitmap2_time+=(ts2.tv_sec-ts1.tv_sec)*1000000000+ts2.tv_nsec-ts1.tv_nsec;

#if DEBUG
		printf("bitmap %lx, end_addr %lx\n", be->bitmap, end_addr);
#endif
	}
#if MADVISE_UNIT_TYPE == MADV_VMA_UNIT
	if (0 && free_after_write) { //cgmin size_sum
		if (madvise((void *)target_vma->start, target_vma->end - target_vma->start, 
					MADV_DONTNEED2)) {
			printf("madvise with vma unit failed %d\n", errno);
			return -1;
		}
	}
#endif
#if DEBUG
	print_vma_table(&trx_data->vma_table);
	//print_bitmap_table(&trx_data->bm_table);
#endif
	return 0;
}

static size_t is_set_bitmap(unsigned long bitmap, unsigned long idx)
{
	if (idx > 63)
		printf("%s: idx > 63\n", __func__);
	return (bitmap) & (1UL << idx);
}

static unsigned long page_index(struct bitmap_entry *bitmap_entry, 
		unsigned long target_bitmap_address,
		unsigned long address)
{
	size_t i;
	size_t page_index = 0;
	//size_t aligned_address = ((address + (PAGE_SIZE - 1)) >> PAGE_SHIFT) << PAGE_SHIFT;
	size_t aligned_address = ((address >> PAGE_SHIFT) + 1) << PAGE_SHIFT;
	size_t pages_to_check = 

		((aligned_address - target_bitmap_address) >> PAGE_SHIFT);
#if DEBUG
	printf("%s: address %lx, aligned_address %lx,  target_bitmap_address %lx, diff %lx, pages_to_check %lu\n", __func__,
			address, aligned_address, target_bitmap_address, aligned_address - target_bitmap_address, pages_to_check);
#endif
#if 0
	if (pages_to_check > BATCH_UNIT_IN_PAGES)
		printf("%s: pages_to_check %lu > BATCH_UNIT_IN_PAGES\n", __func__,
				pages_to_check);
	else
		printf("%s: pages_to_check %lu <= BATCH_UNIT_IN_PAGES\n", __func__,
				pages_to_check);
#endif

	for (i = 0; i < pages_to_check; i++) {
		if (is_set_bitmap(bitmap_entry->bitmap, i))
			page_index++;
#if DEBUG
		else
			printf("bitmap[%lu]=0\n",i);
#endif
	}
#if DEBUG
	printf("%s: bitmap %lx, page_index %lx\n", __func__, bitmap_entry->bitmap, page_index - 1);
#endif

	return page_index - 1;
}

static unsigned long page_offset(struct bitmap_entry *bitmap_entry,
		unsigned long target_bitmap_address,
		unsigned long address)
{
	unsigned long base = bitmap_entry->page_offset;
	unsigned long index = page_index(bitmap_entry, 
			target_bitmap_address, address);
#if DEBUG
	printf("%s: page offset = %lx + %lx = %lx\n", __func__,
			base, index * PAGE_SIZE, base + (index * PAGE_SIZE));
#endif
	return base + (index * PAGE_SIZE);
}

static unsigned long dump_offset(unsigned long page_offset, 
		unsigned long address)
{
#if DEBUG
	printf("%s: dump offset = %lx + %lx = %lx\n", __func__,
			page_offset, address & (~PAGE_MASK), page_offset + (address & (~PAGE_MASK)));
#endif
	return page_offset + (address & (~PAGE_MASK));
}

static unsigned long bitmap_entry_index(
		struct vma_entry *vma_entry, unsigned long address)
{
#if DEBUG
	printf("%s: address:%lx, vma_start:%lx, vma_end:%lx, bitmap_entry_index:%lx\n",
			__func__, address, vma_entry->vma_start, vma_entry->vma_end,
			(address - vma_entry->vma_start) >> (PAGE_SHIFT + BATCH_SHIFT));
#endif

	return ((address - vma_entry->vma_start) 
			>> (PAGE_SHIFT + BATCH_SHIFT));
}

static struct bitmap_entry *get_base_bitmap_entry(
		struct vma_entry *vma_entry, struct bitmap_table *bm_table)
{
	unsigned long base_index = vma_entry->bitmap_offset / 
		//								sizeof(struct vma_entry);
		sizeof(struct bitmap_entry); //cgmin

#if DEBUG
	printf("%s: bitmap_table[%lu]'s bitmap %lx, page_offset %lx\n", __func__, base_index, bm_table->table[base_index].bitmap, bm_table->table[base_index].page_offset);
#endif
	return &bm_table->table[base_index];
}

static struct vma_entry *vma_entry_offset(struct vma_table *vma_table,
		unsigned long address)
{
	unsigned long cursor = 0;
	struct vma_entry *table = vma_table->table;

	//cgmin
	struct timespec ts1,ts2;
	clock_gettime(CLOCK_MONOTONIC, &ts1);

	for (; cursor < vma_table->pos; cursor++) {
		if (table[cursor].vma_end > address &&
				table[cursor].vma_start <= address) {
#if DEBUG
			printf("%s: vma_table[%lu]\n", __func__, cursor);
#endif
			clock_gettime(CLOCK_MONOTONIC, &ts2);
			vs_time+=(ts2.tv_sec-ts1.tv_sec)*1000000000+ts2.tv_nsec-ts1.tv_nsec;
			return &table[cursor];
		}
	}
	return NULL;
}

static unsigned long get_object_file_offset_in_dump(unsigned long address, 
		struct transactional_data *trx_data)
{
	struct vma_table *vma_table = &trx_data->vma_table;
	struct bitmap_table *bm_table = &trx_data->bm_table;
	struct vma_entry *target_vma_entry;
	struct bitmap_entry *base;
	unsigned long bm_entry_index;
	struct bitmap_entry *target_bitmap_entry;
	unsigned long target_bitmap_address;
	unsigned long page_off;

	target_vma_entry = vma_entry_offset(vma_table, address);
	if (!target_vma_entry) {
		printf("%s: cannot find vma for address(%lx)\n", __func__,address);
		return -1;
	}

	base = get_base_bitmap_entry(target_vma_entry, bm_table);
	if (!base) {
		printf("%s: get_base_bitmap_entry == NULL\n", __func__);
		return -1;
	}

	bm_entry_index = bitmap_entry_index(target_vma_entry, address);
#if DEBUG
	printf("%s: bitmap_table[base+%lu]\n", __func__, bm_entry_index);
#endif
	target_bitmap_entry = base + bm_entry_index;
	target_bitmap_address = target_vma_entry->vma_start
		+ (bm_entry_index * BATCH_UNIT_IN_BYTES);
	page_off = page_offset(target_bitmap_entry, target_bitmap_address, address);
	return dump_offset(page_off, address);
}

static inline int is_vma_stored(struct vma_info *vma)
{
	return vma->stored;
}

static struct vma_info *find_vma(struct address_space_table *table, 
		unsigned long address)
{
	unsigned long cursor;
	if (vma_cache) {
		if (vma_cache->start <= address && vma_cache->end > address)
			return vma_cache;
	}

retry:
	cursor = 0;
	for (;cursor < table->nr_entries; cursor++) {
		if (table->table[cursor].end > address && 
				table->table[cursor].start <= address) {
			vma_cache = &table->table[cursor];
			return vma_cache;
		}
#if 0
		if (table[cursor].start <= address
				&& table[cursor].end > address) {
			return &table[cursor];
		} else if (table[cursor].start > address) {
			return NULL;
		}
#endif
	}

	if (load_vma_info_into_table(table))
		return NULL;
	else {
		printf("%s: reload vma info into table\n", __func__);
		goto retry;
	}
	return NULL;
}

size_t pingpong = 0;
#ifdef THREAD1
size_t size_sum = 0;
#endif
//char xxx[4096] = {255,};

static int mdc_fwrite_for_chkpointing_reference(const void *buf, 
		size_t size, size_t count, FILE *fp)
{
	struct vma_info *target_vma;
	struct address_space_table *as_table;

	// cgmin doesn't access the buf
#if ENABLE_MALLOC_GROUP
	//always write value itself
	size_t ret = fwrite(buf, size, count, fp);
	if (ret != count)
		return -1;
	return 0;

#else
	// check if it is too small
	if (size < 128) {//1024) { //cgmin //cgmin VAL always // now
		size_t ret = fwrite(buf, size, count, fp);
		if (ret != count)
			return -1;
		return 0;
	}
#endif
	//otherwise, write ref and request page dump?
	as_table = &global_trx.trx_data.as_table;

	target_vma = find_vma(as_table, (unsigned long)buf);
	if (!target_vma) {
		printf("cannot find vma for %p\n", buf);
		return -1;
	}
	if (!is_vma_stored(target_vma))
	{
		target_vma->stored = 1;
		//printf("vma %lx %lx\n",target_vma->start,target_vma->end);
		//		perform_memory_dump_for_vma(&global_trx.trx_data, target_vma,0);
		new_vma = 1;
	}
#ifdef THREAD1
	//		size_sum+=size;
	size_sum++;
	while (size_sum >= 4)//096) //cgmin
	{
		//			write(global_trx.trx_data.dump_fd,xxx,4096);
		//			perform_memory_dump_for_vma_partial(&global_trx.trx_data,1,0);
		perform_memory_dump_for_vma_partial(&global_trx.trx_data,1,1);

		size_sum-=4;//096;
	}
#endif

#if 0
	struct timespec start, end;
	long e_time;
	if ((pingpong++ % 4) < 2) {
		clock_gettime(CLOCK_MONOTONIC, &start);
		if (__mdc_fwrite_for_chkpointing_reference(buf, size, count, fp))
			return -1;
		clock_gettime(CLOCK_MONOTONIC, &end);
		e_time = ((end.tv_sec-start.tv_sec)*1000000000 +
				end.tv_nsec-start.tv_nsec);
		printf("mdc_fwrite ref took %ld ns\n", e_time);
	} else {
		clock_gettime(CLOCK_MONOTONIC, &start);
		size_t ret = fwrite(buf, size, count, fp);
		if (ret != count)
			return -1;
		clock_gettime(CLOCK_MONOTONIC, &end);
		e_time = ((end.tv_sec-start.tv_sec)*1000000000 +
				end.tv_nsec-start.tv_nsec);
		printf("mdc_fwrite val took %ld ns\n", e_time);
	}
#else
	if (__mdc_fwrite_for_chkpointing_reference(buf, size, count, fp))
		return -1;
#if 0 // moved to check_end
	//cgmin size_sum
	//bring code from zmalloc_size
	//is it ok???

	unsigned long start_addr,end_addr;
	size_t chunk_size,cs1,cs2;
	int index;

	//chunk_size = malloc_size(buf); // zmalloc_size???
	chunk_size = *(size_t*)((char*)buf-sizeof(size_t)); // ???
	chunk_size = chunk_size-chunk_size%8;


	if (chunk_size > 4096)
	{
		printf("cs big\n");
		return 0;
	}
	//start_addr = (char*)buf-sizeof(size_t); // ??? what if it is not front of chunk
	start_addr = (char*)buf-sizeof(size_t)*2;
	end_addr = start_addr+chunk_size;
	index = (start_addr-target_vma->start)/4096;
	//printf("cs %d %d",chunk_size,index);
	//printf("%p %p\n",buf,(void*)(target_vma->start+index*4096));
	if (start_addr/4096 == end_addr/4096)
	{
		target_vma->size_sum[index]+=chunk_size;
		check_and_free(target_vma,index);
	}
	else
	{
		cs1 = 4096-start_addr%4096;
		cs2 = chunk_size-cs1;
		target_vma->size_sum[index]+=cs1;
		check_and_free(target_vma,index);

		index++;
		target_vma->size_sum[index]+=cs2;
		check_and_free(target_vma,index);


	}
#endif
#endif
	return 0;
	}

#if 0
	static size_t copy_objects_from_dump_to_file(void *buf, size_t size, 
			size_t count, unsigned long *add
#endif

			static int mdc_fwrite_for_chkpointing_value(const void *buf, 
				size_t size, size_t count, FILE *fp)
			{
#if 0
			unsigned long address = (unsigned long) buf;
			struct transactional_data *trx_data = &global_trx.trx_data;
			struct address_space_table *as_table = &trx_data->as_table;
			struct vma_info *target_vma = find_vma(as_table, (unsigned long)buf);
			if (!target_vma) {
			printf("cannot find vma for %p\n", buf);
			return 0;
			}
			if (is_vma_stored(target_vma)) {
			ssize_t result;
			int fd_in = trx_data->dump_fd;
			int fd_out = fileno(fp);
			printf("%s: vma is stored %p\n", __func__, buf);
			loff_t off_in = get_object_file_offset_in_dump(address, trx_data);
			fflush(fp);
			loff_t off_out = lseek(fd_out, 0, SEEK_CUR);
			printf("%s: [BEFORE] fd_in offset %lx, fd_out offset %lx, size %lx\n", 
					__func__, off_in, off_out, size * count);

			result = sendfile(fd_out, fd_in, &off_in, size * count);

			off_out = lseek(fd_out, 0, SEEK_CUR);
			printf("%s: [AFTER] fd_in offset %lx, fd_out offset %lx, size %lx\n", 
					__func__, off_in, off_out, size * count);

			//result = copy_file_range(fd_in, &off_in, fd_out, &off_out, size * count, 0);
			if (result == -1) {
				printf("%s: copy_file_range failed errno %d, fd_in size %lx\n", 
						__func__, errno, lseek(fd_in, 0, SEEK_END));
				return 0;
			}
			return count;
			}
#endif
			return fwrite(buf, size, count, fp);
			}

	size_t mdc_fwrite(const void *buf, size_t size, size_t count, 
			FILE *fp, int type)
	{
		size_t result = 0;
		size_t ret = 0;
		//printf(">> fwrite(buf:%p, size:%lu, count:%lu, fp:%p, type:%s)\n", 
		//		buf, size, count, fp, type? "CHKPOINT_REF":"CHKPOINT_VAL");
		if (!buf)
			return 0;

#ifdef CGMIN_DEBUG
		printf("buf %p %lu\n",buf,size); //cgmin test
#endif

		switch (type) {
			case CHKPOINT_VAL:
#ifdef CGMIN_DEBUG
				printf("VAL %p %lu\n",buf,size); //cgmin test
#endif
				//printf("VAL %p %lu\n",buf,size);

				result = mdc_fwrite_for_chkpointing_value(buf, size, count, fp);
				break;
			case CHKPOINT_REF:
#ifdef CGMIN_DEBUG
				printf("REF %p %lu\n",buf,size); //cgmin test
#endif
//				printf("REF %p %lu\n",buf,size); //cgmin test

				ret = mdc_fwrite_for_chkpointing_reference(buf, size, count, fp);

				if (!ret)
					result = count;
				break;
			default:
				printf("type %d\n", type);
				break;
		}
		//printf("%s: fp offset %lx\n", __func__, ftell(fp));
		return result;
	}

	void finalize_transaction(void)
	{
		pthread_mutex_unlock(&trx_mutex);
	}

	static void free_bitmap_table(struct bitmap_table *bm_table)
	{
		if (bm_table->table)
			free(bm_table->table);
	}

	static void free_vma_table(struct vma_table *vma_table)
	{
		if (vma_table->table)
			free(vma_table->table);
	}

	static void free_address_space_table(struct address_space_table *table)
	{
		if (table->table)
		{
			//cgmin test
			int i,j;

			for (j=0;j<table->nr_entries;j++)
			{
#if 0
				for(i=0;i<(table->table[j].end-table->table[j].start)/4096;i++)
					printf("%d ",table->table[j].size_sum[i]);
				printf("\n");
#endif
				free(table->table[j].size_sum); //cgmin size_sum
				free(table->table[j].size_cnt);
				free(table->table[j].dumped2);
			}
			free(table->table);
		}

	}

	void __close_files(struct transactional_data *trx_data)
	{
		printf("%s: fsync for dump files\n", __func__);
		if (trx_data->vma_table_fd >= 0) {
			fsync(trx_data->vma_table_fd);
			close(trx_data->vma_table_fd);
		}
		if (trx_data->bitmap_table_fd >= 0) {
			fsync(trx_data->bitmap_table_fd);
			close(trx_data->bitmap_table_fd);
		}
		if (trx_data->dump_fd >= 0) {
			fsync(trx_data->dump_fd);
			close(trx_data->dump_fd);
		}
	}

	static inline void close_files_for_checkpointing(
			struct transactional_data *trx_data)
	{
		__close_files(trx_data);
	}

	void cleanup_for_checkpointing(struct transactional_data *trx_data)
	{
		close_files_for_checkpointing(trx_data);
		free_address_space_table(&trx_data->as_table);
		free_vma_table(&trx_data->vma_table);
		free_bitmap_table(&trx_data->bm_table);
	}

	static int __perform_memory_dump_for_each_vma(struct transactional_data *trx_data, int free_after_write)
	{
		struct vma_info *target_vma;
		struct address_space_table *as_table = &trx_data->as_table;
		size_t i;
		for (i = 0; i < as_table->nr_entries; i++) {
			target_vma = &as_table->table[i];
			if (is_vma_stored(target_vma))
				if (perform_memory_dump_for_vma(trx_data, target_vma, free_after_write))
					return -1;
		}
		return 0;
	}

	static inline int perform_memory_dump_for_each_vma(struct transactional_data *trx_data)
	{
		return __perform_memory_dump_for_each_vma(trx_data, 0); // only dump not free
	}

	static inline int perform_memory_dump_for_each_vma_and_free(struct transactional_data *trx_data)
	{
		return __perform_memory_dump_for_each_vma(trx_data, 1); // dump and free
	}

	int rename_file_with_suffix(char *tmpfile, char *newfile, char *suffix)
	{
		char tmpfile_name[256];
		char newfile_name[256];
		create_new_name_with_suffix(tmpfile_name,256,tmpfile, suffix);
		create_new_name_with_suffix(newfile_name,256,newfile, suffix);
		return rename(tmpfile_name, newfile_name);
	}

	int rename_files(char *tmpfile, char *newfile)
	{
		if (rename_file_with_suffix(tmpfile, newfile, "vma"))
			return -1;
		if (rename_file_with_suffix(tmpfile, newfile, "bitmap"))
			return -1;
		if (rename_file_with_suffix(tmpfile, newfile, "dump"))
			return -1;
		return 0;
	}

	int checkpoint_end(char *name, int free_after_write)
	{
		char *tmpfile = global_trx.name;
		char *newfile = strdup(name);
		struct transactional_data *trx_data = &global_trx.trx_data;
		printf("madvise mc %d nmc %d\n",mc,nmc);
		/*
		   if (free_after_write) {
		   if (perform_memory_dump_for_each_vma_and_free(trx_data)) {
		   printf("%s: memory dump and free failed\n",__func__);
		   return -1;
		   }
		   } else {
		   if (perform_memory_dump_for_each_vma(trx_data)) {
		   printf("%s: memory dump failed\n",__func__);
		   return -1;
		   }
		   }
		 */
#ifdef THREAD1
		//	perform_memory_dump_for_vma_partial(trx_data,0,0);//cgmin
		perform_memory_dump_for_vma_partial(trx_data,0,1);//cgmin
#endif

#ifdef THREAD2
		dump_exit=1;
		pthread_join(dump_thread,NULL);
#endif

		cleanup_for_checkpointing(trx_data);
		finalize_transaction();
		if (rename_files(tmpfile,newfile)) {
			printf("%s: failed tmpfile %s, newfile %s\n",__func__,tmpfile,newfile);
			unlink(tmpfile);
			return -1;
		}
		free(newfile);
		//printf("\n>> checkpoint_end()\n");

		printf("dump %ld\nresidency %ld\nbitmap %ld bitmap2 %ld\n",dump_time,residency_time,bitmap_time,bitmap2_time);

		return 0;
	}

#if 0
	static int __rewind(FILE *fp)
	{
		off_t temp_offset;
		temp_offset = lseek(vma_table_fd, 0, SEEK_CUR);
		if (temp_offset !=0) {
			printf("%s: reposition of file offset is required\n", __func__);
			if (lseek(vma_table_fd, 0, SEEK_SET) != 0)
				return -1;
		}
		return 0;
	}
#endif

	static int __load_objects_into_table_from_file(int fd, void *table,
			size_t object_size,	size_t nr_objects)
	{
		size_t bytes;
		size_t size = nr_objects * object_size;
		bytes = read(fd, table, size);
		if (bytes != size)
			return -1;
		return 0;
	}

	static int load_vma_entries_into_table_from_file(struct vma_entry *table,
			size_t nr_entries, struct transactional_data *trx_data)
	{
		return __load_objects_into_table_from_file(trx_data->vma_table_fd,
				table, sizeof(struct vma_entry), nr_entries);
	}

	static size_t get_nr_objects(int fd, size_t object_size)
	{
		struct stat sb;
		fstat(fd, &sb);
		if (sb.st_size % object_size) {
			printf("%s: size %lu is not divided by %lu\n", 
					__func__, sb.st_size, object_size);
			return 0;
		}
		return sb.st_size / object_size;
	}


	static int load_bitmap_entries_into_table_from_file(struct bitmap_entry *table,
			size_t nr_entries, struct transactional_data *trx_data)
	{

		return __load_objects_into_table_from_file(trx_data->bitmap_table_fd,
				table, sizeof(struct bitmap_entry), nr_entries);
	}


	static size_t get_nr_bitmap_entries(struct transactional_data *trx_data)
	{
		return get_nr_objects(trx_data->bitmap_table_fd, sizeof(struct bitmap_entry));
	}

	static int load_bitmap_entries_into_table(struct transactional_data *trx_data,
			struct bitmap_table *bm_table)
	{
		size_t nr_entries;
		struct bitmap_entry *table;

		nr_entries = get_nr_bitmap_entries(trx_data);
		if (!nr_entries)
			return -1;

		table = allocate_bitmap_table(nr_entries);
		if (!table)
			return -1;

		if (load_bitmap_entries_into_table_from_file(table, nr_entries, trx_data))
			goto error_free_bitmap_table;

		bm_table->table = table;
		bm_table->nr_entries = nr_entries;
		bm_table->pos = nr_entries;

		printf("%s: nr bitmap entries %lu\n", __func__, nr_entries);

#if DEBUG
		print_bitmap_table(bm_table);
#endif
		return 0;
error_free_bitmap_table:
		free(table);
		return -1;
	}


	static size_t get_nr_vma_entries(struct transactional_data *trx_data)
	{
		return get_nr_objects(trx_data->vma_table_fd, 
				sizeof(struct vma_entry));
	}

	static int load_vma_entries_into_table(struct transactional_data *trx_data,
			struct vma_table *vma_table)
	{
		size_t nr_entries;
		struct vma_entry *table;

		nr_entries = get_nr_vma_entries(trx_data);
		if (!nr_entries)
			return -1;

		table = allocate_vma_table(nr_entries);
		if (!table)
			return -1;

		if (load_vma_entries_into_table_from_file(table, nr_entries, trx_data))
			goto error_free_vma_table;

		vma_table->table = table;
		vma_table->nr_entries = nr_entries;
		vma_table->pos = nr_entries;

		printf("%s: nr vma entries %lu\n", __func__, nr_entries);
#if DEBUG | DEBUG2
		print_vma_table(vma_table);
#endif
		return 0;
error_free_vma_table:
		free(table);
		return -1;
	}

	static int open_files_for_restoring(struct transactional_data *trx_data,
			char *name)
	{
		return __open_files(trx_data, name, 0);
	}

	static void *readahead(void *arg)
	{
		int ret;
		struct transactional_data *trx_data = arg;

		size_t bytes_to_read = trx_data->dump_mmap_size;
		size_t offset = 0;

		while (bytes_to_read > 0) {
			size_t size = MIN(bytes_to_read, BATCH_UNIT_IN_BYTES*8);
			ret = posix_fadvise(trx_data->dump_fd, offset, size, POSIX_FADV_WILLNEED);
			if (ret)
				printf("%s: readahead failed %d\n", __func__, ret);
			offset += size;
			bytes_to_read -= size;
		}

		printf("%s: readahead done\n", __func__);
		return NULL;
	}

	static int create_readahead_thread(struct transactional_data *trx_data)
	{
		return pthread_create(&trx_data->ra_thd, NULL, readahead, (void *)trx_data);
	}

	static size_t get_file_size(int fd)
	{
		struct stat sb;
		fstat(fd, &sb);
		return (sb.st_size + PAGE_SIZE-1) & PAGE_MASK;
	}

	static int prepare_for_restoring(struct transaction *trx)
	{
		char *name = trx->name;
		struct transactional_data *trx_data = &trx->trx_data;
		if (open_files_for_restoring(trx_data, name))
			return -1;

		trx_data->dump_mmap_size = get_file_size(trx_data->dump_fd);

#if 0
		if (create_readahead_thread(trx_data))
			printf("%s: create_readahead_thread failed\n", __func__);
#endif

		if (load_vma_entries_into_table(trx_data, &trx_data->vma_table))
			return -1;
		if (load_bitmap_entries_into_table(trx_data, &trx_data->bm_table))
			return -1;

		trx_data->dump_mmap_addr = mmap(NULL, trx_data->dump_mmap_size, PROT_READ,
				//MAP_SHARED | MAP_FILE, trx_data->dump_fd, 0);
				//				MAP_SHARED | MAP_FILE | MAP_POPULATE, trx_data->dump_fd, 0);
			MAP_PRIVATE | MAP_FILE | MAP_POPULATE, trx_data->dump_fd, 0);
#if 0
		//cgmin
		global_max_page = trx_data->dump_mmap_size/4096+1;
		//printf("gmp %d\n",global_max_page);
		global_num_in_page = (int*)malloc(sizeof(int)*global_max_page);
		int i;
		for (i=0;i<MAX_IN_PAGE;i++)
		{
			global_ref_file_position[i] = (unsigned long*)malloc(sizeof(unsigned long)*global_max_page);
			global_ref_dbid[i] = (unsigned long*)malloc(sizeof(unsigned long)*global_max_page);

		}
		//printf("cgmin rsetore init\n");
#endif
		return 0;
	}

	int restore_start(char *name)
	{
		struct transaction *trx = &global_trx;

#if DEBUG_TIME2
		vs_time = 0;//cgmin
		ttt1 = ttt2 = ttt3 = ttt4 = ttt5 = 0;
		ttt6 = ttt7 = 0;
#endif

#if DEBUG
		printf("\n>> restore_start(%s)\n", name);
#endif
#if DEBUG_TIME
		total_time = 0;
		_fread_original_time = 0;
		_fread_time = 0;
		_object_time = 0;
		__file_offset_time = 0;
		__pread_time = 0;
#endif
		initialize_transaction(trx, name);
		return prepare_for_restoring(trx);
	}



	static size_t get_object_from_dump(void *buf, size_t size,
			unsigned long address, struct transactional_data *trx_data)
	{
#if DEBUG_TIME
		struct timespec start, mid, mid2, end;
		clock_gettime(CLOCK_MONOTONIC, &start);
#endif
		unsigned long dump_off = get_object_file_offset_in_dump(address, trx_data);
#if DEBUG_TIME
		clock_gettime(CLOCK_MONOTONIC, &mid);
		__file_offset_time += get_time_difference_ns(&mid, &start);
#endif
#if 0

		if (global_sort) // cgmin ref sort
		{
			//printf("x1\n");
			global_REF = 1;

			int page = dump_off/4096;
			int index = global_num_in_page[page];
			//reversed array
			/*
			   if (page >= global_max_page)
			   printf("eee2\n");
			   if (index >= 32)
			   printf("eee");
			 */
			global_ref_file_position/*[global_page]*/[index][page] = global_file_position;
			global_ref_dbid/*[global_page]*/[index][page] = global_dbid;

			//debug
			/*
			   if (index == 0 && page == 0)
			   {
			   printf("0 0 %lu %d\n",global_file_position,global_dbid);

			   }
			 */
			global_num_in_page[page]++;
			//printf("x2\n");
			return size;
		}
#endif

#if DEBUG
		printf("buf %p, size %lx, offset %lx\n", buf, size, dump_off);
#endif
		if (dump_off == -1)
			return 0;
#if DEBUG_TIME
		clock_gettime(CLOCK_MONOTONIC, &mid2);
		//size_t ret = pread(trx_data->dump_fd, buf, size, dump_off);
		memcpy(buf, (char *)trx_data->dump_mmap_addr + dump_off, size);
		clock_gettime(CLOCK_MONOTONIC, &end);
		__pread_time += get_time_difference_ns(&end, &mid2);
		return size;
		//	return ret;
#else
		memcpy(buf, (char *)trx_data->dump_mmap_addr + dump_off, size);
		return size;
		//int rv;
		//	rv = pread(trx_data->dump_fd, buf, size, dump_off);
		//printf("rv %d\n",rv);
		//if (rv == size)
		//	return rv;
		//return 0;
#endif
	}

	static size_t get_objects_from_dump(void *buf, size_t size, size_t count, 
			unsigned long *address_table, struct transactional_data *trx_data)
	{
		size_t i = 0;
		size_t bytes = 0;
		char *buf_cursor = buf;

		for (; i < count; i++) {
			bytes += get_object_from_dump((void *)buf_cursor, 
					size, address_table[i], trx_data);
			buf_cursor += size;
		}
		if (bytes != (size * count))
			return -1;
		return 0;
	}

	static void release_address_table(struct address_table *table)
	{
		if (table->table)
			free(table->table);
	}

#if DEBUG
	static void print_address_table(struct address_table *table)
	{
		size_t idx;
		printf("\n<Address Table>\naddress\n");
		for (idx = 0; idx < table->nr_entries; idx++)
			printf("[%lu] %lx\n", idx, table->table[idx]);
		printf("\n");
	}
#endif

	static int load_address_into_table_from_file(
			struct address_table *addr_table, FILE *fp)
	{
		size_t bytes = fread(addr_table->table, POINTER_SIZE_IN_BYTES, 
				addr_table->nr_entries, fp);	
		if (bytes != addr_table->nr_entries) {
			printf("%s: bytes %lu != %lu * %lu\n", __func__, bytes, POINTER_SIZE_IN_BYTES, addr_table->nr_entries);
			return -1;
		}
		return 0;
	}

	static int initialize_address_table(
			struct address_table *table, size_t count)
	{
		table->table = calloc(count, sizeof(unsigned long));
		if (!table->table)
			return -1;
		table->nr_entries = count;
		return 0;
	}

	static int mdc_fread_for_restoring_reference(void *buf,
			size_t size, size_t count, FILE *fp)
	{
		struct transactional_data *trx_data = &global_trx.trx_data;
		if (count > 1) {
			struct address_table table;
			printf("cgmin count\n");
#if ENABLE_MALLOC_GROUP
			size_t ret = fread(buf, size, count, fp);
			if (ret != count)
				return -1;
			return 0;
#else
			if (size < 128) {//1024) { //cgmin //cgmin VAL always // cgmin now
				size_t ret = fread(buf, size, count, fp);
				if (ret != count)
					return -1;
				return 0;
			}
#endif

#if DEBUG
			printf(">> fread(buf:%p, size:%lu, count:%lu, fp:%p, type:%s)\n", 
					buf, size, count, fp, "CHKPOINT_REF");
#endif
			if (initialize_address_table(&table, count))
				return -1;

			if (load_address_into_table_from_file(&table, fp))
				return -1;

#if DEBUG
			print_address_table(&table);
#endif
			get_objects_from_dump(buf, size, count, table.table, trx_data);

			release_address_table(&table);
			return 0;
			} else {
#if DEBUG_TIME
				struct timespec start, mid, mid2, end;
#endif
#if ENABLE_MALLOC_GROUP
				{ //< 1024) { //cgmin //cgmin VAL now
#else
				if (size < 128) {
#endif
#if DEBUG_TIME
					clock_gettime(CLOCK_MONOTONIC, &start);
#endif
					size_t ret = fread(buf, size, count, fp);
					if (ret != count)
						return -1;
#if DEBUG_TIME
					clock_gettime(CLOCK_MONOTONIC, &end);
					_fread_original_time += get_time_difference_ns(&end, &start);
					total_time += get_time_difference_ns(&end, &start);
#endif
					return 0;
				}
#if DEBUG_TIME
				clock_gettime(CLOCK_MONOTONIC, &start);
#endif
				size_t address;
				size_t ret = fread(&address, POINTER_SIZE_IN_BYTES,1,fp);
				if (ret != 1) {
					return -1;
				}
#if DEBUG_TIME
				clock_gettime(CLOCK_MONOTONIC, &mid);
				_fread_time += get_time_difference_ns(&mid, &start);
				clock_gettime(CLOCK_MONOTONIC, &mid2);
#endif

				size_t bytes = get_object_from_dump(buf, size, address, trx_data);
#if DEBUG_TIME
				clock_gettime(CLOCK_MONOTONIC, &end);
#endif
				if (bytes != (size * count)) //cgmin size
					return -1;
#if DEBUG_TIME
				_object_time += get_time_difference_ns(&end, &mid2);
				total_time += get_time_difference_ns(&end, &start);
#endif
				return 0;
				}
			}

			static size_t mdc_fread_for_restoring_value(void *buf,
					size_t size, size_t count, FILE *fp)
			{
#if DEBUG
				printf(">> fread(buf:%p, size:%lu, count:%lu, fp:%p, type:%s)\n", 
						buf, size, count, fp, "CHKPOINT_VAL");
#endif
				return fread(buf, size, count, fp);
			}

			size_t mdc_fread(void *buf, size_t size, size_t count, FILE *fp, int type)
			{
				size_t result = 0;
				size_t ret = 0;
				switch (type) {
					case CHKPOINT_VAL:
						result = mdc_fread_for_restoring_value(buf, size, count, fp);
						break;
					case CHKPOINT_REF:
						ret = mdc_fread_for_restoring_reference(buf, size, count, fp);

						if (!ret)
							result = count;
						break;
					default:
						break;
				}
				return result;
			}



			static inline void close_files_for_restoring(
					struct transactional_data *trx_data)
			{
				__close_files(trx_data);
			}

			void restore_end(void)
			{

				//cgmin
#if 0
				free(global_num_in_page);
				int i;
				for (i=0;i<MAX_IN_PAGE;i++)
				{
					free(global_ref_file_position[i]);
					free(global_ref_dbid[i]);
				}
#endif
				struct transactional_data *trx_data = &global_trx.trx_data;
				close_files_for_restoring(trx_data);
				munmap(trx_data->dump_mmap_addr, trx_data->dump_mmap_size);
				free_vma_table(&trx_data->vma_table);
				free_bitmap_table(&trx_data->bm_table);
				finalize_transaction();
#if DEBUG_TIME
				/*
				   printf("Total read time: %ld sec\n"
				   "-- fread original time: %ld sec\n"
				   "-- fread time: %ld sec\n"
				   "-- object time: %ld sec\n"
				   "---- file offset time: %ld sec\n"
				   "---- pread time: %ld sec\n",
				   total_time/1000000000,
				   _fread_original_time/1000000000,
				   _fread_time/1000000000,
				   _object_time/1000000000,
				   __file_offset_time/1000000000,
				   __pread_time/1000000000);
				 */
				printf("Total read time: %ld sec\n"
						"-- fread original time: %ld sec\n"
						"-- fread time: %ld sec\n"
						"-- object time: %ld sec\n"
						"---- file offset time: %ld sec\n"
						"---- pread time: %ld sec\n",
						total_time,
						_fread_original_time,
						_fread_time,
						_object_time,
						__file_offset_time,
						__pread_time);

#endif

#if DEBUG_TIME2
				printf("vs_time %ld\n",vs_time); //cgmin
				printf("key %ld val %ld dbadd %ld\n",ttt1,ttt2,ttt3);
				printf("hase zip %ld zset2 %ld\n",ttt4,ttt5);
				printf("zmalloc %ld rioRead %ld\n",ttt6,ttt7);
#endif

				//printf("\n>> restore_end()\n");
			}

			struct vma_info *global_target_vma = NULL;
			unsigned long global_page_addr;
			unsigned long global_end_addr;
			unsigned char global_residency_vec[BATCH_UNIT_IN_PAGES];
			unsigned long global_dump_addr;
			int global_residency_index;
			int global_nr_pages;

			static int perform_memory_dump_for_vma_partial(struct transactional_data *trx_data,
					int partial, int free_after_write) //cgmin
			{
				unsigned long page_addr;
				struct timespec ts1,ts2;
				struct vma_info *target_vma;
				if (new_vma == 0)
					return 0;
retry:
				if (global_target_vma == NULL)
				{
					struct address_space_table *table=&trx_data->as_table;
					for (int cursor=0;cursor < table->nr_entries;cursor++)
					{
						if (table->table[cursor].stored == 1 && table->table[cursor].dumped == 0)
						{
							global_target_vma = &table->table[cursor];
							table->table[cursor].dumped = 1;
							if (partial)
								break;
							else
								perform_memory_dump_for_vma(trx_data,global_target_vma,free_after_write);
						}
					}
					if (partial == 0)
						return 0;
					if ( global_target_vma == NULL)
					{
						new_vma = 0;
						//printf("global target vma failed\n");
						return 1; // fail?
					}
					target_vma = global_target_vma;
					global_page_addr = target_vma->start;
					global_end_addr = target_vma->start; //global_page_addr + BATCH_UNIT_IN_BYTES; 

					//initial target vma
					if (save_vma_info(trx_data, target_vma)) {
						printf("%s: save_vma_info failed\n", __func__);
						return -1;
					}

				}
				target_vma = global_target_vma;
				page_addr = global_page_addr;

				if (page_addr >= global_end_addr) // per batch
				{
					if (page_addr >= global_target_vma->end) // end of vma
					{
						global_target_vma = NULL;
						//			return 2; //retry
						goto retry;
					}
					//		unsigned char residency_vec[BATCH_UNIT_IN_PAGES];
					unsigned char *residency_vec = &global_residency_vec;
					unsigned long end_addr;
					size_t nr_pages;
					size_t size;
					struct bitmap_entry *be = get_free_bitmap_entry(&trx_data->bm_table);
					end_addr = get_end_address_in_current_batch(page_addr, target_vma->end);
					size = end_addr - page_addr;
					nr_pages = size >> PAGE_SHIFT;

					global_residency_index=0;
					global_end_addr = end_addr;
					global_nr_pages = nr_pages;


					clock_gettime(CLOCK_MONOTONIC,&ts1);
					if (get_page_residency(residency_vec, page_addr, nr_pages)) {
						printf("%s: get_page_residency failed\n", __func__);
						return -1;
					}
					clock_gettime(CLOCK_MONOTONIC,&ts2);
					residency_time+=(ts2.tv_sec-ts1.tv_sec)*1000000000+ts2.tv_nsec-ts1.tv_nsec;
					clock_gettime(CLOCK_MONOTONIC,&ts1);
					set_bitmap_entry_for_batch(trx_data, be, residency_vec, nr_pages);
					clock_gettime(CLOCK_MONOTONIC,&ts2);
					bitmap_time+=(ts2.tv_sec-ts1.tv_sec)*1000000000+ts2.tv_nsec-ts1.tv_nsec;
#if MADVISE_UNIT_TYPE == MADV_DUMP_UNIT
#if 0
					if (mlock((void *)page_addr, size)) {
						printf("mlock failed %d addr:%lx size:%lx\n", 
								errno, page_addr, size);
						return -1;
					}
#endif
#endif
					/*
					   clock_gettime(CLOCK_MONOTONIC,&ts1);
					   if (__perform_memory_dump_in_batch(trx_data, page_addr, end_addr, residency_vec)) {
					   printf("%s: __perform_memory_dump_in_batch failed\n", __func__);
					   return -1;
					   }
					   clock_gettime(CLOCK_MONOTONIC,&ts2);
					   dump_time+=(ts2.tv_sec-ts1.tv_sec)*1000000000+ts2.tv_nsec-ts1.tv_nsec;
					 */

#if MADVISE_UNIT_TYPE == MADV_DUMP_UNIT
#if 0
					if (munlock((void *)page_addr, size)) {
						printf("mulock failed %d addr:%lx size:%lx\n", 
								errno, page_addr, size);
					}
#endif
					//cgmin may not use
					/*
					   if (free_after_write) {
					   if (madvise((void *)page_addr, size, MADV_DONTNEED2)) {
					   printf("madvise with dump unit failed %d\n", errno);
					   return -1;
					   }
					   }
					 */
#endif
					clock_gettime(CLOCK_MONOTONIC,&ts1);

					if (save_bitmap_table(trx_data, be)) {
						printf("%s: save_bitmap_table failed\n", __func__);
						return -1;
					}
					clock_gettime(CLOCK_MONOTONIC,&ts2);
					bitmap2_time+=(ts2.tv_sec-ts1.tv_sec)*1000000000+ts2.tv_nsec-ts1.tv_nsec;

#if DEBUG
					printf("bitmap %lx, end_addr %lx\n", be->bitmap, end_addr);
#endif
				}

				//cgmin may not use
				/*
#if MADVISE_UNIT_TYPE == MADV_VMA_UNIT
if (free_after_write) {
if (madvise((void *)target_vma->start, target_vma->end - target_vma->start, 
MADV_DONTNEED2)) {
printf("madvise with vma unit failed %d\n", errno);
return -1;
}
}
#endif
#if DEBUG
print_vma_table(&trx_data->vma_table);
//print_bitmap_table(&trx_data->bm_table);
#endif
				 */

//page dump here
clock_gettime(CLOCK_MONOTONIC,&ts1);
/*
   if (__perform_memory_dump_in_batch(trx_data, page_addr, end_addr, residency_vec)) {
   printf("%s: __perform_memory_dump_in_batch failed\n", __func__);
   return -1;
   }
 */
while(global_residency_index < global_nr_pages && global_residency_vec[global_residency_index]==0)
{
	++global_residency_index;
	global_page_addr+=PAGE_SIZE;
}
//		if (residency_vec[i]) {
if (global_residency_index < global_nr_pages)
{
	size_t bytes = write(trx_data->dump_fd, (void *)global_page_addr, PAGE_SIZE);
	if (bytes != PAGE_SIZE)
		return -1;
	//#if MADVISE_UNIT_TYPE == MADV_PAGE_UNIT
	if (free_after_write)
	{
		//printf("madvise %p\n",global_page_addr);
		if (madvise((void *)global_page_addr, PAGE_SIZE, MADV_DONTNEED2)) {
			printf("madvise with page unit failed %d\n", errno);
			return -1;
		}
	}
	//#endif

	//printf("memory dump addr %lx\n", addr);
}
else
goto retry;
//			return 2; //retry

/* else {
   nr_non_present_pages++;
   }*/
global_page_addr += PAGE_SIZE;
++global_residency_index;

clock_gettime(CLOCK_MONOTONIC,&ts2);
dump_time+=(ts2.tv_sec-ts1.tv_sec)*1000000000+ts2.tv_nsec-ts1.tv_nsec;

if (partial == 0)
	goto retry;

	return 0;
	}


#ifdef THREAD2
void *dump_function()
{
	printf("dump start\n");
	while(!dump_exit || new_vma == 1)
	{
		struct transactional_data *trx_data = &global_trx.trx_data;
		struct address_space_table *table=&trx_data->as_table;
		int free_after_write=1;
		for (int cursor=0;cursor < table->nr_entries;cursor++)
		{
			if (table->table[cursor].stored == 1 && table->table[cursor].dumped == 0)
			{
				table->table[cursor].dumped = 1;
				perform_memory_dump_for_vma(trx_data,&table->table[cursor],free_after_write);
			}
		}
		new_vma = 0;
		sleep(1);
	}
	printf("dump end\n");
}
#endif
void check_end(void* buf)
{
	if (buf == 0)
	{
		printf("buf error\n");
		return;
	}
	//int no_free=0;
	/*
	   if (check_group(buf) == 0)
	   {
	//printf("cge\n");
	//no_free = 1;
	nmc++;
	return;
	}
	 */

	struct transactional_data *trx_data = &global_trx.trx_data;
	struct address_space_table *as_table=&trx_data->as_table;
	struct vma_info* target_vma = find_vma(as_table, (unsigned long)buf);


	unsigned long start_addr,end_addr;
	size_t chunk_size,cs1,cs2;
	int index;

	//chunk_size = malloc_size(buf); // zmalloc_size???
	chunk_size = *(size_t*)((char*)buf-sizeof(size_t)); // ???
	chunk_size = chunk_size-chunk_size%8;

	if (chunk_size > 4096)
	{
		printf("cs big\n");
		return;
	}
	//start_addr = (char*)buf-sizeof(size_t); // ??? what if it is not front of chunk
	start_addr = (unsigned long)((char*)buf-sizeof(size_t)*2); // not good...
	end_addr = start_addr+chunk_size;
	index = (start_addr-target_vma->start)/4096;
	//printf("cs %d %d",chunk_size,index);
	//printf("%p %p\n",buf,(void*)(target_vma->start+index*4096));

	if (start_addr/4096 == (end_addr-1)/4096) // (end_addr-1)
	{
		target_vma->size_cnt[index]+=chunk_size;
		//if (!no_free)
		check_and_free(target_vma,index);
	}
	else
	{
		cs1 = 4096-start_addr%4096;
		cs2 = chunk_size-cs1;
		target_vma->size_cnt[index]+=cs1;
		//if (!no_free)
		check_and_free(target_vma,index);
		index++;
		target_vma->size_cnt[index]+=cs2;
		//if (!no_free)
		check_and_free(target_vma,index);
	}
}
