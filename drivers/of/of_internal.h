#ifndef _OF_INTERNAL_H
#define _OF_INTERNAL_H

/* Raw FDT pointer, only valid while the early memory scans run. */
void *of_early_get_fdt(void);
int of_fdt_errno(int err);

#endif
