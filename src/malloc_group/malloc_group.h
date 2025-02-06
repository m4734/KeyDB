//#pragma once

#include<stddef.h>

//#ifndef MG_DEF
//#define MG_DEF
#if 1
//cgmin
//void *malloc_group(size_t size, size_t group);
void *__zmalloc_group(size_t size, size_t group);

//int get_size_sum(void *mem);
int zget_size_sum(void *mem);

//void* malloc_group_f(size_t size,size_t group);
#else

#include "../../deps/malloc_group/include/malloc.h"
void* __zmalloc_group(size_t size, size_t group)
{
//return NULL;
	return malloc_group(size,group);
}


int zget_size_sum(void *mem)
{
	return get_size_sum(mem);
}


#endif

//#endif
