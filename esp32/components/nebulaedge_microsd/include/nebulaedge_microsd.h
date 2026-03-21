#ifndef NEBULAEDGE_MICROSD
#define NEBULAEDGE_MICROSD

#include <stdio.h>
#include "schema.pb-c.h"

void mount_sd(void);
void unmount_sd(void);
void save_measure_to_sd(Measure *m, FILE *f, char *file_path);
void read_measures_from_sd(char *file_path);

#endif