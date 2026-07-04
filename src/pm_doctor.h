#ifndef PM_DOCTOR_H
#define PM_DOCTOR_H

#include "pm_context.h"

#include <stdbool.h>

typedef struct {
    char text[131072];
    int issues;
    int warnings;
} pm_doctor_report;

void pm_doctor_run(const pm_context *ctx, pm_doctor_report *report);
void pm_doctor_run_spec(const pm_context *ctx, pm_doctor_report *report, bool json);

#endif /* PM_DOCTOR_H */
