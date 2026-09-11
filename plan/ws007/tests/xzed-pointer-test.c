#include "userland/X11/xzed/pointer.h"

#include <assert.h>
#include <stdint.h>

int
main(void)
{
	int x = 400;
	int y = 300;

	xzed_pointer_move(&x, &y, 100, 50, 800, 600);
	assert(x == 500 && y == 350);
	xzed_pointer_move(&x, &y, INT32_MAX, INT32_MAX, 800, 600);
	assert(x == 799 && y == 599);
	xzed_pointer_move(&x, &y, INT32_MIN, INT32_MIN, 800, 600);
	assert(x == 0 && y == 0);
	xzed_pointer_move(&x, &y, 37, 29, 800, 600);
	assert(x == 37 && y == 29);
	return 0;
}
