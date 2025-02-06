#include "malloc_group.h"

int main()
{
	void* ptr = malloc_group(16,0);
	free(ptr);
	return 0;
}
