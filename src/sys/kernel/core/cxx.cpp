typedef void initfunc_t(void);
extern initfunc_t *__init_array_start[], *__init_array_end[];

extern "C" void init_global_constructors_array(void)
{
  for (initfunc_t **p = __init_array_start; p != __init_array_end; p++)
    (*p)();
}