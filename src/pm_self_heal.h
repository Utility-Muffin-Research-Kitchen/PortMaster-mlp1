#ifndef PM_SELF_HEAL_H
#define PM_SELF_HEAL_H

#include "pm_context.h"

int pm_self_heal_leaf_ports_launcher(const pm_context *ctx,
                                     char *detail,
                                     size_t detail_size);
int pm_self_heal_leaf_ports_catalog(const pm_context *ctx,
                                    char *detail,
                                    size_t detail_size);

#endif /* PM_SELF_HEAL_H */
