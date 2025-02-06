#include "malloc_group.h"

//#ifndef MG_DEF
//#define MG_DEF

//#include "zmalloc.h"
#include "../../deps/malloc_group/include/malloc.h"

#if 0
//cgmin
int zget_size_sum(void *mem)
{
//return 0;
	return get_size_sum(mem);
}
#endif


#if 1
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
