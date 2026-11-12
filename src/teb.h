#ifndef XBEMU_TEB_H
#define XBEMU_TEB_H

/*
 * Set up the TEB and GS base for the calling thread. Idempotent per thread.
 * Must run on every thread that executes guest code, before the code runs.
 */
void teb_init_current_thread(void);

#endif
