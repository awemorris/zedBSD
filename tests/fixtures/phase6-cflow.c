#define IGNORED(x) x
static int helper(int value)
{
	return value + 1;
}

int main(void)
{
	const char *text = "not_a_call()";
	(void)text;
	return helper(41);
}
