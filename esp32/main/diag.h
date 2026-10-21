#ifndef NEBULAEDGE_DIAG_H
#define NEBULAEDGE_DIAG_H

/* Identifica el build, el motivo del último reinicio y la memoria al arrancar.
 * Va antes que cualquier otra cosa en app_main. */
void log_boot_banner(void);

/* Informa la memoria libre del heap interno, con `stage` como etiqueta. */
void log_memory(const char *stage);

/* Arranca el reporte periódico de memoria (MEMORY_LOG_PERIOD_MS, en diag.c). */
void start_memory_monitor(void);

#endif
