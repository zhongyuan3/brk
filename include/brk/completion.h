#ifndef _BRK_COMPLETION_H
#define _BRK_COMPLETION_H

#include <brk/completion_types.h>
#include <stdbool.h>

void completion_init(completion_t *comp, const char *name);
void reinit_completion(completion_t *comp);

void wait_for_completion(completion_t *comp);
int wait_for_completion_interruptible(completion_t *comp);

/* May be called from interrupt context. */
void complete(completion_t *comp);
void complete_all(completion_t *comp);

bool completion_done(const completion_t *comp);

#endif
