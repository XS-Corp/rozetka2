#ifndef VALUE
#define VALUE 17
#endif
#ifdef STRONG
#define WEAK
#else
#define WEAK __attribute__((weak))
#endif
WEAK int shared_values[2] = {VALUE, VALUE + 1};
WEAK int shared_answer(void) { return VALUE; }
/* Also marks the strong provider as containing weak definitions. */
__attribute__((weak)) int weak_marker;
static int (*volatile function_pointer)(void) = shared_answer;
static int *volatile interior_pointer = &shared_values[1];
int provider_value(void) { return function_pointer(); }
int provider_data(void) { return *interior_pointer; }
void *provider_address(void) { return (void *)function_pointer; }
