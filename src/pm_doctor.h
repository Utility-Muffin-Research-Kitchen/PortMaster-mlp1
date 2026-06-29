#ifndef PM_DOCTOR_H
#define PM_DOCTOR_H

#include "pm_context.h"

typedef struct {
    char text[4096];
    int issues;
} pm_doctor_report;

void pm_doctor_run(const pm_context *ctx, pm_doctor_report *report);

#endif /* PM_DOCTOR_H */

