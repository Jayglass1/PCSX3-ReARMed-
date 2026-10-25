/* function-entry ring for hang diagnosis (-finstrument-functions) */
volatile unsigned long ws_ring[16];
volatile unsigned ws_ring_i;
void __attribute__((no_instrument_function)) __cyg_profile_func_enter(void *fn, void *cs)
{
    ws_ring[ws_ring_i++ & 15] = (unsigned long)fn;
}
void __attribute__((no_instrument_function)) __cyg_profile_func_exit(void *fn, void *cs) { }
volatile unsigned long ws_bc[8];
volatile unsigned long ws_step;
volatile int ws_where;
volatile int ws_hcinfo[4];
volatile unsigned long ws_iter, ws_exec;
